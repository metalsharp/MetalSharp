#pragma once

#include "d3d12.h"
#include <atomic>
#include <cstdint>

namespace dxmt {

class ResidencyState {
public:
  // D3D12 residency is reference-counted: objects start with one resident
  // reference unless CREATE_NOT_RESIDENT was requested, and Evict only takes
  // effect after the matching number of MakeResident calls.
  explicit ResidencyState(bool resident = true)
      : resident_references_(resident ? 1u : 0u) {}

  bool isResident() const {
    return resident_references_.load(std::memory_order_acquire) != 0;
  }

  void makeResident() {
    uint32_t current = resident_references_.load(std::memory_order_acquire);
    while (current != UINT32_MAX &&
           !resident_references_.compare_exchange_weak(
               current, current + 1, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
  }

  void evict() {
    uint32_t current = resident_references_.load(std::memory_order_acquire);
    while (current != 0 &&
           !resident_references_.compare_exchange_weak(
               current, current - 1, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
  }
  D3D12_RESIDENCY_PRIORITY priority() const {
    return static_cast<D3D12_RESIDENCY_PRIORITY>(
        priority_.load(std::memory_order_acquire));
  }

  void setPriority(D3D12_RESIDENCY_PRIORITY priority) {
    priority_.store(static_cast<uint32_t>(priority), std::memory_order_release);
  }

private:
  std::atomic<uint32_t> resident_references_;
  std::atomic<uint32_t> priority_ = {
      static_cast<uint32_t>(D3D12_RESIDENCY_PRIORITY_NORMAL)};
};

} // namespace dxmt
