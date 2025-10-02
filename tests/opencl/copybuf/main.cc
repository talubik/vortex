#define CL_TARGET_OPENCL_VERSION 120
#include <CL/opencl.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
  cl_int err;
  cl_uint num_platforms = 0;
  err = clGetPlatformIDs(0, NULL, &num_platforms);
  if (err != CL_SUCCESS || num_platforms == 0) {
    fprintf(stderr, "No OpenCL platform\n");
    return -1;
  }
  cl_platform_id *platforms = (cl_platform_id *)malloc(sizeof(cl_platform_id) * num_platforms);
  clGetPlatformIDs(num_platforms, platforms, NULL);
  cl_uint num_devices = 0;
  err = clGetDeviceIDs(platforms[0], CL_DEVICE_TYPE_ALL, 0, NULL, &num_devices);
  if (err != CL_SUCCESS || num_devices == 0) {
    fprintf(stderr, "No OpenCL device\n");
    free(platforms);
    return -1;
  }
  cl_device_id *devices = (cl_device_id *)malloc(sizeof(cl_device_id) * num_devices);
  clGetDeviceIDs(platforms[0], CL_DEVICE_TYPE_ALL, num_devices, devices, NULL);
  cl_context context = clCreateContext(NULL, 1, &devices[0], NULL, NULL, &err);
  if (err != CL_SUCCESS) {
    fprintf(stderr, "Failed to create context\n");
    return -1;
  }
  cl_command_queue queue = clCreateCommandQueue(context, devices[0], 0, &err);
  if (err != CL_SUCCESS) {
    fprintf(stderr, "Failed to create queue\n");
    return -1;
  }
  int src[] = {10, 20, 30, 40, 50};
  int dst[5] = {0};
  cl_mem bufSrc = clCreateBuffer(context, 0 ,
                                 sizeof(src), NULL, &err);
  if(err!= CL_SUCCESS){
    fprintf(stderr, "Failed to create buffer\n");
    return -1;
  }
  cl_mem bufDst = clCreateBuffer(context, 0,
                                 sizeof(dst), NULL, &err);
  if (err != CL_SUCCESS) {
    fprintf(stderr, "Failed to create buffer\n");
    return -1;
  }
  err = clEnqueueCopyBuffer(queue, bufSrc, bufDst, 0, 0, sizeof(src), 0, NULL, NULL);
  if (err != CL_SUCCESS) {
    fprintf(stderr, "Failed to copy buffer\n");
    return -1;
  }
  err = clEnqueueReadBuffer(queue, bufDst, CL_TRUE, 0, sizeof(dst), dst, 0, NULL, NULL);
  if (err != CL_SUCCESS) {
    fprintf(stderr, "Failed to read buffer\n");
    return -1;
  }

  printf("Source Buffer: ");
  for (int i = 0; i < 5; i++)
    printf("%d ", src[i]);
  printf("\nDestination buffer: ");
  for (int i = 0; i < 5; i++)
    printf("%d ", dst[i]);
  printf("\n");

  clReleaseMemObject(bufSrc);
  clReleaseMemObject(bufDst);
  clReleaseCommandQueue(queue);
  clReleaseContext(context);

  free(devices);
  free(platforms);

  return 0;
}
