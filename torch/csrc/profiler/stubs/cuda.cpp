#include <sstream>

#ifndef FBCODE_CAFFE2
#include <nvtx3/nvToolsExt.h>
#else
#include <nvToolsExt.h>
#endif

#include <c10/cuda/CUDAGuard.h>
#include <c10/util/irange.h>
#include <torch/csrc/profiler/stubs/base.h>
#include <torch/csrc/profiler/util.h>

namespace torch {
namespace profiler {
namespace impl {
namespace {

static inline void cudaCheck(cudaError_t result, const char* file, int line) {
  if (result != cudaSuccess) {
    std::stringstream ss;
    ss << file << ":" << line << ": ";
    if (result == cudaErrorInitializationError) {
      // It is common for users to use DataLoader with multiple workers
      // and the autograd profiler. Throw a nice error message here.
      ss << "CUDA initialization error. "
         << "This can occur if one runs the profiler in CUDA mode on code "
         << "that creates a DataLoader with num_workers > 0. This operation "
         << "is currently unsupported; potential workarounds are: "
         << "(1) don't use the profiler in CUDA mode or (2) use num_workers=0 "
         << "in the DataLoader or (3) Don't profile the data loading portion "
         << "of your code. https://github.com/pytorch/pytorch/issues/6313 "
         << "tracks profiler support for multi-worker DataLoader.";
    } else {
      ss << cudaGetErrorString(result);
    }
    throw std::runtime_error(ss.str());
  }
}
#define TORCH_CUDA_CHECK(result) cudaCheck(result, __FILE__, __LINE__);

struct CUDAMethods : public ProfilerStubs {
  void record(int* device, ProfilerEventStub* event, int64_t* cpu_ns)
      const override {
    if (device) {
      TORCH_CUDA_CHECK(cudaGetDevice(device));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    CUevent_st* cuda_event_ptr;
    TORCH_CUDA_CHECK(cudaEventCreate(&cuda_event_ptr));
    *event = std::shared_ptr<CUevent_st>(cuda_event_ptr, [](CUevent_st* ptr) {
      TORCH_CUDA_CHECK(cudaEventDestroy(ptr));
    });
    auto stream = at::cuda::getCurrentCUDAStream();
    if (cpu_ns) {
      *cpu_ns = torch::profiler::impl::getTime();
    }
    TORCH_CUDA_CHECK(cudaEventRecord(cuda_event_ptr, stream));
  }

  float elapsed(const ProfilerEventStub* event, const ProfilerEventStub* event2)
      const override {
    TORCH_CUDA_CHECK(cudaEventSynchronize(event->get()));
    TORCH_CUDA_CHECK(cudaEventSynchronize(event2->get()));
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    float ms;
    TORCH_CUDA_CHECK(cudaEventElapsedTime(&ms, event->get(), event2->get()));
    // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-avoid-magic-numbers,cppcoreguidelines-narrowing-conversions)
    return ms * 1000.0;
  }

  void mark(const char* name) const override {
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    ::nvtxMark(name);
  }

  void rangePush(const char* name) const override {
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    ::nvtxRangePushA(name);
  }

  void rangePop() const override {
    ::nvtxRangePop();
  }

  void onEachDevice(std::function<void(int)> op) const override {
    at::cuda::OptionalCUDAGuard device_guard;
    for (const auto i : c10::irange(at::cuda::device_count())) {
      device_guard.set_index(i);
      op(i);
    }
  }

  void synchronize() const override {
    TORCH_CUDA_CHECK(cudaDeviceSynchronize());
  }

  bool enabled() const override {
    return true;
  }
};

struct RegisterCUDAMethods {
  RegisterCUDAMethods() {
    static CUDAMethods methods;
    registerCUDAMethods(&methods);
  }
};
RegisterCUDAMethods reg;

} // namespace
} // namespace impl
} // namespace profiler
} // namespace torch
