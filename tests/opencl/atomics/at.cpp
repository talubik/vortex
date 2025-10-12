#define CL_TARGET_OPENCL_VERSION 120
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120

#include <CL/cl.h>
#include <CL/opencl.hpp>
#include <iostream>
#include <string>
#include <vector>

const char *kernelSrc = R"CLC(
__kernel void atomic_test(global int* data) {
    int id = get_global_id(0);
    atomic_add((volatile global int*)&data[0], 1);
}
)CLC";

int main() {
  std::vector<cl::Platform> platforms;
  cl::Platform::get(&platforms);
  if (platforms.empty()) {
    std::cerr << "No avaliable OpenCL platforms" << std::endl;
    return 1;
  }

  std::vector<cl::Device> devices;
  platforms[0].getDevices(CL_DEVICE_TYPE_ALL, &devices);
  if (devices.empty()) {
    std::cerr << "No devices on platform" << std::endl;
    return 2;
  }

  cl::Context context(devices[0]);
  cl::CommandQueue queue(context, devices[0]);

  int initialValue = 0;
  cl::Buffer buffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                    sizeof(int), &initialValue);

  cl::Program program(context, kernelSrc);
  if (program.build({devices[0]}) != CL_SUCCESS) {
    std::cerr << "Kernel compilation error:\n"
              << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(devices[0])
              << std::endl;
    return 3;
  }

  cl::Kernel kernel(program, "atomic_test");
  kernel.setArg(0, buffer);

  const int N = 1024;
  queue.enqueueNDRangeKernel(kernel, cl::NullRange,
                             cl::NDRange(N), cl::NullRange);

  int result = 0;
  queue.enqueueReadBuffer(buffer, CL_TRUE, 0, sizeof(int), &result);

  std::cout << "Expected result: " << N
            << ", received: " << result << std::endl;

  return 0;
}
