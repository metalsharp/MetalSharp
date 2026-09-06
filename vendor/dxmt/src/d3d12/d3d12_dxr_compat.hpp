#pragma once

#include "d3d12.h"

#include <cstdint>
#include <cstring>

namespace dxmt {

#if defined(_WIN32)

// Agility 1.619.5 extends the DXR ABI without changing the size of the
// existing geometry/input unions.  The source build intentionally uses the
// older MinGW D3D12 header, so keep the new wire values and POD records local
// to the provider instead of replacing the public header underneath callers.
constexpr UINT kD3D12RaytracingGeometryTypeOmmTriangles = 2u;
constexpr UINT kD3D12RaytracingAccelerationStructureTypeOmmArray = 2u;
// Agility 1.619.5 / D3D12 OMM alignment is 128 bytes for raw input and
// array storage.  The build scratch address is still governed by the normal
// acceleration-structure alignment (256 bytes).  NVAPI's separate OMM
// extension uses a 256-byte OMM-array rule and is not this D3D12 ABI.
constexpr UINT kD3D12RaytracingAccelerationStructureByteAlignment = 256u;
constexpr UINT kD3D12RaytracingOmmArrayByteAlignment = 128u;
constexpr UINT kD3D12RaytracingOmmDescsByteAlignment = 4u;
constexpr UINT kD3D12RaytracingBuildFlagAllowUpdate = 0x1u;
constexpr UINT kD3D12RaytracingBuildFlagAllowCompaction = 0x2u;
constexpr UINT kD3D12RaytracingBuildFlagPerformUpdate = 0x20u;
constexpr UINT kD3D12RaytracingBuildFlagAllowOmmUpdate = 0x40u;
constexpr UINT kD3D12RaytracingBuildFlagAllowDisableOmms = 0x80u;
constexpr UINT kD3D12RaytracingInstanceFlagForceOmm2State = 0x10u;
constexpr UINT kD3D12RaytracingInstanceFlagDisableOmms = 0x20u;
constexpr UINT kD3D12RaytracingPipelineFlagSkipTriangles = 0x100u;
constexpr UINT kD3D12RaytracingPipelineFlagSkipProceduralPrimitives = 0x200u;
constexpr UINT kD3D12RaytracingPipelineFlagAllowOpacityMicromaps = 0x400u;
constexpr UINT kD3D12RaytracingPipelineConfig1SubobjectType = 12u;
constexpr UINT kD3D12RaytracingOmmFormatOc1TwoState = 0x1u;
constexpr UINT kD3D12RaytracingOmmFormatOc1FourState = 0x2u;

// The bounded provider only decodes the two-state OC1 representation.  Keep
// the four-state ABI value above for validation and future matrix coverage,
// but never accept it as if states 2/3 had native any-hit semantics.
inline bool D3D12IsSupportedOmmFormat(UINT format) {
  return format == kD3D12RaytracingOmmFormatOc1TwoState;
}

constexpr int32_t kD3D12RaytracingOmmSpecialFullyTransparent = -1;
constexpr int32_t kD3D12RaytracingOmmSpecialFullyOpaque = -2;
constexpr int32_t kD3D12RaytracingOmmSpecialUnknownTransparent = -3;
constexpr int32_t kD3D12RaytracingOmmSpecialUnknownOpaque = -4;

struct D3D12RaytracingPipelineConfig1Compat {
  UINT max_trace_recursion_depth;
  UINT flags;
};
static_assert(sizeof(D3D12RaytracingPipelineConfig1Compat) == 8,
              "D3D12 raytracing pipeline CONFIG1 ABI");

struct D3D12OpacityMicromapDescCompat {
  UINT byte_offset;
  UINT subdivision_level : 16;
  UINT format : 16;
};
static_assert(sizeof(D3D12OpacityMicromapDescCompat) == 8,
              "D3D12 OMM descriptor ABI");

struct D3D12OpacityMicromapLinkageDescCompat {
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE index_buffer;
  DXGI_FORMAT index_format;
  UINT base_location;
  D3D12_GPU_VIRTUAL_ADDRESS opacity_micromap_array;
};
static_assert(sizeof(D3D12OpacityMicromapLinkageDescCompat) == 32,
              "D3D12 OMM linkage ABI");

struct D3D12OpacityMicromapTrianglesDescCompat {
  const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC *triangles;
  const D3D12OpacityMicromapLinkageDescCompat *linkage;
};
static_assert(sizeof(D3D12OpacityMicromapTrianglesDescCompat) == 16,
              "D3D12 OMM triangles ABI");

struct D3D12OpacityMicromapHistogramEntryCompat {
  UINT count;
  UINT subdivision_level;
  UINT format;
};
static_assert(sizeof(D3D12OpacityMicromapHistogramEntryCompat) == 12,
              "D3D12 OMM histogram ABI");

struct D3D12OpacityMicromapArrayDescCompat {
  UINT histogram_entry_count;
  const D3D12OpacityMicromapHistogramEntryCompat *histogram;
  D3D12_GPU_VIRTUAL_ADDRESS input_buffer;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE per_omm_descs;
};
static_assert(sizeof(D3D12OpacityMicromapArrayDescCompat) == 40,
              "D3D12 OMM array ABI");

// The old and new SDKs use the same union slot for the OMM array descriptor
// and for pGeometryDescs.  These helpers document that deliberate ABI cast at
// each boundary and keep it out of the normal triangle/AABB paths.
inline const D3D12OpacityMicromapArrayDescCompat *
D3D12GetOpacityMicromapArrayDesc(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs) {
  return reinterpret_cast<const D3D12OpacityMicromapArrayDescCompat *>(
      inputs.pGeometryDescs);
}

inline D3D12OpacityMicromapTrianglesDescCompat
D3D12GetOpacityMicromapTrianglesDesc(
    const D3D12_RAYTRACING_GEOMETRY_DESC &geometry) {
  D3D12OpacityMicromapTrianglesDescCompat value = {};
  static_assert(sizeof(value) == sizeof(geometry.Triangles.Transform3x4) * 2,
                "OMM geometry union must remain two pointers");
  std::memcpy(&value, &geometry.Triangles, sizeof(value));
  return value;
}

#else

// The native DXIL contract tests use Wine's older WIDL D3D12 header.  That
// header intentionally stops before the DXR 1.1/enhanced-barrier/shading-rate
// PODs which are present in the cross-build MinGW header.  Keep the test-side
// declarations local to the provider namespace rather than editing or
// replacing the public WIDL header.  These declarations mirror the Agility
// layouts used by Cmd* records; they are not exported API declarations.
struct ID3D12StateObject;
struct ID3D12ProtectedResourceSession;
struct ID3D12MetaCommand;
struct D3D12OpacityMicromapLinkageDescCompat;

struct D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE {
  D3D12_GPU_VIRTUAL_ADDRESS StartAddress;
  UINT64 StrideInBytes;
};

struct D3D12_GPU_VIRTUAL_ADDRESS_RANGE {
  D3D12_GPU_VIRTUAL_ADDRESS StartAddress;
  UINT64 SizeInBytes;
};

struct D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE {
  D3D12_GPU_VIRTUAL_ADDRESS StartAddress;
  UINT64 SizeInBytes;
  UINT64 StrideInBytes;
};

enum D3D12_RAYTRACING_GEOMETRY_FLAGS {
  D3D12_RAYTRACING_GEOMETRY_FLAG_NONE = 0,
  D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE = 0x1,
  D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION = 0x2,
};

enum D3D12_RAYTRACING_GEOMETRY_TYPE {
  D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES = 0,
  D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS = 1,
  D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES = 2,
};

struct D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC {
  D3D12_GPU_VIRTUAL_ADDRESS Transform3x4;
  DXGI_FORMAT IndexFormat;
  DXGI_FORMAT VertexFormat;
  UINT IndexCount;
  UINT VertexCount;
  D3D12_GPU_VIRTUAL_ADDRESS IndexBuffer;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE VertexBuffer;
};

struct D3D12_RAYTRACING_GEOMETRY_AABBS_DESC {
  UINT64 AABBCount;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE AABBs;
};

struct D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC {
  const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC *pTriangles;
  const D3D12OpacityMicromapLinkageDescCompat *pOmmLinkage;
};

struct D3D12_RAYTRACING_GEOMETRY_DESC {
  D3D12_RAYTRACING_GEOMETRY_TYPE Type;
  D3D12_RAYTRACING_GEOMETRY_FLAGS Flags;
  union {
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC Triangles;
    D3D12_RAYTRACING_GEOMETRY_AABBS_DESC AABBs;
    D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC OmmTriangles;
  };
};

enum D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE = 0,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE = 0x1,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION = 0x2,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE = 0x4,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD = 0x8,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY = 0x10,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE = 0x20,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE = 0x40,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS = 0x80,
};

enum D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE = 0,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT = 1,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS = 2,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE = 3,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE = 4,
};

enum D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL = 0,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL = 1,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY = 2,
};

enum D3D12_ELEMENTS_LAYOUT {
  D3D12_ELEMENTS_LAYOUT_ARRAY = 0,
  D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS = 1,
};

enum D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE = 0,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION = 1,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION = 2,
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE = 3,
};

struct D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE Type;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags;
  UINT NumDescs;
  D3D12_ELEMENTS_LAYOUT DescsLayout;
  union {
    D3D12_GPU_VIRTUAL_ADDRESS InstanceDescs;
    const D3D12_RAYTRACING_GEOMETRY_DESC *pGeometryDescs;
    const D3D12_RAYTRACING_GEOMETRY_DESC *const *ppGeometryDescs;
  };
};

struct D3D12_DISPATCH_RAYS_DESC {
  D3D12_GPU_VIRTUAL_ADDRESS_RANGE RayGenerationShaderRecord;
  D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE MissShaderTable;
  D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE HitGroupTable;
  D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE CallableShaderTable;
  UINT Width;
  UINT Height;
  UINT Depth;
};

using D3D12_BARRIER_SYNC = UINT64;
using D3D12_BARRIER_ACCESS = UINT64;
using D3D12_BARRIER_LAYOUT = UINT32;
enum D3D12_BARRIER_TYPE { D3D12_BARRIER_TYPE_GLOBAL = 0,
                          D3D12_BARRIER_TYPE_TEXTURE = 1,
                          D3D12_BARRIER_TYPE_BUFFER = 2 };
enum D3D12_TEXTURE_BARRIER_FLAGS {
  D3D12_TEXTURE_BARRIER_FLAG_NONE = 0,
  D3D12_TEXTURE_BARRIER_FLAG_DISCARD = 1,
};
struct D3D12_BARRIER_SUBRESOURCE_RANGE {
  UINT IndexOrFirstMipLevel;
  UINT NumMipLevels;
  UINT FirstArraySlice;
  UINT NumArraySlices;
  UINT FirstPlane;
  UINT NumPlanes;
};
struct D3D12_GLOBAL_BARRIER {
  D3D12_BARRIER_SYNC SyncBefore;
  D3D12_BARRIER_SYNC SyncAfter;
  D3D12_BARRIER_ACCESS AccessBefore;
  D3D12_BARRIER_ACCESS AccessAfter;
};
struct D3D12_TEXTURE_BARRIER {
  D3D12_BARRIER_SYNC SyncBefore;
  D3D12_BARRIER_SYNC SyncAfter;
  D3D12_BARRIER_ACCESS AccessBefore;
  D3D12_BARRIER_ACCESS AccessAfter;
  D3D12_BARRIER_LAYOUT LayoutBefore;
  D3D12_BARRIER_LAYOUT LayoutAfter;
  ID3D12Resource *pResource;
  D3D12_BARRIER_SUBRESOURCE_RANGE Subresources;
  D3D12_TEXTURE_BARRIER_FLAGS Flags;
};
struct D3D12_BUFFER_BARRIER {
  D3D12_BARRIER_SYNC SyncBefore;
  D3D12_BARRIER_SYNC SyncAfter;
  D3D12_BARRIER_ACCESS AccessBefore;
  D3D12_BARRIER_ACCESS AccessAfter;
  ID3D12Resource *pResource;
  UINT64 Offset;
  UINT64 Size;
};

enum D3D12_SHADING_RATE {
  D3D12_SHADING_RATE_1X1 = 0,
  D3D12_SHADING_RATE_1X2 = 1,
  D3D12_SHADING_RATE_2X1 = 4,
  D3D12_SHADING_RATE_2X2 = 5,
  D3D12_SHADING_RATE_2X4 = 6,
  D3D12_SHADING_RATE_4X2 = 9,
  D3D12_SHADING_RATE_4X4 = 10,
};
enum D3D12_SHADING_RATE_COMBINER {
  D3D12_SHADING_RATE_COMBINER_PASSTHROUGH = 0,
  D3D12_SHADING_RATE_COMBINER_OVERRIDE = 1,
  D3D12_SHADING_RATE_COMBINER_MIN = 2,
  D3D12_SHADING_RATE_COMBINER_MAX = 3,
  D3D12_SHADING_RATE_COMBINER_SUM = 4,
};

constexpr UINT kD3D12RaytracingGeometryTypeOmmTriangles = 2u;
constexpr UINT kD3D12RaytracingAccelerationStructureTypeOmmArray = 2u;
constexpr UINT kD3D12RaytracingAccelerationStructureByteAlignment = 256u;
constexpr UINT kD3D12RaytracingOmmArrayByteAlignment = 128u;
constexpr UINT kD3D12RaytracingOmmDescsByteAlignment = 4u;
constexpr UINT kD3D12RaytracingBuildFlagAllowUpdate = 0x1u;
constexpr UINT kD3D12RaytracingBuildFlagAllowCompaction = 0x2u;
constexpr UINT kD3D12RaytracingBuildFlagPerformUpdate = 0x20u;
constexpr UINT kD3D12RaytracingBuildFlagAllowOmmUpdate = 0x40u;
constexpr UINT kD3D12RaytracingBuildFlagAllowDisableOmms = 0x80u;
constexpr UINT kD3D12RaytracingInstanceFlagForceOmm2State = 0x10u;
constexpr UINT kD3D12RaytracingInstanceFlagDisableOmms = 0x20u;
constexpr UINT kD3D12RaytracingPipelineFlagSkipTriangles = 0x100u;
constexpr UINT kD3D12RaytracingPipelineFlagSkipProceduralPrimitives = 0x200u;
constexpr UINT kD3D12RaytracingPipelineFlagAllowOpacityMicromaps = 0x400u;
constexpr UINT kD3D12RaytracingPipelineConfig1SubobjectType = 12u;
constexpr UINT kD3D12RaytracingOmmFormatOc1TwoState = 0x1u;
constexpr UINT kD3D12RaytracingOmmFormatOc1FourState = 0x2u;

inline bool D3D12IsSupportedOmmFormat(UINT format) {
  return format == kD3D12RaytracingOmmFormatOc1TwoState;
}

struct D3D12RaytracingPipelineConfig1Compat {
  UINT max_trace_recursion_depth;
  UINT flags;
};
static_assert(sizeof(D3D12RaytracingPipelineConfig1Compat) == 8,
              "D3D12 raytracing pipeline CONFIG1 ABI");

struct D3D12OpacityMicromapDescCompat {
  UINT byte_offset;
  UINT subdivision_level : 16;
  UINT format : 16;
};
static_assert(sizeof(D3D12OpacityMicromapDescCompat) == 8,
              "D3D12 OMM descriptor ABI");

struct D3D12OpacityMicromapLinkageDescCompat {
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE index_buffer;
  DXGI_FORMAT index_format;
  UINT base_location;
  D3D12_GPU_VIRTUAL_ADDRESS opacity_micromap_array;
};
static_assert(sizeof(D3D12OpacityMicromapLinkageDescCompat) == 32,
              "D3D12 OMM linkage ABI");

struct D3D12OpacityMicromapHistogramEntryCompat {
  UINT count;
  UINT subdivision_level;
  UINT format;
};
static_assert(sizeof(D3D12OpacityMicromapHistogramEntryCompat) == 12,
              "D3D12 OMM histogram ABI");

struct D3D12OpacityMicromapArrayDescCompat {
  UINT histogram_entry_count;
  const D3D12OpacityMicromapHistogramEntryCompat *histogram;
  D3D12_GPU_VIRTUAL_ADDRESS input_buffer;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE per_omm_descs;
};
static_assert(sizeof(D3D12OpacityMicromapArrayDescCompat) == 40,
              "D3D12 OMM array ABI");

#endif // defined(_WIN32)

} // namespace dxmt
