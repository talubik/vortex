#include <stdio.h>
#include <vx_intrinsics.h>
#include <vx_print.h>
#include <vx_spawn.h>



#define HEAP_SZ (1024 * 1024)

char __data_pool[HEAP_SZ];
int __data_pool_offset = 0;

void *vx_malloc(int sz) {
  if (__data_pool_offset + sz > HEAP_SZ) {
    vx_printf("Out of memory\n");
    return nullptr;
  }
  void *ptr = &__data_pool[__data_pool_offset];
  __data_pool_offset += sz;
  return ptr;
}

inline int vx_atomic_add(int *addr, int value) {
  int old_value;
  __asm__ volatile(
      "amoadd.w %0, %2, (%1)"
      : "=r"(old_value)
      : "r"(addr), "r"(value)
      : "memory");
  return old_value;
}



typedef struct {
  int *data;
  int num_threads;
} atomic_add_args_t;


void atomic_add_kernel(atomic_add_args_t *__UNIFORM__ args) {
  int old_val = vx_atomic_add(args->data, 1);
  vx_printf("[+] Thread %d: atomic_add completed. Old value was %d.\n", vx_thread_id(), old_val);
}


int main() {
  vx_printf(">> Starting atomic_add test in hostless mode (coreid=%d, warpid=%d, threadid=%d)\n",
            vx_core_id(), vx_warp_id(), vx_thread_id());

  vx_printf(">> Allocating shared counter\n");
  int *shared_counter = (int *)vx_malloc(sizeof(int));

  *shared_counter = 0;
  vx_printf(">> Initial value of shared counter: %d\n", *shared_counter);

  atomic_add_args_t args;
  args.data = shared_counter;

  vx_printf(">> kernel_arg.data: %p\n", args.data);

  atomic_add_kernel(&args); 

  vx_printf(">> Kernel finished executing\n");

  vx_printf(">> Back to single threaded execution (coreid=%d, warpid=%d, threadid=%d)\n",
            vx_core_id(), vx_warp_id(), vx_thread_id());

  int final_value = *shared_counter;
  int expected_value = 1;

  vx_printf(">> Final value: %d, Expected value: %d\n", final_value, expected_value);

  if (final_value == expected_value) {
    vx_printf("*** Atomic Add test PASSED! ***\n");
  } else {
    vx_printf("*** Atomic Add test FAILED! ***\n");
    return -1;
  }

  return 0;
}