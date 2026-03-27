// Copyright (c) 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

#include <CL/cl.h>
#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CL_CHECK(_expr)                                                         \
  do {                                                                          \
    cl_int _err = _expr;                                                        \
    if (_err == CL_SUCCESS) break;                                              \
    fprintf(stderr, "OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err);   \
    abort();                                                                    \
  } while (0)

#define CL_CHECK2(_expr)                                                        \
  ({                                                                            \
    cl_int _err = CL_INVALID_VALUE;                                             \
    decltype(_expr) _ret = _expr;                                               \
    if (_err != CL_SUCCESS) {                                                   \
      fprintf(stderr, "OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err); \
      abort();                                                                  \
    }                                                                           \
    _ret;                                                                       \
  })

static int read_kernel_file(const char *filename, uint8_t **data, size_t *size) {
  if (!filename || !data || !size) return -1;
  FILE *fp = fopen(filename, "r");
  if (!fp) { fprintf(stderr, "Failed to load kernel.\n"); return -1; }
  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  rewind(fp);
  *data = (uint8_t *)malloc(fsize);
  *size = fread(*data, 1, fsize, fp);
  fclose(fp);
  return 0;
}

static uint8_t       *kernel_bin = NULL;
static cl_device_id   device_id;
static cl_context     ctx;
static cl_command_queue queue;
static cl_program     program;
static cl_mem         buf;   // single-int working buffer
static cl_mem         buf2;  // secondary single-int buffer (XCHG old-value)
static int            total_errors = 0;



static void buf_write(cl_mem m, int val) {
  CL_CHECK(clEnqueueWriteBuffer(queue, m, CL_TRUE, 0, sizeof(int), &val, 0, NULL, NULL));
}

static int buf_read(cl_mem m) {
  int val = 0;
  CL_CHECK(clEnqueueReadBuffer(queue, m, CL_TRUE, 0, sizeof(int), &val, 0, NULL, NULL));
  return val;
}

static void check(const char *name, int expected, int actual) {
  bool pass = (expected == actual);
  printf("  %-12s: %s  (expected=0x%08X / %d,  actual=0x%08X / %d)\n",
         name, pass ? "PASSED" : "FAILED",
         (unsigned)expected, expected, (unsigned)actual, actual);
  if (!pass) ++total_errors;
}


static void test_amo_add(int N) {
  buf_write(buf, 0);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_add", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_ADD", N, buf_read(buf));
  clReleaseKernel(k);
}

static void test_amo_or(int N) {
  buf_write(buf, 0);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_or", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_OR", (1 << N) - 1, buf_read(buf));
  clReleaseKernel(k);
}

static void test_amo_and(int N) {
  buf_write(buf, (1 << N) - 1);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_and", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_AND", 0, buf_read(buf));
  clReleaseKernel(k);
}

static void test_amo_xor(int N) {
  buf_write(buf, 0);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_xor", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_XOR", (1 << N) - 1, buf_read(buf));
  clReleaseKernel(k);
}

static void test_amo_xchg() {
  buf_write(buf,  99);
  buf_write(buf2,  0);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_xchg", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  cl_int new_val = 42;
  CL_CHECK(clSetKernelArg(k, 1, sizeof(cl_int), &new_val));
  CL_CHECK(clSetKernelArg(k, 2, sizeof(cl_mem), &buf2));
  size_t gws = 1, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_XCHG_old", 99, buf_read(buf2)); // returned old value
  check("AMO_XCHG_new", 42, buf_read(buf));  // counter after swap
  clReleaseKernel(k);
}

static void test_amo_min(int N) {
  buf_write(buf, INT_MAX);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_min", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_MIN", 0, buf_read(buf));
  clReleaseKernel(k);
}

static void test_amo_max(int N) {
  buf_write(buf, INT_MIN);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_max", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_MAX", N - 1, buf_read(buf));
  clReleaseKernel(k);
}

static void test_amo_minu(int N) {
  buf_write(buf, (int)(unsigned int)UINT_MAX);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_minu", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_MINU", 0, buf_read(buf));
  clReleaseKernel(k);
}

static void test_amo_maxu(int N) {
  buf_write(buf, 0);
  cl_kernel k = CL_CHECK2(clCreateKernel(program, "test_amo_maxu", &_err));
  CL_CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  size_t gws = (size_t)N, lws = 1;
  CL_CHECK(clEnqueueNDRangeKernel(queue, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
  CL_CHECK(clFinish(queue));
  check("AMO_MAXU", N - 1, buf_read(buf));
  clReleaseKernel(k);
}



static int num_threads = 16;

static void show_usage() {
  printf("Usage: [-n num_threads (1-16)] [-h: help]\n");
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:h")) != -1) {
    switch (c) {
    case 'n': num_threads = atoi(optarg); break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  if (num_threads < 1 || num_threads > 16) {
    fprintf(stderr, "Error: num_threads must be between 1 and 16 (got %d).\n", num_threads);
    exit(-1);
  }
  printf("num_threads=%d\n", num_threads);
}

int main(int argc, char **argv) {
  parse_args(argc, argv);

  cl_platform_id platform_id;
  size_t kernel_size;

  CL_CHECK(clGetPlatformIDs(1, &platform_id, NULL));
  CL_CHECK(clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_DEFAULT, 1, &device_id, NULL));

  ctx   = CL_CHECK2(clCreateContext(NULL, 1, &device_id, NULL, NULL, &_err));
  queue = CL_CHECK2(clCreateCommandQueue(ctx, device_id, 0, &_err));

  printf("Create program from kernel source\n");
  if (read_kernel_file("kernel.cl", &kernel_bin, &kernel_size) != 0) return -1;
  program = CL_CHECK2(clCreateProgramWithSource(
      ctx, 1, (const char **)&kernel_bin, &kernel_size, &_err));
  CL_CHECK(clBuildProgram(program, 1, &device_id, NULL, NULL, NULL));

  buf  = CL_CHECK2(clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(int), NULL, &_err));
  buf2 = CL_CHECK2(clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(int), NULL, &_err));

  printf("Running AMO tests with %d thread(s)...\n", num_threads);

  int N = num_threads;
  test_amo_add (N);
  test_amo_or  (N);
  test_amo_and (N);
  test_amo_xor (N);
  test_amo_xchg();
  test_amo_min (N);
  test_amo_max (N);
  test_amo_minu(N);
  test_amo_maxu(N);

  if (total_errors == 0)
    printf("\nPASSED!\n");
  else
    printf("\nFAILED! - %d error(s)\n", total_errors);

  clReleaseMemObject(buf);
  clReleaseMemObject(buf2);
  clReleaseProgram(program);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);
  clReleaseDevice(device_id);
  free(kernel_bin);

  return total_errors;
}
