## Fix local memory address aliasing in simx simulator

When I tried to run OpenCL kernels that use `__local` buffers larger than 2048 bytes, I found that writes to addresses beyond the 2048-byte boundary silently overwrote data at lower addresses. This Pull Request fixes a bug in `sim/simx/local_mem.cpp` where the address mask used to index the local memory RAM was too narrow.

### Bug: `to_local_addr()` uses line count instead of capacity for address bits

The function `to_local_addr()` extracts the lower N bits of the incoming address to produce a RAM index. N was computed as `log2ceil(capacity / line_size)` (the number of cache lines) instead of `log2ceil(capacity)` (the number of addressable bytes).

With the default configuration (`capacity = 16384`, `line_size = 8`):

- **Buggy**: `total_lines = 16384 / 8 = 2048`, `line_bits_ = log2ceil(2048) = 11`, mask = `0x7FF`
- **Fixed**: `addr_bits_ = log2ceil(16384) = 14`, mask = `0x3FFF`

The 11-bit mask means any two byte addresses that differ by exactly 2048 map to the **same** RAM location. For example, `A[0]` (offset 0) and `A[512]` (offset 2048) alias each other, so writing `A[512]` silently overwrites `A[0]`.

### Fix

Compute `addr_bits_` directly from `config.capacity` instead of from the number of lines:

```cpp
// Before
uint32_t total_lines = config.capacity / config.line_size;
line_bits_ = log2ceil(total_lines);

// After
addr_bits_ = log2ceil(config.capacity);
```

I also added a test [`tests/opencl/lmemtest2`](tests/opencl/lmemtest2) which reproduces the aliasing:
each thread `t` writes `A[t] = t+1`, then `A[t+512] = -(t+1)`.
With the bug, `A[t+512]` at byte offset `2048 + t*4` aliases `A[t]`, so reading `A[t]` returns `-(t+1)` instead of `t+1`.
With the fix, all reads return the correct values.
