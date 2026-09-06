#include "../../src/d3d12/d3d12_dxr_compat.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

int main() {
  try {
    static_assert(sizeof(dxmt::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS) ==
                      24,
                  "DXR build-input union ABI drifted");
    static_assert(sizeof(dxmt::D3D12OpacityMicromapTrianglesDescCompat) == 16,
                  "OMM geometry ABI drifted");

    dxmt::D3D12OpacityMicromapArrayDescCompat array = {};
    dxmt::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.pOpacityMicromapArrayDesc = &array;
    require(dxmt::D3D12GetOpacityMicromapArrayDesc(inputs) == &array,
            "named OMM array union member was not preserved");

    dxmt::D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles = {};
    dxmt::D3D12OpacityMicromapLinkageDescCompat linkage = {};
    dxmt::D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.OmmTriangles.pTriangles = &triangles;
    geometry.OmmTriangles.pOmmLinkage = &linkage;
    const auto decoded = dxmt::D3D12GetOpacityMicromapTrianglesDesc(geometry);
    require(decoded.triangles == &triangles && decoded.linkage == &linkage,
            "OMM geometry union decoding changed");

    require(dxmt::D3D12IsSupportedOmmFormat(
                dxmt::kD3D12RaytracingOmmFormatOc1TwoState),
            "OC1 two-state format rejected");
    require(!dxmt::D3D12IsSupportedOmmFormat(
                dxmt::kD3D12RaytracingOmmFormatOc1FourState),
            "OC1 four-state format promoted accidentally");
    std::cout << "PASS: local WIDL DXR OMM declarations\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
