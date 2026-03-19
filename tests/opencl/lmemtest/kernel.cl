__kernel void lmemtest(__global int* output, __local int* A, __local int* B) {
    int lid   = get_local_id(0);
    int lsize = get_local_size(0);

    A[lid] = lid + 1;
    B[lid] = (lid + 1) * 10;
    barrier(CLK_LOCAL_MEM_FENCE);

    if (lid == 0) {
        output[0] = (int)(B - A);   // expected: lsize
    }

    output[lid + 1] = A[lid] + B[lid]; 
}
