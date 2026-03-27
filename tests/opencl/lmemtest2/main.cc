#include <stdio.h>
#include <stdlib.h>
#include <CL/opencl.h>
#include <unistd.h>
#include <vector>

#define KERNEL_NAME "lmemtest2"

#define LMEM_INTS 516

#define CL_CHECK(_expr)                                                \
   do {                                                                \
     cl_int _err = _expr;                                              \
     if (_err == CL_SUCCESS)                                           \
       break;                                                          \
     printf("OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err);  \
     cleanup();                                                        \
     exit(-1);                                                         \
   } while (0)

#define CL_CHECK2(_expr)                                               \
   ({                                                                  \
     cl_int _err = CL_INVALID_VALUE;                                   \
     decltype(_expr) _ret = _expr;                                     \
     if (_err != CL_SUCCESS) {                                         \
       printf("OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err); \
       cleanup();                                                       \
       exit(-1);                                                        \
     }                                                                  \
     _ret;                                                              \
   })

static int read_kernel_file(const char* filename, uint8_t** data, size_t* size) {
  if (nullptr == filename || nullptr == data || 0 == size)
    return -1;
  FILE* fp = fopen(filename, "r");
  if (NULL == fp) {
    fprintf(stderr, "Failed to load kernel.\n");
    return -1;
  }
  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  rewind(fp);
  *data = (uint8_t*)malloc(fsize);
  *size = fread(*data, 1, fsize, fp);
  fclose(fp);
  return 0;
}

cl_device_id device_id       = NULL;
cl_context context            = NULL;
cl_command_queue commandQueue = NULL;
cl_program program            = NULL;
cl_kernel kernel              = NULL;
cl_mem out_memobj             = NULL;
uint8_t* kernel_bin           = NULL;

static void cleanup() {
  if (commandQueue) clReleaseCommandQueue(commandQueue);
  if (kernel)       clReleaseKernel(kernel);
  if (program)      clReleaseProgram(program);
  if (out_memobj)   clReleaseMemObject(out_memobj);
  if (context)      clReleaseContext(context);
  if (device_id)    clReleaseDevice(device_id);
  if (kernel_bin)   free(kernel_bin);
}

uint32_t local_size = 4;

static void show_usage() {
  printf("Usage: [-n local_size] [-h: help]\n");
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:h")) != -1) {
    switch (c) {
    case 'n': local_size = atoi(optarg); break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  printf("local_size=%d\n", local_size);
}

int main(int argc, char **argv) {
  parse_args(argc, argv);

  cl_platform_id platform_id;
  size_t kernel_size;

  CL_CHECK(clGetPlatformIDs(1, &platform_id, NULL));
  CL_CHECK(clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_DEFAULT, 1, &device_id, NULL));

  printf("Create context\n");
  context = CL_CHECK2(clCreateContext(NULL, 1, &device_id, NULL, NULL, &_err));

  printf("Allocate device buffer\n");
  out_memobj = CL_CHECK2(clCreateBuffer(context, CL_MEM_WRITE_ONLY,
                                        local_size * sizeof(int), NULL, &_err));

  printf("Create program from kernel source\n");
  if (0 != read_kernel_file("kernel.cl", &kernel_bin, &kernel_size))
    return -1;
  program = CL_CHECK2(clCreateProgramWithSource(
    context, 1, (const char**)&kernel_bin, &kernel_size, &_err));

  CL_CHECK(clBuildProgram(program, 1, &device_id, NULL, NULL, NULL));
  kernel = CL_CHECK2(clCreateKernel(program, KERNEL_NAME, &_err));

  // arg 0: global output (local_size ints)
  CL_CHECK(clSetKernelArg(kernel, 0, sizeof(cl_mem), &out_memobj));
  // arg 1: local buffer A -- LMEM_INTS ints
  // Indices 0..local_size-1 and 512..512+local_size-1 must be valid.
  // Index 512 is at byte offset 2048, which aliases index 0 in the buggy simulator.
  CL_CHECK(clSetKernelArg(kernel, 1, LMEM_INTS * sizeof(int), NULL));

  commandQueue = CL_CHECK2(clCreateCommandQueue(context, device_id, 0, &_err));

  printf("Execute kernel\n");
  size_t gws[1] = {local_size};
  size_t lws[1] = {local_size};
  CL_CHECK(clEnqueueNDRangeKernel(commandQueue, kernel, 1, NULL, gws, lws, 0, NULL, NULL));
  CL_CHECK(clFinish(commandQueue));

  printf("Read output\n");
  std::vector<int> results(local_size);
  CL_CHECK(clEnqueueReadBuffer(commandQueue, out_memobj, CL_TRUE, 0,
                               local_size * sizeof(int), results.data(), 0, NULL, NULL));

  printf("Verify result\n");
  // Each thread t writes A[t] = t+1, then A[t+512] = -(t+1).
  // Bug:   A[t+512] at byte offset 2048+t*4 wraps to same address as A[t] -> output[t] = -(t+1)  FAIL
  // Fixed: A[t+512] is a distinct address                                  -> output[t] =   t+1   PASS
  int errors = 0;
  for (uint32_t i = 0; i < local_size; ++i) {
    int expected = (int)(i + 1);
    if (results[i] != expected) {
      printf("*** FAIL: output[%d] = %d, expected %d\n", i, results[i], expected);
      printf("         (A[%d] at byte offset %d aliased A[%d] due to wrong address mask)\n",
             i + 512, (i + 512) * 4, i);
      ++errors;
    }
  }

  if (errors == 0) {
    printf("PASSED!\n");
  } else {
    printf("FAILED! - %d errors\n", errors);
  }

  cleanup();
  return errors;
}
