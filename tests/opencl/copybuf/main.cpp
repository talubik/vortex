#define CL_TARGET_OPENCL_VERSION 120
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120

#include <CL/opencl.hpp>
#include <iostream>
#include <vector>

int main() {
  std::vector<cl::Platform> platforms;
  cl::Platform::get(&platforms);
  if (platforms.empty()) {
    std::cerr << "No OpenCL platform\n";
    return 1;
  }

  std::vector<cl::Device> devices;
  platforms[0].getDevices(CL_DEVICE_TYPE_ALL, &devices);
  if (devices.empty()) {
    std::cerr << "No OpenCL device\n";
    return 2;
  }

  cl::Context context(devices[0]);
  cl::CommandQueue queue(context, devices[0]);

  std::vector<int> src = {10, 20, 30, 40, 50};
  std::vector<int> dst(src.size(), 0);

  cl::Buffer bufSrc(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                    sizeof(int) * src.size(), src.data());
  cl::Buffer bufDst(context, CL_MEM_WRITE_ONLY,
                    sizeof(int) * dst.size());

  queue.enqueueCopyBuffer(bufSrc, bufDst, 0, 0, sizeof(int) * src.size());

  queue.enqueueReadBuffer(bufDst, CL_TRUE, 0, sizeof(int) * dst.size(), dst.data());

  std::cout << "Source Buffer: ";
  for (auto v : src)
    std::cout << v << " ";
  std::cout << "\nDestination buffer: ";
  for (auto v : dst)
    std::cout << v << " ";
  std::cout << std::endl;

  return 0;
}
