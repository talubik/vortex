## Fix `__local` argument pointer generation in pocl Vortex device

When I tried to run OpenCL kernels with `__local` memory arguments, I found that the generated argument pointers were incorrect. This Pull Request fixes two bugs in the LLVM IR generation for `__local` kernel arguments.

### Bug 1: `ArgOffset` not updated before reading per-argument offset

When processing the first `__local` argument, `ArgOffset` was captured before reading `__local_size` from the argument buffer. After `__local_size` is read, `arg_offset` advances by 4 bytes, but the cached `ArgOffset` constant is never updated. As a result, the per-argument offset field is read from the same position as `__local_size`, returning the total local memory size instead of the actual offset (which should be 0 for the first argument).

### Bug 2: `I8PtrTy` used instead of `I8Ty` in GEP

The GEP instruction that applies the per-argument offset to the local memory base pointer used `I8PtrTy` (pointer type, 8 bytes on 64-bit) as the element type instead of `I8Ty` (1 byte). This caused the offset to be scaled by 8, placing the pointer far past the correct location in the allocated local memory block.

### Effect of each bug (local_size = 4, `B - A` expected = 4)

| Bugs present | `B - A` result |
|---|---|
| Both bugs | -32 |
| Bug 2 only (I8PtrTy) | +32 |
| Bug 1 only (ArgOffset) | -4 |
| Fixed (this PR) | **4 ✓** |

I also added a test `tests/opencl/lmemtest` which verifies the correct pointer layout of two `__local` arguments by checking that `B - A == local_work_size`. The test successfully passes with this fix and fails under all three buggy configurations.
