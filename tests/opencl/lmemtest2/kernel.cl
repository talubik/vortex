__kernel void lmemtest2(__global int* output, __local int* A) {
    int lid = get_local_id(0);
    A[lid] = lid + 1;
    barrier(CLK_LOCAL_MEM_FENCE);
    A[lid + 512] = -(lid + 1);
    barrier(CLK_LOCAL_MEM_FENCE);
    output[lid] = A[lid];  // expected: lid+1
}
