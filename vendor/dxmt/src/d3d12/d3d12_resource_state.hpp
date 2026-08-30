#pragma once

#include "d3d12.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace dxmt {

struct ResourceStateSnapshot {
  D3D12_RESOURCE_STATES legacy_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_COMMON;
  uint64_t generation = 0;
  D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;
};

// Tracks the D3D12-visible state independently of any Metal encoder.  A
// command list can therefore record transitions before an encoder exists and
// a later queue/provider can validate and apply them at replay time.
class ResourceStateTracker {
public:
  explicit ResourceStateTracker(
      D3D12_RESOURCE_STATES initial_state,
      D3D12_BARRIER_LAYOUT initial_layout = D3D12_BARRIER_LAYOUT_COMMON)
      : global_{initial_state, initial_layout, 0} {}

  ResourceStateSnapshot
  snapshot(UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) const {
    std::lock_guard lock(mutex_);
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
      return global_;
    auto it = subresources_.find(subresource);
    return it == subresources_.end() ? global_ : it->second;
  }

  // before == ALL_SUBRESOURCES is treated as a validation wildcard.  The
  // caller can use the returned bool to surface a stale-state transition
  // rather than silently accepting it.
  bool transitionLegacy(UINT subresource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after) {
    std::lock_guard lock(mutex_);
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
      if (!matchesAllLegacy(before))
        return false;
      global_.legacy_state = after;
      global_.layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
      global_.generation++;
      subresources_.clear();
      return true;
    }

    auto &state = subresources_[subresource];
    if (state.generation == 0)
      state = global_;
    if (state.legacy_state != before)
      return false;
    state.legacy_state = after;
    state.layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
    state.generation = ++generation_;
    return true;
  }

  bool transitionLayout(UINT subresource, D3D12_BARRIER_LAYOUT before,
                        D3D12_BARRIER_LAYOUT after) {
    std::lock_guard lock(mutex_);
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
      if (!matchesAllLayout(before))
        return false;
      global_.layout = after;
      global_.generation = ++generation_;
      subresources_.clear();
      return true;
    }

    auto &state = subresources_[subresource];
    if (state.generation == 0)
      state = global_;
    if (before != D3D12_BARRIER_LAYOUT_UNDEFINED && state.layout != before)
      return false;
    state.layout = after;
    state.generation = ++generation_;
    return true;
  }

  // Enhanced buffer barriers carry byte-range/access information rather than
  // a texture layout.  Keep the last access and generation visible even
  // though Metal's encoder model does not need an explicit buffer layout.
  bool transitionEnhancedAccess(D3D12_BARRIER_ACCESS before,
                                D3D12_BARRIER_ACCESS after,
                                uint64_t offset, uint64_t size) {
    (void)before;
    (void)offset;
    (void)size;
    std::lock_guard lock(mutex_);
    global_.access = after;
    global_.generation = ++generation_;
    return true;
  }

  void markAliased() {
    std::lock_guard lock(mutex_);
    global_.legacy_state = D3D12_RESOURCE_STATE_COMMON;
    global_.layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
    global_.generation = ++generation_;
    subresources_.clear();
  }

  uint64_t generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
  }

private:
  bool matchesAllLegacy(D3D12_RESOURCE_STATES before) const {
    if (!subresources_.empty()) {
      for (const auto &entry : subresources_) {
        if (entry.second.legacy_state != before)
          return false;
      }
      return true;
    }
    return global_.legacy_state == before;
  }

  bool matchesAllLayout(D3D12_BARRIER_LAYOUT before) const {
    if (before == D3D12_BARRIER_LAYOUT_UNDEFINED)
      return true;
    if (global_.layout == before)
      return true;
    for (const auto &entry : subresources_) {
      if (entry.second.layout != before)
        return false;
    }
    return subresources_.empty();
  }

  mutable std::mutex mutex_;
  ResourceStateSnapshot global_;
  std::unordered_map<UINT, ResourceStateSnapshot> subresources_;
  uint64_t generation_ = 0;
};

} // namespace dxmt
