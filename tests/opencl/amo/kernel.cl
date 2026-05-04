#pragma OPENCL EXTENSION cl_khr_global_int32_base_atomics     : enable
#pragma OPENCL EXTENSION cl_khr_global_int32_extended_atomics : enable
#pragma OPENCL EXTENSION cl_khr_local_int32_base_atomics      : enable
#pragma OPENCL EXTENSION cl_khr_local_int32_extended_atomics  : enable


__kernel void test_amo_add(__global int *counter) {
    atomic_add(counter, 1);
}


__kernel void test_amo_or(__global int *flags) {
    int id = (int)get_global_id(0);
    atomic_or(flags, 1 << id);
}


__kernel void test_amo_and(__global int *flags) {
    int id = (int)get_global_id(0);
    atomic_and(flags, ~(1 << id));
}


__kernel void test_amo_xor(__global int *val) {
    int id = (int)get_global_id(0);
    atomic_xor(val, 1 << id);
}


__kernel void test_amo_xchg(__global int *counter,
                             int          new_val,
                             __global int *old_out) {
    old_out[0] = atomic_xchg(counter, new_val);
}


__kernel void test_amo_min(__global int *val) {
    int id = (int)get_global_id(0);
    atomic_min(val, id);
}


__kernel void test_amo_max(__global int *val) {
    int id = (int)get_global_id(0);
    atomic_max(val, id);
}


__kernel void test_amo_minu(__global uint *val) {
    uint id = get_global_id(0);
    atomic_min(val, id);
}

__kernel void test_amo_maxu(__global uint *val) {
    uint id = get_global_id(0);
    atomic_max(val, id);
}


__kernel void test_local_amo_add(__global int *out) {
    __local int counter;
    counter = 0;
    atomic_add(&counter, 42);
    out[0] = counter;
}


__kernel void test_amo_cas(__global int *val,
                            int           cmp,
                            int           desired,
                            __global int *old_out) {
    old_out[0] = atomic_cmpxchg(val, cmp, desired);
}
