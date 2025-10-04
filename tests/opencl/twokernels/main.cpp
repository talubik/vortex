#define CL_TARGET_OPENCL_VERSION 120
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120

#include <CL/opencl.hpp>
#include <iostream>
#include <vector>

const char *kernelSrcAdd = R"CLC(
__kernel void pair_add(__global const int* in, __global int* out) {
    int gid = get_global_id(0);
    int i = 2 * gid;
    out[gid] = in[i]+ in[i + 1];
}
)CLC";

const char *kernelSrcMul = R"CLC(
__kernel void pair_mul(__global const int* in, __global int* out) {
    int gid = get_global_id(0);
    int i = 2 * gid;
    out[gid] = in[i]* in[i + 1];
}
)CLC";

std::vector<int> runPairAdd(cl::Context &context, cl::CommandQueue &queue,
                            const std::vector<int> &input) {
  int N = input.size();
  int outN = N / 2;
  std::vector<int> output(outN, 0);

  cl::Buffer bufIn(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                   sizeof(int) * N, const_cast<int *>(input.data()));
  cl::Buffer bufOut(context, CL_MEM_WRITE_ONLY, sizeof(int) * outN);

  cl::Program program(context, kernelSrcAdd);
  if (program.build({queue.getInfo<CL_QUEUE_DEVICE>()}) != CL_SUCCESS) {
    std::cerr << "Error of bulding  pair_add:\n"
              << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(queue.getInfo<CL_QUEUE_DEVICE>())
              << std::endl;
    return {};
  }

  cl::Kernel kernel(program, "pair_add");
  kernel.setArg(0, bufIn);
  kernel.setArg(1, bufOut);

  queue.enqueueNDRangeKernel(kernel, cl::NullRange,
                             cl::NDRange(outN), cl::NullRange);
  queue.enqueueReadBuffer(bufOut, CL_TRUE, 0, sizeof(int) * outN, output.data());
  queue.finish();
  return output;
}

std::vector<int> runPairMul(cl::Context &context, cl::CommandQueue &queue,
                            const std::vector<int> &input) {
  int N = input.size();
  int outN = N / 2;
  std::vector<int> output(outN, 0);

  cl::Buffer bufIn(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                   sizeof(int) * N, const_cast<int *>(input.data()));
  cl::Buffer bufOut(context, CL_MEM_WRITE_ONLY, sizeof(int) * outN);

  cl::Program program(context, kernelSrcMul);
  if (program.build({queue.getInfo<CL_QUEUE_DEVICE>()}) != CL_SUCCESS) {
    std::cerr << "Error of building pair_mul:\n"
              << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(queue.getInfo<CL_QUEUE_DEVICE>())
              << std::endl;
    return {};
  }

  cl::Kernel kernel(program, "pair_mul");
  kernel.setArg(0, bufIn);
  kernel.setArg(1, bufOut);
  queue.enqueueNDRangeKernel(kernel, cl::NullRange,
                             cl::NDRange(outN), cl::NullRange);
  queue.enqueueReadBuffer(bufOut, CL_TRUE, 0, sizeof(int) * outN, output.data());
  queue.finish();
  return output;
}

int main() {
  std::vector<cl::Platform> platforms;
  cl::Platform::get(&platforms);
  if (platforms.empty()) {
    std::cerr << "No OpenCL platforms\n";
    return 1;
  }

  std::vector<cl::Device> devices;

  platforms[0].getDevices(CL_DEVICE_TYPE_ALL, &devices);
  if (devices.empty()) {
    std::cerr << "No OpenCL devices\n";
    return 2;
  }

  cl::Context context(devices[0]);
  cl::CommandQueue queue(context, devices[0]);

  std::vector<int> input = {1, 2, 3, 4, 5, 6, 7, 8};
  auto resultAdd = runPairAdd(context, queue, input);
  auto resultMul = runPairMul(context, queue, input);

  std::cout << "Input: ";
  for (auto v : input)
    std::cout << v << " ";
  std::cout << "\nResult of pair_add: ";
  for (auto v : resultAdd)
    std::cout << v << " ";
  std::cout << "\nResult of pair_mul: ";
  for (auto v : resultMul)
    std::cout << v << " ";
  std::cout << std::endl;

  return 0;
}
