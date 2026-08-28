#pragma once

#include "d3d12.h"
#include <atomic>
#include <cstdint>

namespace dxmt {

class ResidencyState {
public:
  explicit ResidencyState(bool resident = true) : resident_(resident) {}

  bool isResident() const {
    return resident_.load(std::memory_order_acquire);
  }

  void makeResident() { resident_.store(true, std::memory_order_release); }

  void evict() { resident_.store(false, std::memory_order_release); }

  D3D12_RESIDENCY_PRIORITY priority() const {
    return static_cast<D3D12_RESIDENCY_PRIORITY>(
        priority_.load(std::memory_order_acquire));
  }

  void setPriority(D3D12_RESIDENCY_PRIORITY priority) {
    priority_.store(static_cast<uint32_t>(priority), std::memory_order_release);
  }

private:
  std::atomic<bool> resident_;
  std::atomic<uint32_t> priority_ = {
      static_cast<uint32_t>(D3D12_RESIDENCY_PRIORITY_NORMAL)};
};

} // namespace dxmt
