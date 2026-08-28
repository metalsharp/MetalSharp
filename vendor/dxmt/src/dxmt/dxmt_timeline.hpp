#pragma once

#include "Metal.hpp"
#include "util_cpu_fence.hpp"
#include <atomic>
#include <cstdint>

namespace dxmt {

// Shared ordering primitive for CPU-visible completion and Metal event
// synchronization.  The D3D12 queue keeps its existing public event handle
// for ABI compatibility, while this object centralizes reservation and
// validation for new providers.
class ExecutionTimeline {
public:
  ExecutionTimeline() = default;

  explicit ExecutionTimeline(CpuFence &cpu_fence) : cpu_fence_(&cpu_fence) {}

  ExecutionTimeline(WMT::Device device, CpuFence &cpu_fence) :
      cpu_fence_(&cpu_fence),
      event_(device.newSharedEvent()) {}

  bool
  valid() const {
    return event_.handle != NULL_OBJECT_HANDLE;
  }

  uint64_t
  reserve() {
    return next_value_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  uint64_t
  reservedValue() const {
    return next_value_.load(std::memory_order_acquire);
  }

  uint64_t
  completedValue() const {
    return cpu_fence_ ? cpu_fence_->signaledValue() : 0;
  }

  void
  waitCPU(uint64_t value) const {
    if (cpu_fence_)
      cpu_fence_->wait(value);
  }

  void
  completeCPU(uint64_t value) {
    if (cpu_fence_)
      cpu_fence_->signal(value);
  }

  bool
  canEncode(WMT::CommandBuffer command_buffer, uint64_t value) const {
    return valid() && command_buffer.handle != NULL_OBJECT_HANDLE && value != 0;
  }

  bool
  encodeWait(WMT::CommandBuffer command_buffer, uint64_t value) const {
    if (!canEncode(command_buffer, value))
      return false;
    command_buffer.encodeWaitForEvent(event_, value);
    return true;
  }

  bool
  encodeSignal(WMT::CommandBuffer command_buffer, uint64_t value) const {
    if (!canEncode(command_buffer, value))
      return false;
    command_buffer.encodeSignalEvent(event_, value);
    return true;
  }

  WMT::Reference<WMT::SharedEvent>
  sharedEvent() const {
    return event_;
  }

private:
  CpuFence *cpu_fence_ = nullptr;
  WMT::Reference<WMT::SharedEvent> event_;
  std::atomic<uint64_t> next_value_ = 0;
};

} // namespace dxmt
