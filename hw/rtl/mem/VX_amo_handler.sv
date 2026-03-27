// Copyright (c) 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

`include "VX_define.vh"

module VX_amo_handler import VX_gpu_pkg::*; #(
    parameter NUM_LANES  = 1,
    parameter DATA_SIZE  = 4,
    parameter TAG_WIDTH  = 1,
    parameter ADDR_WIDTH = 32
) (
    input wire clk,
    input wire reset,

    VX_lsu_mem_if.slave  core_mem_if,
    VX_lsu_mem_if.master next_mem_if,

    input wire next_pending,

    // Global AMO lock interface
    output wire                                    amo_lock_req,
    output wire [`CLOG2(`AMO_LOCK_BANKS)-1:0]      amo_lock_bank,
    `ifdef LMEM_ENABLE
    input  wire                                    amo_lock_grant,
    output wire                                    amo_local_lock_req,
    input  wire                                    amo_local_lock_grant
    `else
    input  wire                                    amo_lock_grant
    `endif
);

    localparam DATA_WIDTH = DATA_SIZE * 8;
    localparam LANE_BITS  = `CLOG2(NUM_LANES);
    localparam LANE_WIDTH = (LANE_BITS > 0) ? LANE_BITS : 1;
    localparam BYTE_BITS  = `CLOG2(DATA_SIZE);
    localparam BYTE_WIDTH = (BYTE_BITS > 0) ? BYTE_BITS : 1;

    localparam [3:0] STATE_IDLE       = 4'd0;
    localparam [3:0] STATE_LOCK       = 4'd1;
    localparam [3:0] STATE_DRAIN      = 4'd2;
    localparam [3:0] STATE_READ_REQ   = 4'd3;
    localparam [3:0] STATE_READ_RSP   = 4'd4;
    localparam [3:0] STATE_WRITE_REQ  = 4'd5;
    localparam [3:0] STATE_WRITE_RSP  = 4'd6;
    // Write fence: after last lane's write, issue a dummy read to ensure
    // the write has passed through the pipeline before releasing the lock.
    // This prevents other cores from reading stale data.
    localparam [3:0] STATE_FENCE_REQ  = 4'd7;
    localparam [3:0] STATE_FENCE_RSP  = 4'd8;
    `ifdef LMEM_ENABLE
    localparam [3:0] STATE_LOCAL_LOCK       = 4'd9;
    `endif

    reg [3:0] state, state_n;

    // Lock request is active from STATE_LOCK through STATE_WRITE_RSP
    reg lock_held_r;
    reg [`CLOG2(`AMO_LOCK_BANKS)-1:0] req_lock_bank_r;
    assign amo_lock_req  = lock_held_r;
    assign amo_lock_bank = req_lock_bank_r;

    // Local-memory atomics bypass global locking
    reg is_local_r;
`ifdef LMEM_ENABLE
    reg local_lock_held_r;
    assign amo_local_lock_req = local_lock_held_r;
`endif

    reg [TAG_WIDTH-1:0]                         req_tag_r;
    reg [NUM_LANES-1:0]                         req_mask_r;
    reg [4:0]                                   req_op_r;

    reg [NUM_LANES-1:0][ADDR_WIDTH-1:0]         req_addr_r;
    reg [NUM_LANES-1:0][DATA_WIDTH-1:0]         req_data_r;
    reg [NUM_LANES-1:0][DATA_SIZE-1:0]          req_byteen_r;

    reg [NUM_LANES-1:0][DATA_WIDTH-1:0]         lane_result_r;

    reg [DATA_WIDTH-1:0]                        read_data_r;

    reg [LANE_WIDTH-1:0]                         lane_idx, lane_idx_n;

    reg [NUM_LANES-1:0]                         reservation_valid;
    reg [NUM_LANES-1:0][ADDR_WIDTH-1:0]         reservation_addr;
    reg [NUM_LANES-1:0][DATA_WIDTH-1:0]         reservation_data;

    reg [DATA_WIDTH-1:0]                        last_write_data_r;
    reg                                         bank_switch_pending;

    reg                                         skip_read_r;

    wire is_amo = (core_mem_if.req_data.atype[0] != 0) && core_mem_if.req_valid;

    wire [LANE_WIDTH-1:0] first_lane_w = find_first_lane(core_mem_if.req_data.mask);

    wire [LANE_WIDTH-1:0] next_lane_w = find_next_lane(req_mask_r, lane_idx + LANE_WIDTH'(1));

    wire is_lr = (req_op_r == INST_LSU_AMO_LR);
    wire is_sc = (req_op_r == INST_LSU_AMO_SC);

    reg [NUM_LANES-1:0] sc_verified_r;

    // --- ALU ---
    wire [DATA_WIDTH-1:0] cur_lane_data = req_data_r[lane_idx];

    reg  [DATA_WIDTH-1:0] amo_byteen_mask;
    reg  [BYTE_WIDTH-1:0] amo_msb_byte;
    always @(*) begin
        amo_byteen_mask = '0;
        amo_msb_byte    = '0;
        for (integer bb = 0; bb < DATA_SIZE; bb = bb + 1) begin
            if (req_byteen_r[lane_idx][bb]) begin
                amo_byteen_mask[bb*8+:8] = 8'hFF;
                amo_msb_byte = BYTE_WIDTH'(bb);
            end
        end
    end

    wire amo_read_sign = read_data_r[amo_msb_byte * 8 + 7];
    wire amo_cur_sign  = cur_lane_data[amo_msb_byte * 8 + 7];
    wire [DATA_WIDTH-1:0] amo_read_sext = (read_data_r  & amo_byteen_mask) | ({DATA_WIDTH{amo_read_sign}} & ~amo_byteen_mask);
    wire [DATA_WIDTH-1:0] amo_cur_sext  = (cur_lane_data & amo_byteen_mask) | ({DATA_WIDTH{amo_cur_sign}}  & ~amo_byteen_mask);
    wire [DATA_WIDTH-1:0] amo_read_zext =  read_data_r  & amo_byteen_mask;
    wire [DATA_WIDTH-1:0] amo_cur_zext  =  cur_lane_data & amo_byteen_mask;

    reg [DATA_WIDTH-1:0] alu_result;

    always @(*) begin
        case (req_op_r)
            INST_LSU_AMO_SWAP: alu_result = cur_lane_data;
            INST_LSU_AMO_ADD:  alu_result = read_data_r + cur_lane_data;
            INST_LSU_AMO_XOR:  alu_result = read_data_r ^ cur_lane_data;
            INST_LSU_AMO_AND:  alu_result = read_data_r & cur_lane_data;
            INST_LSU_AMO_OR:   alu_result = read_data_r | cur_lane_data;
            INST_LSU_AMO_MIN:  alu_result = ($signed(amo_read_sext) < $signed(amo_cur_sext)) ? read_data_r : cur_lane_data;
            INST_LSU_AMO_MAX:  alu_result = ($signed(amo_read_sext) > $signed(amo_cur_sext)) ? read_data_r : cur_lane_data;
            INST_LSU_AMO_MINU: alu_result = (amo_read_zext < amo_cur_zext) ? read_data_r : cur_lane_data;
            INST_LSU_AMO_MAXU: alu_result = (amo_read_zext > amo_cur_zext) ? read_data_r : cur_lane_data;
            INST_LSU_AMO_SC:   alu_result = cur_lane_data;
            default:           alu_result = read_data_r;
        endcase
    end

    function automatic [LANE_WIDTH-1:0] find_first_lane;
        input [NUM_LANES-1:0] mask;
        integer i;
        reg found;
        begin
            find_first_lane = '0;
            found = 1'b0;
            for (i = 0; i < NUM_LANES; i = i + 1) begin
                if (!found && mask[i]) begin
                    find_first_lane = LANE_WIDTH'(i);
                    found = 1'b1;
                end
            end
        end
    endfunction

    function automatic [LANE_WIDTH-1:0] find_next_lane;
        input [NUM_LANES-1:0] mask;
        input [LANE_WIDTH-1:0] start;
        integer i;
        reg found;
        begin
            find_next_lane = start;
            found = 1'b0;
            for (i = 0; i < NUM_LANES; i = i + 1) begin
                if (!found && (LANE_WIDTH'(i) >= start) && mask[i]) begin
                    find_next_lane = LANE_WIDTH'(i);
                    found = 1'b1;
                end
            end
        end
    endfunction

    function automatic is_last_active_lane;
        input [NUM_LANES-1:0] mask;
        input [LANE_WIDTH-1:0] current;
        integer i;
        reg found_more;
        begin
            found_more = 1'b0;
            for (i = 0; i < NUM_LANES; i = i + 1) begin
                if ((LANE_WIDTH'(i) > current) && mask[i]) begin
                    found_more = 1'b1;
                end
            end
            is_last_active_lane = !found_more;
        end
    endfunction

    always @(posedge clk) begin
        if (reset) begin
            state <= STATE_IDLE;
            reservation_valid <= {NUM_LANES{1'b0}};
            lane_idx <= '0;
            lock_held_r <= 1'b0;
            req_lock_bank_r <= '0;
            is_local_r <= 1'b0;
            sc_verified_r <= {NUM_LANES{1'b0}};
            skip_read_r <= 1'b0;
            bank_switch_pending <= 1'b0;
`ifdef LMEM_ENABLE
            local_lock_held_r <= 1'b0;
`endif
        end else begin
            state    <= state_n;
            lane_idx <= lane_idx_n;

            if (state == STATE_IDLE && is_amo) begin : capture_amo
                integer ii;

                req_tag_r   <= core_mem_if.req_data.tag;
                req_mask_r  <= core_mem_if.req_data.mask;
                req_op_r    <= core_mem_if.req_data.atype[0];
                skip_read_r <= 1'b0;

                for (ii = 0; ii < NUM_LANES; ii = ii + 1) begin
                    req_addr_r[ii]   <= core_mem_if.req_data.addr[ii];
                    req_data_r[ii]   <= core_mem_if.req_data.data[ii];
                    req_byteen_r[ii] <= core_mem_if.req_data.byteen[ii];
                end

            `ifdef LMEM_ENABLE
                is_local_r <= core_mem_if.req_data.flags[first_lane_w][MEM_REQ_FLAG_LOCAL];
            `else
                is_local_r <= 1'b0;
            `endif


                if (!(core_mem_if.req_data.atype[0] == INST_LSU_AMO_SC
                      && !(reservation_valid[first_lane_w]
                           && (reservation_addr[first_lane_w]
                               == core_mem_if.req_data.addr[first_lane_w])))) begin
            `ifdef LMEM_ENABLE
                    if (!core_mem_if.req_data.flags[first_lane_w][MEM_REQ_FLAG_LOCAL]) begin
            `endif
                        lock_held_r <= 1'b1;
                        req_lock_bank_r <= core_mem_if.req_data.addr[first_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0];
            `ifdef LMEM_ENABLE
                    end else begin
                        local_lock_held_r <= 1'b1;
                    end
            `endif
                end

                if (core_mem_if.req_data.atype[0] == INST_LSU_AMO_SC) begin
                    reservation_valid <= {NUM_LANES{1'b0}};
                    sc_verified_r     <= {NUM_LANES{1'b0}};
                end
            end
            if (state == STATE_WRITE_REQ && next_mem_if.req_ready && !is_last_active_lane(req_mask_r, lane_idx) && !is_local_r) begin
                lock_held_r <= 1'b1;
                if (req_addr_r[next_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0] != req_lock_bank_r) begin
                    req_lock_bank_r <= req_addr_r[next_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0];
                end
            end
            if (state == STATE_READ_RSP && next_mem_if.rsp_valid && is_sc
                && (next_mem_if.rsp_data.data[0] != reservation_data[lane_idx])
                && !is_last_active_lane(req_mask_r, lane_idx) && !is_local_r) begin
                lock_held_r <= 1'b1;
                if (req_addr_r[next_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0] != req_lock_bank_r) begin
                    req_lock_bank_r <= req_addr_r[next_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0];
                end
            end
            if (state == STATE_WRITE_RSP && core_mem_if.rsp_ready) begin
                lock_held_r <= 1'b0;
`ifdef LMEM_ENABLE
                if (is_local_r) begin
                    local_lock_held_r <= 1'b0;
                end
`endif
            end

            if (state == STATE_WRITE_REQ && next_mem_if.req_ready) begin
                last_write_data_r  <= alu_result;
            end

            if (state == STATE_WRITE_REQ && next_mem_if.req_ready
                && !is_last_active_lane(req_mask_r, lane_idx)
                && !is_lr && !is_sc) begin
                skip_read_r <= (req_addr_r[next_lane_w] == req_addr_r[lane_idx]);
            end

            if (skip_read_r
                && ((state == STATE_LOCK && amo_lock_grant && !next_pending)
                 || (state == STATE_DRAIN && !next_pending))) begin
                read_data_r <= last_write_data_r;
                lane_result_r[lane_idx] <= last_write_data_r;
            end

            if (state == STATE_READ_RSP && next_mem_if.rsp_valid) begin
                read_data_r <= next_mem_if.rsp_data.data[0];
                lane_result_r[lane_idx] <= next_mem_if.rsp_data.data[0];
            end

            if (state == STATE_READ_RSP && next_mem_if.rsp_valid && is_lr) begin
                reservation_valid[lane_idx] <= 1'b1;
                reservation_addr[lane_idx]  <= req_addr_r[lane_idx];
                reservation_data[lane_idx]  <= next_mem_if.rsp_data.data[0];
            end

            if (state == STATE_READ_RSP && next_mem_if.rsp_valid && is_sc) begin
                sc_verified_r[lane_idx] <= (next_mem_if.rsp_data.data[0] == reservation_data[lane_idx]);
            end

            if (state == STATE_FENCE_RSP && next_mem_if.rsp_valid) begin
                if (bank_switch_pending) begin
                    req_lock_bank_r <= req_addr_r[next_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0];
                    lock_held_r     <= 1'b1; 
                end
                bank_switch_pending <= 1'b0;
            end
            if (state == STATE_WRITE_REQ && next_mem_if.req_ready && !is_sc && !is_lr) begin : amo_invalidate_reservation
                integer jj;
                for (jj = 0; jj < NUM_LANES; jj = jj + 1) begin
                    if (reservation_valid[jj] && reservation_addr[jj] == req_addr_r[lane_idx]) begin
                        reservation_valid[jj] <= 1'b0;
                    end
                end
            end

            if (state == STATE_WRITE_REQ && next_mem_if.req_ready && !is_last_active_lane(req_mask_r, lane_idx) && !is_local_r) begin
                // If next lane is in different bank, request a bank-fence before switching
                if (req_addr_r[next_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0] != req_lock_bank_r) begin
                    bank_switch_pending <= 1'b1;
                end else begin
                    lock_held_r <= 1'b1;
                    req_lock_bank_r <= req_addr_r[next_lane_w][`CLOG2(`AMO_LOCK_BANKS)-1:0];
                end
            end
        end
    end

    integer i;


    always @(*) begin
        state_n    = state;
        lane_idx_n = lane_idx;

        next_mem_if.req_valid = core_mem_if.req_valid;
        core_mem_if.req_ready = next_mem_if.req_ready;
        next_mem_if.req_data  = core_mem_if.req_data;

        core_mem_if.rsp_valid = next_mem_if.rsp_valid;
        core_mem_if.rsp_data  = next_mem_if.rsp_data;
        next_mem_if.rsp_ready = core_mem_if.rsp_ready;

        case (state)
            STATE_IDLE: begin
                if (is_amo) begin
                    next_mem_if.req_valid = 1'b0;
                    core_mem_if.req_ready = 1'b1;

                    lane_idx_n = first_lane_w;

                    if (core_mem_if.req_data.atype[0] == INST_LSU_AMO_SC
                        && !(reservation_valid[first_lane_w]
                             && (reservation_addr[first_lane_w]
                                 == core_mem_if.req_data.addr[first_lane_w]))) begin
                        state_n = STATE_WRITE_RSP;
                    end else begin
                    `ifdef LMEM_ENABLE
                        if (core_mem_if.req_data.flags[first_lane_w][MEM_REQ_FLAG_LOCAL]) begin
                            state_n = STATE_LOCAL_LOCK;
                        end else
                    `endif
                        begin
                            state_n = STATE_LOCK;
                        end
                    end
                end
            end

            STATE_LOCK: begin
                next_mem_if.req_valid = 1'b0;
                core_mem_if.req_ready = 1'b0;

                if (amo_lock_grant) begin
                    if (!next_pending) begin
                        state_n = skip_read_r ? STATE_WRITE_REQ : STATE_READ_REQ;
                    end else begin
                        state_n = STATE_DRAIN;
                    end
                end
            end

            STATE_LOCAL_LOCK: begin
                next_mem_if.req_valid = 1'b0;
                core_mem_if.req_ready = 1'b0;

                if (amo_local_lock_grant) begin
                    if (!next_pending) begin
                        state_n = skip_read_r ? STATE_WRITE_REQ : STATE_READ_REQ;
                    end else begin
                        state_n = STATE_DRAIN;
                    end
                end
            end

            STATE_DRAIN: begin
                next_mem_if.req_valid = 1'b0;
                core_mem_if.req_ready = 1'b0;

                if (!next_pending) begin
                    state_n = skip_read_r ? STATE_WRITE_REQ : STATE_READ_REQ;
                end
            end

            STATE_READ_REQ: begin
                next_mem_if.req_valid   = 1'b1;
                next_mem_if.req_data.rw = 1'b0;
                for (i = 0; i < NUM_LANES; i = i + 1) begin
                    next_mem_if.req_data.addr[i]   = req_addr_r[lane_idx];
                    next_mem_if.req_data.atype[i]  = 5'b0;
                    next_mem_if.req_data.data[i]   = {DATA_WIDTH{1'b0}};
                    next_mem_if.req_data.byteen[i] = {DATA_SIZE{1'b0}};
                    next_mem_if.req_data.flags[i]  = '0;
                end
            `ifdef LMEM_ENABLE
                if (is_local_r) begin
                    next_mem_if.req_data.flags[0][MEM_REQ_FLAG_LOCAL] = 1'b1;
                end else
            `endif
                begin
                    next_mem_if.req_data.flags[0][MEM_REQ_FLAG_IO] = 1'b1;
                end
                next_mem_if.req_data.mask = {{(NUM_LANES-1){1'b0}}, 1'b1};
                next_mem_if.req_data.tag  = req_tag_r;

                core_mem_if.req_ready = 1'b0;
                core_mem_if.rsp_valid = 1'b0;
                next_mem_if.rsp_ready = 1'b0;

                if (next_mem_if.req_ready) begin
                    state_n = STATE_READ_RSP;
                end
            end

            STATE_READ_RSP: begin
                next_mem_if.req_valid = 1'b0;
                next_mem_if.rsp_ready = 1'b1;
                core_mem_if.req_ready = 1'b0;
                core_mem_if.rsp_valid = 1'b0;

                if (next_mem_if.rsp_valid) begin
                    if (is_lr) begin
                        state_n = STATE_WRITE_RSP;
                    end else if (is_sc) begin
                        if (next_mem_if.rsp_data.data[0] == reservation_data[lane_idx]) begin
                            state_n = STATE_WRITE_REQ; // data matches, proceed to write
                        end else begin
                            if (is_last_active_lane(req_mask_r, lane_idx)) begin
                                state_n = STATE_WRITE_RSP; // all lanes done
                            end else begin
                                lane_idx_n = next_lane_w;
                                state_n = is_local_r ? STATE_LOCAL_LOCK : STATE_LOCK;
                            end
                        end
                    end else begin
                        state_n = STATE_WRITE_REQ;
                    end
                end
            end

            STATE_WRITE_REQ: begin
                next_mem_if.req_valid   = 1'b1;
                next_mem_if.req_data.rw = 1'b1;
                for (i = 0; i < NUM_LANES; i = i + 1) begin
                    next_mem_if.req_data.addr[i]   = req_addr_r[lane_idx];
                    next_mem_if.req_data.data[i]   = alu_result;
                    next_mem_if.req_data.byteen[i] = req_byteen_r[lane_idx];
                    next_mem_if.req_data.atype[i]  = 5'b0;
                    next_mem_if.req_data.flags[i]  = '0;
                end
            `ifdef LMEM_ENABLE
                if (is_local_r) begin
                    next_mem_if.req_data.flags[0][MEM_REQ_FLAG_LOCAL] = 1'b1;
                end else
            `endif
                begin
                    next_mem_if.req_data.flags[0][MEM_REQ_FLAG_IO] = 1'b1;
                end
                next_mem_if.req_data.mask = {{(NUM_LANES-1){1'b0}}, 1'b1};
                next_mem_if.req_data.tag  = req_tag_r;

                core_mem_if.req_ready = 1'b0;
                core_mem_if.rsp_valid = 1'b0;
                next_mem_if.rsp_ready = 1'b0;

                if (next_mem_if.req_ready) begin
                    if (is_last_active_lane(req_mask_r, lane_idx)) begin
                        state_n = (is_local_r || is_lr) ? STATE_WRITE_RSP : STATE_FENCE_REQ;
                    end else begin
                        if (bank_switch_pending && !is_local_r) begin
                            lane_idx_n = lane_idx; 
                            state_n = STATE_FENCE_REQ;
                        end else begin
                            lane_idx_n = next_lane_w;
                            state_n = is_local_r ? STATE_LOCAL_LOCK : STATE_LOCK;
                        end
                    end
                end
            end

            STATE_FENCE_REQ: begin
                next_mem_if.req_valid   = 1'b1;
                next_mem_if.req_data.rw = 1'b0;
                for (i = 0; i < NUM_LANES; i = i + 1) begin
                    next_mem_if.req_data.addr[i]   = req_addr_r[lane_idx];
                    next_mem_if.req_data.atype[i]  = 5'b0;
                    next_mem_if.req_data.data[i]   = {DATA_WIDTH{1'b0}};
                    next_mem_if.req_data.byteen[i] = {DATA_SIZE{1'b0}};
                    next_mem_if.req_data.flags[i]  = '0;
                end
                next_mem_if.req_data.flags[0][MEM_REQ_FLAG_IO] = 1'b1;
                next_mem_if.req_data.mask = {{(NUM_LANES-1){1'b0}}, 1'b1};
                next_mem_if.req_data.tag  = req_tag_r;

                core_mem_if.req_ready = 1'b0;
                core_mem_if.rsp_valid = 1'b0;
                next_mem_if.rsp_ready = 1'b0;

                if (next_mem_if.req_ready) begin
                    state_n = STATE_FENCE_RSP;
                end
            end

            STATE_FENCE_RSP: begin
                next_mem_if.req_valid = 1'b0;
                next_mem_if.rsp_ready = 1'b1;
                core_mem_if.req_ready = 1'b0;
                core_mem_if.rsp_valid = 1'b0;

                if (next_mem_if.rsp_valid) begin
                    if (bank_switch_pending) begin
                        lane_idx_n = next_lane_w;
                        state_n = STATE_LOCK;
                    end else begin
                        state_n = STATE_WRITE_RSP;
                    end
                end
            end

            STATE_WRITE_RSP: begin
                next_mem_if.rsp_ready = 1'b0;
                next_mem_if.req_valid = 1'b0;
                core_mem_if.req_ready = 1'b0;

                core_mem_if.rsp_valid = 1'b1;
                for (i = 0; i < NUM_LANES; i = i + 1) begin
                    if (is_sc) begin
                        core_mem_if.rsp_data.data[i] = sc_verified_r[i] ? {DATA_WIDTH{1'b0}} : {{(DATA_WIDTH-1){1'b0}}, 1'b1};
                    end else begin
                        core_mem_if.rsp_data.data[i] = lane_result_r[i];
                    end
                end
                core_mem_if.rsp_data.tag  = req_tag_r;
                core_mem_if.rsp_data.mask = req_mask_r;

                if (core_mem_if.rsp_ready) begin
                    state_n = STATE_IDLE;
                end
            end

            default: begin
                state_n = STATE_IDLE;
            end
        endcase
    end


`ifdef DBG_TRACE_MEM
    always @(posedge clk) begin
        if (state == STATE_IDLE && is_amo) begin
            `TRACE(2, ("%t: AMO-HANDLER: captured AMO, op=%0d, tag=0x%0h, addr[0]=0x%0h, mask=%b, pending=%0d\n",
                     $time, core_mem_if.req_data.atype[0], core_mem_if.req_data.tag, core_mem_if.req_data.addr[0], core_mem_if.req_data.mask, next_pending))
        end
        if (state == STATE_READ_REQ && next_mem_if.req_ready) begin
            `TRACE(2, ("%t: AMO-HANDLER: READ_REQ lane=%0d, addr=0x%0h\n", $time, lane_idx, req_addr_r[lane_idx]))
        end
        if (state == STATE_READ_RSP && next_mem_if.rsp_valid) begin
            `TRACE(2, ("%t: AMO-HANDLER: READ_RSP lane=%0d, data=0x%0h\n", $time, lane_idx, next_mem_if.rsp_data.data[0]))
        end
        if (state == STATE_WRITE_REQ && next_mem_if.req_ready) begin
            `TRACE(2, ("%t: AMO-HANDLER: WRITE_REQ lane=%0d, data=0x%0h\n", $time, lane_idx, alu_result))
        end
        if (skip_read_r
            && ((state == STATE_LOCK && amo_lock_grant && !next_pending)
             || (state == STATE_DRAIN && !next_pending))) begin
            `TRACE(2, ("%t: AMO-HANDLER: FORWARDING lane=%0d, data=0x%0h (skip read)\n", $time, lane_idx, last_write_data_r))
        end
        if (state == STATE_FENCE_REQ && next_mem_if.req_ready) begin
            `TRACE(2, ("%t: AMO-HANDLER: FENCE_REQ lane=%0d, addr=0x%0h\n", $time, lane_idx, req_addr_r[lane_idx]))
        end
        if (state == STATE_FENCE_RSP && next_mem_if.rsp_valid) begin
            `TRACE(2, ("%t: AMO-HANDLER: FENCE_RSP lane=%0d (write committed)\n", $time, lane_idx))
        end
        if (state == STATE_WRITE_RSP && core_mem_if.rsp_ready) begin
            `TRACE(2, ("%t: AMO-HANDLER: WRITE_RSP->IDLE, mask=%b\n", $time, req_mask_r))
        end
    end
`endif


endmodule
