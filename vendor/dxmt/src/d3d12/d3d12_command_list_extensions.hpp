#pragma once

#include "d3d12.h"

namespace dxmt {

// MinGW's D3D12 header currently stops at GraphicsCommandList7, while the
// stable Agility 1.619.5 contract adds this command-list interface. Keep the
// exact inherited vtable extension local to DXMT until the toolchain header
// catches up. The GUID and method order are part of the public ABI.
static const GUID kID3D12GraphicsCommandList8 = {
    0xee936ef9, 0x599d, 0x4d28,
    {0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51}};

struct GraphicsCommandList8Extension : public ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(
      UINT front_stencil_ref, UINT back_stencil_ref) = 0;
};

static const GUID kID3D12GraphicsCommandList9 = {
    0x34ed2808, 0xffe6, 0x4c2b,
    {0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1}};

struct GraphicsCommandList9Extension : public GraphicsCommandList8Extension {
  virtual void STDMETHODCALLTYPE RSSetDepthBias(
      FLOAT depth_bias, FLOAT depth_bias_clamp,
      FLOAT slope_scaled_depth_bias) = 0;
  virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(
      D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value) = 0;
};

} // namespace dxmt
