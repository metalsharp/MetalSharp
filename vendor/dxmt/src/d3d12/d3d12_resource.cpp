#include "d3d12_resource.hpp"
#include "d3d12_device.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_pipeline_state.hpp"
#include "d3d12_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstring>

#define RTRACE(fmt, ...) DXMTD3D12Trace("Resource", fmt, ##__VA_ARGS__)

namespace dxmt {

namespace {

static bool IsCPUAccessibleHeap(
    const D3D12_HEAP_PROPERTIES &properties) {
  const UINT type = static_cast<UINT>(properties.Type);
  if (type == static_cast<UINT>(D3D12_HEAP_TYPE_UPLOAD) ||
      type == static_cast<UINT>(D3D12_HEAP_TYPE_READBACK) || type == 5)
    return true;
  return type == static_cast<UINT>(D3D12_HEAP_TYPE_CUSTOM) &&
         (properties.CPUPageProperty ==
              D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
          properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
}

static bool SubmitBufferCopy(WMT::Device device, WMT::Buffer source,
                             uint64_t source_offset, WMT::Buffer destination,
                             uint64_t destination_offset, uint64_t length) {
  if (!device.handle || !source.handle || !destination.handle || !length)
    return false;
  auto queue = device.newCommandQueue(1);
  if (!queue.handle)
    return false;
  auto command_buffer = queue.commandBuffer();
  if (!command_buffer.handle)
    return false;
  auto blit = command_buffer.blitCommandEncoder();
  if (!blit.handle)
    return false;
  wmtcmd_blit_copy_from_buffer_to_buffer command = {};
  command.type = WMTBlitCommandCopyFromBufferToBuffer;
  command.next.set(nullptr);
  command.src = source.handle;
  command.src_offset = source_offset;
  command.dst = destination.handle;
  command.dst_offset = destination_offset;
  command.copy_length = length;
  if (!blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&command))) {
    blit.endEncoding();
    return false;
  }
  blit.endEncoding();
  command_buffer.commit();
  command_buffer.waitUntilCompleted();
  return command_buffer.status() != WMTCommandBufferStatusError;
}

static bool SubmitTextureUpload(WMT::Device device, WMT::Texture destination,
                                uint32_t slice, uint32_t level,
                                WMTOrigin origin, WMTSize size,
                                const void *source, uint64_t source_length,
                                uint32_t bytes_per_row,
                                uint32_t bytes_per_image) {
  if (!device.handle || !destination.handle || !source || !source_length ||
      !bytes_per_row || !bytes_per_image)
    return false;
  WMTBufferInfo staging_info = {};
  staging_info.length = source_length;
  staging_info.options = WMTResourceStorageModeShared;
  auto staging = device.newBuffer(staging_info);
  void *staging_data = staging_info.memory.get_accessible_or_null();
  if (!staging.handle || !staging_data)
    return false;
  std::memcpy(staging_data, source, static_cast<size_t>(source_length));
  auto queue = device.newCommandQueue(1);
  if (!queue.handle)
    return false;
  auto command_buffer = queue.commandBuffer();
  if (!command_buffer.handle)
    return false;
  auto blit = command_buffer.blitCommandEncoder();
  if (!blit.handle)
    return false;
  wmtcmd_blit_copy_from_buffer_to_texture command = {};
  command.type = WMTBlitCommandCopyFromBufferToTexture;
  command.next.set(nullptr);
  command.src = staging.handle;
  command.src_offset = 0;
  command.bytes_per_row = bytes_per_row;
  command.bytes_per_image = bytes_per_image;
  command.size = size;
  command.dst = destination.handle;
  command.slice = slice;
  command.level = level;
  command.origin = origin;
  if (!blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&command))) {
    blit.endEncoding();
    return false;
  }
  blit.endEncoding();
  command_buffer.commit();
  command_buffer.waitUntilCompleted();
  return command_buffer.status() != WMTCommandBufferStatusError;
}

static bool SubmitTextureReadback(WMT::Device device, WMT::Texture source,
                                  uint32_t slice, uint32_t level,
                                  WMTOrigin origin, WMTSize size,
                                  WMT::Buffer destination, uint64_t offset,
                                  uint32_t bytes_per_row,
                                  uint32_t bytes_per_image) {
  if (!device.handle || !source.handle || !destination.handle ||
      !bytes_per_row || !bytes_per_image)
    return false;
  auto queue = device.newCommandQueue(1);
  if (!queue.handle)
    return false;
  auto command_buffer = queue.commandBuffer();
  if (!command_buffer.handle)
    return false;
  auto blit = command_buffer.blitCommandEncoder();
  if (!blit.handle)
    return false;
  wmtcmd_blit_copy_from_texture_to_buffer command = {};
  command.type = WMTBlitCommandCopyFromTextureToBuffer;
  command.next.set(nullptr);
  command.src = source.handle;
  command.slice = slice;
  command.level = level;
  command.origin = origin;
  command.size = size;
  command.dst = destination.handle;
  command.offset = offset;
  command.bytes_per_row = bytes_per_row;
  command.bytes_per_image = bytes_per_image;
  if (!blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&command))) {
    blit.endEncoding();
    return false;
  }
  blit.endEncoding();
  command_buffer.commit();
  command_buffer.waitUntilCompleted();
  return command_buffer.status() != WMTCommandBufferStatusError;
}

static bool IsBlockCompressedFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC2_TYPELESS:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS:
  case DXGI_FORMAT_BC4_UNORM:
  case DXGI_FORMAT_BC4_SNORM:
  case DXGI_FORMAT_BC5_TYPELESS:
  case DXGI_FORMAT_BC5_UNORM:
  case DXGI_FORMAT_BC5_SNORM:
  case DXGI_FORMAT_BC6H_TYPELESS:
  case DXGI_FORMAT_BC6H_UF16:
  case DXGI_FORMAT_BC6H_SF16:
  case DXGI_FORMAT_BC7_TYPELESS:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return true;
  default:
    return false;
  }
}

static uint32_t PackedTextureRowCount(DXGI_FORMAT format,
                                      uint64_t height) {
  return static_cast<uint32_t>(
      IsBlockCompressedFormat(format) ? std::max<uint64_t>(1, (height + 3) / 4)
                                      : std::max<uint64_t>(1, height));
}

static uint32_t PackedTextureRowBytes(DXGI_FORMAT format, uint64_t width) {
  if (IsBlockCompressedFormat(format)) {
    const uint64_t blocks = std::max<uint64_t>(1, (width + 3) / 4);
    const bool eight_byte_block =
        format == DXGI_FORMAT_BC1_TYPELESS ||
        format == DXGI_FORMAT_BC1_UNORM ||
        format == DXGI_FORMAT_BC1_UNORM_SRGB ||
        format == DXGI_FORMAT_BC4_TYPELESS ||
        format == DXGI_FORMAT_BC4_UNORM ||
        format == DXGI_FORMAT_BC4_SNORM;
    return static_cast<uint32_t>(blocks * (eight_byte_block ? 8 : 16));
  }

  switch (format) {
  case DXGI_FORMAT_B5G6R5_UNORM:
  case DXGI_FORMAT_B5G5R5A1_UNORM:
  case DXGI_FORMAT_B4G4R4A4_UNORM:
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_A8_UNORM:
    return static_cast<uint32_t>(width *
                                 ((format == DXGI_FORMAT_B5G6R5_UNORM ||
                                   format == DXGI_FORMAT_B5G5R5A1_UNORM ||
                                   format == DXGI_FORMAT_B4G4R4A4_UNORM)
                                      ? 2
                                      : 1));
  case DXGI_FORMAT_R8G8_TYPELESS:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
  case DXGI_FORMAT_R16_TYPELESS:
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_D16_UNORM:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
    return static_cast<uint32_t>(width * 2);
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return static_cast<uint32_t>(width * 16);
  case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
  case DXGI_FORMAT_R8G8_B8G8_UNORM:
  case DXGI_FORMAT_G8R8_G8B8_UNORM:
    return static_cast<uint32_t>(width * 4);
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R32G32_TYPELESS:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    return static_cast<uint32_t>(width * 8);
  default:
    return static_cast<uint32_t>(width * 4);
  }
}

static bool IsStencilPlaneFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
         format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
         format == DXGI_FORMAT_R24G8_TYPELESS ||
         format == DXGI_FORMAT_R32G8X24_TYPELESS ||
         format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
         format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
         format == DXGI_FORMAT_X24_TYPELESS_G8_UINT ||
         format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
}

static bool IsPlanarFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
  case DXGI_FORMAT_420_OPAQUE:
    return true;
  default:
    return false;
  }
}

static UINT ResourcePlaneCount(DXGI_FORMAT format) {
  return (IsStencilPlaneFormat(format) || IsPlanarFormat(format)) ? 2 : 1;
}

static DXGI_FORMAT ResourcePlaneFormat(DXGI_FORMAT format, UINT plane) {
  if (plane == 0) {
    if (IsStencilPlaneFormat(format))
      return DXGI_FORMAT_R32_TYPELESS;
    switch (format) {
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_420_OPAQUE:
      return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
      return DXGI_FORMAT_R16_UNORM;
    default:
      return format;
    }
  }
  if (IsStencilPlaneFormat(format))
    return DXGI_FORMAT_R8_TYPELESS;
  switch (format) {
  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_420_OPAQUE:
    return DXGI_FORMAT_R8G8_UNORM;
  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
    return DXGI_FORMAT_R16G16_UNORM;
  default:
    return format;
  }
}

static void AdjustResourcePlaneDimensions(DXGI_FORMAT format, UINT plane,
                                           uint64_t &width, uint64_t &height) {
  if (plane != 1)
    return;
  switch (format) {
  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
  case DXGI_FORMAT_420_OPAQUE:
    width = std::max<uint64_t>(1, (width + 1) / 2);
    height = std::max<uint64_t>(1, (height + 1) / 2);
    break;
  default:
    break;
  }
}

static uint64_t PlanarShadowSize(const D3D12_RESOURCE_DESC &desc) {
  if (!IsPlanarFormat(desc.Format))
    return 0;
  const UINT mip_levels = std::max<UINT>(desc.MipLevels, 1);
  const UINT array_size =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(desc.DepthOrArraySize, 1);
  uint64_t total = 0;
  for (UINT plane = 0; plane < 2; ++plane) {
    const DXGI_FORMAT plane_format = ResourcePlaneFormat(desc.Format, plane);
    for (UINT slice = 0; slice < array_size; ++slice) {
      for (UINT mip = 0; mip < mip_levels; ++mip) {
        uint64_t width = std::max<uint64_t>(1, desc.Width >> mip);
        uint64_t height = std::max<uint64_t>(1, desc.Height >> mip);
        AdjustResourcePlaneDimensions(desc.Format, plane, width, height);
        const uint64_t rows = PackedTextureRowCount(plane_format, height);
        const uint64_t row_bytes = PackedTextureRowBytes(plane_format, width);
        const uint64_t depth =
            desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                ? std::max<uint64_t>(1, desc.DepthOrArraySize >> mip)
                : 1;
        if (row_bytes && rows > UINT64_MAX / row_bytes)
          return 0;
        const uint64_t image_bytes = row_bytes * rows;
        if (depth && image_bytes > UINT64_MAX / depth)
          return 0;
        const uint64_t bytes = image_bytes * depth;
        if (bytes > UINT64_MAX - total)
          return 0;
        total += bytes;
      }
    }
  }
  return total <= SIZE_MAX ? total : 0;
}

static size_t PlanarShadowOffset(const D3D12_RESOURCE_DESC &desc, UINT plane,
                                 UINT mip, UINT slice) {
  if (!IsPlanarFormat(desc.Format) || plane >= 2)
    return SIZE_MAX;
  const UINT mip_levels = std::max<UINT>(desc.MipLevels, 1);
  const UINT array_size =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(desc.DepthOrArraySize, 1);
  if (mip >= mip_levels || slice >= array_size)
    return SIZE_MAX;
  uint64_t offset = 0;
  for (UINT prior_plane = 0; prior_plane < plane; ++prior_plane) {
    const DXGI_FORMAT plane_format =
        ResourcePlaneFormat(desc.Format, prior_plane);
    for (UINT prior_slice = 0; prior_slice < array_size; ++prior_slice) {
      for (UINT prior_mip = 0; prior_mip < mip_levels; ++prior_mip) {
        uint64_t width = std::max<uint64_t>(1, desc.Width >> prior_mip);
        uint64_t height = std::max<uint64_t>(1, desc.Height >> prior_mip);
        AdjustResourcePlaneDimensions(desc.Format, prior_plane, width, height);
        const uint64_t image_bytes =
            uint64_t(PackedTextureRowBytes(plane_format, width)) *
            PackedTextureRowCount(plane_format, height);
        const uint64_t depth =
            desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                ? std::max<uint64_t>(1, desc.DepthOrArraySize >> prior_mip)
                : 1;
        if (depth && image_bytes > UINT64_MAX / depth)
          return SIZE_MAX;
        if (image_bytes * depth > UINT64_MAX - offset)
          return SIZE_MAX;
        offset += image_bytes * depth;
      }
    }
  }
  const DXGI_FORMAT plane_format = ResourcePlaneFormat(desc.Format, plane);
  for (UINT prior_slice = 0; prior_slice < slice; ++prior_slice) {
    for (UINT prior_mip = 0; prior_mip < mip_levels; ++prior_mip) {
      uint64_t width = std::max<uint64_t>(1, desc.Width >> prior_mip);
      uint64_t height = std::max<uint64_t>(1, desc.Height >> prior_mip);
      AdjustResourcePlaneDimensions(desc.Format, plane, width, height);
      const uint64_t image_bytes =
          uint64_t(PackedTextureRowBytes(plane_format, width)) *
          PackedTextureRowCount(plane_format, height);
      const uint64_t depth =
          desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? std::max<uint64_t>(1, desc.DepthOrArraySize >> prior_mip)
              : 1;
      if (depth && image_bytes > UINT64_MAX / depth)
        return SIZE_MAX;
      if (image_bytes * depth > UINT64_MAX - offset)
        return SIZE_MAX;
      offset += image_bytes * depth;
    }
  }
  for (UINT prior_mip = 0; prior_mip < mip; ++prior_mip) {
    uint64_t width = std::max<uint64_t>(1, desc.Width >> prior_mip);
    uint64_t height = std::max<uint64_t>(1, desc.Height >> prior_mip);
    AdjustResourcePlaneDimensions(desc.Format, plane, width, height);
    const uint64_t image_bytes =
        uint64_t(PackedTextureRowBytes(plane_format, width)) *
        PackedTextureRowCount(plane_format, height);
    const uint64_t depth =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? std::max<uint64_t>(1, desc.DepthOrArraySize >> prior_mip)
            : 1;
    if (depth && image_bytes > UINT64_MAX / depth)
      return SIZE_MAX;
    if (image_bytes * depth > UINT64_MAX - offset)
      return SIZE_MAX;
    offset += image_bytes * depth;
  }
  return offset <= SIZE_MAX ? static_cast<size_t>(offset) : SIZE_MAX;
}

static bool PlanarShadowLayout(const D3D12_RESOURCE_DESC &desc, UINT plane,
                               UINT mip, UINT slice, const WMTOrigin &origin,
                               const WMTSize &size, size_t &base_offset,
                               uint64_t &full_row_bytes,
                               uint64_t &full_slice_bytes) {
  if (!IsPlanarFormat(desc.Format) || plane >= 2)
    return false;
  uint64_t full_width = std::max<uint64_t>(1, desc.Width >> mip);
  uint64_t full_height = std::max<uint64_t>(1, desc.Height >> mip);
  AdjustResourcePlaneDimensions(desc.Format, plane, full_width, full_height);
  const DXGI_FORMAT plane_format = ResourcePlaneFormat(desc.Format, plane);
  full_row_bytes = PackedTextureRowBytes(plane_format, full_width);
  const uint64_t full_rows = PackedTextureRowCount(plane_format, full_height);
  if (!full_row_bytes || full_rows > UINT64_MAX / full_row_bytes)
    return false;
  full_slice_bytes = full_row_bytes * full_rows;
  base_offset = PlanarShadowOffset(desc, plane, mip, slice);
  if (base_offset == SIZE_MAX ||
      uint64_t(origin.x) + size.width > full_width ||
      uint64_t(origin.y) + size.height > full_height ||
      uint64_t(origin.z) + size.depth >
          (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? std::max<uint64_t>(1, desc.DepthOrArraySize >> mip)
               : 1))
    return false;
  const uint64_t x_offset = PackedTextureRowBytes(plane_format, origin.x);
  const uint64_t row_bytes = PackedTextureRowBytes(plane_format, size.width);
  const uint64_t rows = PackedTextureRowCount(plane_format, size.height);
  if (x_offset > full_row_bytes || row_bytes > full_row_bytes - x_offset ||
      rows > UINT64_MAX / full_row_bytes ||
      uint64_t(origin.y) + rows > full_rows ||
      size.depth && full_slice_bytes > UINT64_MAX / size.depth)
    return false;
  return true;
}

static uint64_t StencilShadowSize(const D3D12_RESOURCE_DESC &desc) {
  if (!IsStencilPlaneFormat(desc.Format))
    return 0;
  const UINT mip_levels = std::max<UINT>(desc.MipLevels, 1);
  const UINT array_size =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(desc.DepthOrArraySize, 1);
  uint64_t total = 0;
  for (UINT slice = 0; slice < array_size; ++slice) {
    (void)slice;
    for (UINT mip = 0; mip < mip_levels; ++mip) {
      const uint64_t width = std::max<uint64_t>(1, desc.Width >> mip);
      const uint64_t height = std::max<uint64_t>(1, desc.Height >> mip);
      if (width > (UINT64_MAX - total) / height)
        return 0;
      total += width * height;
    }
  }
  return total <= SIZE_MAX ? total : 0;
}

static size_t StencilShadowOffset(const D3D12_RESOURCE_DESC &desc, UINT mip,
                                  UINT slice) {
  const UINT mip_levels = std::max<UINT>(desc.MipLevels, 1);
  const UINT array_size =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(desc.DepthOrArraySize, 1);
  if (slice >= array_size || mip >= mip_levels)
    return SIZE_MAX;
  uint64_t offset = 0;
  for (UINT s = 0; s < slice; ++s) {
    for (UINT m = 0; m < mip_levels; ++m)
      offset += std::max<uint64_t>(1, desc.Width >> m) *
                std::max<uint64_t>(1, desc.Height >> m);
  }
  for (UINT m = 0; m < mip; ++m)
    offset += std::max<uint64_t>(1, desc.Width >> m) *
              std::max<uint64_t>(1, desc.Height >> m);
  return offset <= SIZE_MAX ? static_cast<size_t>(offset) : SIZE_MAX;
}

static bool ResolveSubresourceRegion(const D3D12_RESOURCE_DESC &desc,
                                     UINT subresource,
                                     const D3D12_BOX *requested,
                                     UINT &mip, UINT &slice, UINT &plane,
                                     DXGI_FORMAT &plane_format,
                                     WMTOrigin &origin, WMTSize &size) {
  const UINT mip_levels = std::max<UINT>(desc.MipLevels, 1);
  const UINT array_size =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(desc.DepthOrArraySize, 1);
  const uint64_t base_subresources = uint64_t(mip_levels) * array_size;
  if (subresource >= base_subresources * ResourcePlaneCount(desc.Format))
    return false;
  plane = static_cast<UINT>(subresource / base_subresources);
  const UINT base_subresource =
      static_cast<UINT>(subresource % base_subresources);
  mip = base_subresource % mip_levels;
  slice = base_subresource / mip_levels;
  plane_format = ResourcePlaneFormat(desc.Format, plane);
  uint64_t width = std::max<uint64_t>(1, desc.Width >> mip);
  uint64_t height = std::max<uint64_t>(1, desc.Height >> mip);
  AdjustResourcePlaneDimensions(desc.Format, plane, width, height);
  const uint64_t depth =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? std::max<uint64_t>(1, desc.DepthOrArraySize >> mip)
          : 1;
  D3D12_BOX box = requested ? *requested
                            : D3D12_BOX{0, 0, 0, static_cast<UINT>(width),
                                        static_cast<UINT>(height),
                                        static_cast<UINT>(depth)};
  if (box.left >= box.right || box.top >= box.bottom || box.front >= box.back ||
      box.right > width || box.bottom > height || box.back > depth)
    return false;
  origin = {box.left, box.top, box.front};
  size = {box.right - box.left, box.bottom - box.top, box.back - box.front};
  return true;
}

} // namespace

static WMTTextureType
TextureTypeForResourceDesc(const D3D12_RESOURCE_DESC &desc) {
  UINT sample_count = desc.SampleDesc.Count ? desc.SampleDesc.Count : 1;
  switch (desc.Dimension) {
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    return (desc.DepthOrArraySize > 1) ? WMTTextureType1DArray : WMTTextureType1D;
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
    return WMTTextureType3D;
  default:
    if (sample_count > 1)
      return (desc.DepthOrArraySize > 1) ? WMTTextureType2DMultisampleArray
                                         : WMTTextureType2DMultisample;
    return (desc.DepthOrArraySize > 1) ? WMTTextureType2DArray : WMTTextureType2D;
  }
}

static uint32_t
SampleCountForResourceDesc(const D3D12_RESOURCE_DESC &desc,
                           WMTTextureType texture_type) {
  UINT sample_count = desc.SampleDesc.Count ? desc.SampleDesc.Count : 1;
  if (texture_type == WMTTextureType2DMultisample ||
      texture_type == WMTTextureType2DMultisampleArray)
    return sample_count;
  return 1;
}

static D3D12_TILE_SHAPE TileShapeForFormat(DXGI_FORMAT format,
                                            bool volume = false,
                                            bool one_d = false) {
  if (one_d) {
    switch (format) {
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
      return {65536, 1, 1};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
      return {32768, 1, 1};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return {16384, 1, 1};
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
      return {8192, 1, 1};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      return {4096, 1, 1};
    default:
      break;
    }
  }
  // Volume tiled resources use a 4x4x4 tile grouping.  Their standard tile
  // shape is not the 2D shape divided by an arbitrary slice count; it is
  // selected from the format's bits per texel/block as specified by D3D12.
  if (volume) {
    switch (format) {
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
      return {64, 32, 32};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
      return {32, 32, 32};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return {32, 32, 16};
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
      return {32, 16, 16};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      return {16, 16, 16};
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
      return {128, 64, 16};
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      return {64, 64, 16};
    default:
      break;
    }
  }

  switch (format) {
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_A8_UNORM:
    return {256, 256, 1};
  case DXGI_FORMAT_R8G8_TYPELESS:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
  case DXGI_FORMAT_R16_TYPELESS:
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
    return {256, 128, 1};
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
    return {128, 128, 1};
  case DXGI_FORMAT_R16G16_TYPELESS:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R32G32_TYPELESS:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
    return {128, 64, 1};
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
    return {128, 64, 1};
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return {64, 64, 1};
  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS:
  case DXGI_FORMAT_BC4_UNORM:
  case DXGI_FORMAT_BC4_SNORM:
    return {512, 256, 1};
  case DXGI_FORMAT_BC2_TYPELESS:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC5_TYPELESS:
  case DXGI_FORMAT_BC5_UNORM:
  case DXGI_FORMAT_BC5_SNORM:
  case DXGI_FORMAT_BC6H_TYPELESS:
  case DXGI_FORMAT_BC6H_UF16:
  case DXGI_FORMAT_BC6H_SF16:
  case DXGI_FORMAT_BC7_TYPELESS:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return {256, 256, 1};
  default:
    return {128, 128, 1};
  }
}

static uint64_t SparseHeapSizeForResource(const D3D12_RESOURCE_DESC &desc) {
  const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const D3D12_TILE_SHAPE shape =
      TileShapeForFormat(desc.Format, volume,
                         desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D);
  const uint64_t slices =
      volume ? 1 : std::max<uint32_t>(1, desc.DepthOrArraySize);
  const uint64_t mips = std::max<uint32_t>(1, desc.MipLevels);
  uint64_t tile_count = 0;
  for (uint64_t slice = 0; slice < slices; slice++) {
    for (uint64_t mip = 0; mip < mips; mip++) {
      const uint64_t width = std::max<uint64_t>(1, desc.Width >> mip);
      const uint64_t height = std::max<uint64_t>(1, desc.Height >> mip);
      const uint64_t depth =
          volume ? std::max<uint64_t>(1, desc.DepthOrArraySize >> mip) : 1;
      const uint64_t tiles_x =
          (width + shape.WidthInTexels - 1) / shape.WidthInTexels;
      const uint64_t tiles_y =
          (height + shape.HeightInTexels - 1) / shape.HeightInTexels;
      const uint64_t tiles_z =
          (depth + shape.DepthInTexels - 1) / shape.DepthInTexels;
      tile_count += tiles_x * tiles_y * tiles_z;
    }
  }
  const uint64_t size = tile_count * UINT64_C(65536);
  // Apple's default sparse page is 16 KiB; one D3D12 standard tile is 64 KiB.
  return std::max<uint64_t>(UINT64_C(65536),
                            (size + UINT64_C(16383)) & ~UINT64_C(16383));
}

MTLD3D12Resource::MTLD3D12Resource(
    MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
    D3D12_RESOURCE_STATES initial_state,
    D3D12_HEAP_PROPERTIES heap_properties, D3D12_HEAP_FLAGS heap_flags,
    bool reserved)
    : m_device(device), m_desc(desc), m_state_tracker(initial_state),
      m_residency((heap_flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT) == 0),
      m_heap_properties(heap_properties), m_heap_flags(heap_flags),
      m_is_reserved(reserved) {
  InitializeResource(WMT::Reference<WMT::Buffer>{}, nullptr, 0, 0);
}

MTLD3D12Resource::MTLD3D12Resource(
    MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
    D3D12_RESOURCE_STATES initial_state,
    D3D12_HEAP_PROPERTIES heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    WMT::Reference<WMT::Buffer> backing_buffer, void *backing_cpu_addr,
    uint64_t backing_gpu_addr, uint64_t backing_offset)
    : m_device(device), m_desc(desc), m_state_tracker(initial_state),
      m_residency((heap_flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT) == 0),
      m_heap_properties(heap_properties), m_heap_flags(heap_flags),
      m_backing_offset(backing_offset) {
  InitializeResource(std::move(backing_buffer), backing_cpu_addr,
                     backing_gpu_addr, backing_offset);
}

MTLD3D12Resource::MTLD3D12Resource(
    MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
    D3D12_RESOURCE_STATES initial_state,
    D3D12_HEAP_PROPERTIES heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    WMT::Reference<WMT::Texture> backing_texture,
    uint64_t backing_texture_gpu_id, uint64_t backing_offset)
    : m_device(device), m_desc(desc), m_state_tracker(initial_state),
      m_residency((heap_flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT) == 0),
      m_heap_properties(heap_properties), m_heap_flags(heap_flags),
      m_tex_gpu_resource_id(backing_texture_gpu_id),
      m_backing_offset(backing_offset) {
  m_mtl_texture = std::move(backing_texture);
  InitializeResource(WMT::Reference<WMT::Buffer>{}, nullptr, 0, 0);
}

void MTLD3D12Resource::InitializeResource(
    WMT::Reference<WMT::Buffer> backing_buffer, void *backing_cpu_addr,
    uint64_t backing_gpu_addr, uint64_t backing_offset) {
  m_dxgi_resource =
      std::make_unique<MTLDXGIResource<MTLD3D12Resource>>(this);
  const uint64_t stencil_shadow_size = StencilShadowSize(m_desc);
  if (stencil_shadow_size)
    m_stencil_shadow.resize(static_cast<size_t>(stencil_shadow_size));
  m_device->AddRef();

  auto wmt_device = m_device->GetDXMTDevice().device();
  RTRACE("ctor: wmt_device=%llu dim=%u fmt=%u w=%llu h=%u depth_or_arr=%u",
    (unsigned long long)wmt_device.handle, m_desc.Dimension, m_desc.Format,
    m_desc.Width, m_desc.Height, m_desc.DepthOrArraySize);

  // Metal has no native multi-plane texture object. Keep planar D3D12
  // resources in a tightly packed CPU shadow with the same plane/mip/slice
  // ordering used by ResolveSubresourceRegion. This provider is explicit:
  // direct subresource I/O remains observable and never pretends that an
  // unrelated single-plane Metal texture represents NV12/P010/P016 data.
  if (!IsBuffer() && IsPlanarFormat(m_desc.Format)) {
    const uint64_t shadow_size = PlanarShadowSize(m_desc);
    if (shadow_size)
      m_planar_shadow.resize(static_cast<size_t>(shadow_size));
    m_gpu_addr = 0;
    RTRACE("ctor: planar shadow format=%u bytes=%llu valid=%d",
           (unsigned)m_desc.Format, (unsigned long long)shadow_size,
           m_planar_shadow.empty() ? 0 : 1);
    m_device->RegisterResource(this);
    return;
  }

  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    bool cpu_accessible =
        m_is_reserved || IsCPUAccessibleHeap(m_heap_properties);
    WMTBufferInfo buf_info = {};
    buf_info.length = m_desc.Width ? m_desc.Width : 256;
    if (m_is_reserved && !backing_buffer.handle) {
      WMTSparseBufferInfo sparse_info = {};
      sparse_info.length = m_desc.Width ? m_desc.Width : 256;
      sparse_info.options = WMTResourceStorageModePrivate |
                            WMTResourceHazardTrackingModeTracked;
      sparse_info.sparse_page_size = WMTSparsePageSize64;
      m_mtl_buffer = wmt_device.newSparseBuffer(sparse_info);
      if (m_mtl_buffer.handle) {
        m_native_sparse_buffer = true;
        m_gpu_addr = sparse_info.gpu_address;
        buf_info.length = sparse_info.length;
        buf_info.options = sparse_info.options;
        buf_info.gpu_address = m_gpu_addr;
        m_buf_info = buf_info;
        RTRACE("ctor: native sparse buffer gpu=0x%llx len=%llu page=%u",
               (unsigned long long)m_gpu_addr,
               (unsigned long long)sparse_info.length,
               sparse_info.sparse_page_size);
      }
    }
    if (!m_native_sparse_buffer && backing_buffer.handle) {
      m_mtl_buffer = std::move(backing_buffer);
      m_cpu_addr = backing_cpu_addr
                       ? static_cast<void *>(static_cast<char *>(backing_cpu_addr) +
                                             backing_offset)
                       : nullptr;
      m_gpu_addr = backing_gpu_addr + backing_offset;
      buf_info.gpu_address = m_gpu_addr;
      buf_info.length = m_desc.Width ? m_desc.Width : 256;
      m_buf_info = buf_info;
      RTRACE("ctor: placed buffer cpu=%p gpu=0x%llx len=%llu heap_off=%llu "
             "heap_type=%u",
             m_cpu_addr, (unsigned long long)m_gpu_addr,
             (unsigned long long)m_desc.Width,
             (unsigned long long)backing_offset,
             (unsigned)m_heap_properties.Type);
      if (m_desc.Width >= (64ull << 20)) {
        Logger::info(str::format("M12 large resource uses heap backing width=",
                                 m_desc.Width, " gpu=0x",
                                 (unsigned long long)m_gpu_addr, " off=",
                                 (unsigned long long)backing_offset));
      }
    } else if (!m_native_sparse_buffer) {
      buf_info.options =
          cpu_accessible ? WMTResourceStorageModeShared
                         : WMTResourceStorageModePrivate;
      m_mtl_buffer = wmt_device.newBuffer(buf_info);
      m_cpu_addr = buf_info.memory.get_accessible_or_null();
      m_gpu_addr = buf_info.gpu_address;
      m_buf_info = buf_info;
      if (m_is_reserved && m_cpu_addr)
        std::memset(m_cpu_addr, 0, static_cast<size_t>(m_desc.Width));
      RTRACE("ctor: buffer cpu=%p gpu=0x%llx len=%llu opts=%u heap_type=%u",
             m_cpu_addr, (unsigned long long)m_gpu_addr,
             (unsigned long long)m_desc.Width, (unsigned)buf_info.options,
             (unsigned)m_heap_properties.Type);
      if (m_desc.Width >= (64ull << 20)) {
        Logger::info(str::format("M12 large resource standalone buffer width=",
                                 m_desc.Width, " gpu=0x",
                                 (unsigned long long)m_gpu_addr, " heap_type=",
                                 (unsigned)m_heap_properties.Type));
      }
    }
  } else {
    bool cpu_accessible = IsCPUAccessibleHeap(m_heap_properties);
    m_is_shading_rate_image =
        m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        m_desc.Format == DXGI_FORMAT_R8_UINT &&
        std::max<UINT>(m_desc.SampleDesc.Count, 1) == 1 &&
        std::max<UINT16>(m_desc.MipLevels, 1) == 1 &&
        std::max<UINT16>(m_desc.DepthOrArraySize, 1) == 1 &&
        m_desc.Width <= UINT32_MAX && m_desc.Height <= UINT32_MAX;
    if (m_is_shading_rate_image) {
      const uint64_t byte_count =
          std::max<uint64_t>(m_desc.Width, 1) *
          std::max<uint64_t>(m_desc.Height, 1);
      if (byte_count <= UINT32_MAX)
        m_shading_rate_image_data.resize(static_cast<size_t>(byte_count));
      else
        m_is_shading_rate_image = false;
    }
    WMTTextureInfo tex_info = {};
    tex_info.width = m_desc.Width;
    tex_info.height = m_desc.Height;
    tex_info.depth = (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                         ? m_desc.DepthOrArraySize
                         : 1;
    // Both 1D and 2D D3D12 arrays use Metal's arrayLength; only 3D uses
    // depth for its additional extent.
    tex_info.array_length =
        (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
         m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            ? m_desc.DepthOrArraySize
            : 1;
    tex_info.mipmap_level_count = m_desc.MipLevels ? m_desc.MipLevels : 1;
    m_writable_msaa_emulated =
        m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        m_desc.SampleDesc.Count > 1 &&
        (m_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
        !(m_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
    if (m_writable_msaa_emulated) {
      // Metal exposes multisampled textures as read-only shader resources.
      // Represent a writable D3D12 MSAA UAV as ordinary array slices, with
      // each logical array slice expanded by the sample count. The DXIL
      // lowering selects the flattened sample slice explicitly.
      tex_info.type = WMTTextureType2DArray;
      tex_info.array_length =
          std::max<UINT16>(m_desc.DepthOrArraySize, 1) *
          std::max<UINT>(m_desc.SampleDesc.Count, 1);
      tex_info.sample_count = 1;
      tex_info.mipmap_level_count = 1;
    } else {
      tex_info.type = TextureTypeForResourceDesc(m_desc);
      tex_info.sample_count = SampleCountForResourceDesc(m_desc, tex_info.type);
      if (tex_info.sample_count > 1)
        tex_info.mipmap_level_count = 1;
    }
    tex_info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget |
                                      WMTTextureUsageShaderRead |
                                      WMTTextureUsageShaderWrite |
                                      WMTTextureUsagePixelFormatView);
    tex_info.options = cpu_accessible ? WMTResourceStorageModeShared : WMTResourceStorageModePrivate;
    tex_info.pixel_format = MTLD3D12PipelineState::DXGIToMTLPixelFormat(static_cast<DXGI_FORMAT>(m_desc.Format));
    if ((m_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
        m_desc.Format == DXGI_FORMAT_R32_TYPELESS)
      tex_info.pixel_format = WMTPixelFormatDepth32Float;
    if (tex_info.pixel_format == WMTPixelFormatInvalid) {
      RTRACE("ctor: unsupported texture format=%u; refusing fallback",
             (unsigned)m_desc.Format);
      return;
    }

    RTRACE("ctor: writable_msaa=%d flags=0x%x samples=%u about to newTexture "
           "type=%u fmt=%u %ux%u depth=%u arr=%u mip=%u sample=%u opts=%u",
      m_writable_msaa_emulated ? 1 : 0, (unsigned)m_desc.Flags,
      (unsigned)m_desc.SampleDesc.Count, tex_info.type, tex_info.pixel_format,
      (unsigned)tex_info.width, (unsigned)tex_info.height,
      (unsigned)tex_info.depth, (unsigned)tex_info.array_length,
      (unsigned)tex_info.mipmap_level_count, (unsigned)tex_info.sample_count,
      (unsigned)tex_info.options);
    const bool placement_sparse_candidate =
        m_is_reserved && std::max<UINT16>(m_desc.MipLevels, 1) == 1 &&
        ((m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
          m_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) ||
         (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
          m_desc.Format == DXGI_FORMAT_R32_FLOAT) ||
         (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
          m_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM));
    if (placement_sparse_candidate) {
      // Metal 4 placement-sparse textures are created without a backing heap;
      // UpdateTileMappings supplies the D3D12 placement heap later. This keeps
      // the proven single-mip RGBA8 shape eligible for physical cross-resource
      // page aliases. Use the MTL4 queue as the availability probe so older
      // Metal falls back to the legacy resource-state sparse heap path below.
      auto mtl4_probe = wmt_device.newMTL4CommandQueue();
      if (mtl4_probe.handle) {
        tex_info.placement_sparse_page_size = WMTSparsePageSize16;
        m_mtl_texture = wmt_device.newTexture(tex_info);
        m_native_placement_sparse_texture = m_mtl_texture.handle != 0;
      }
    } else if (!m_is_reserved && !m_mtl_texture.handle) {
      if (m_heap_flags & D3D12_HEAP_FLAG_SHARED) {
        m_mtl_texture = wmt_device.newSharedTexture(tex_info);
        m_shared_texture_mach_port = tex_info.mach_port;
      } else {
        m_mtl_texture = wmt_device.newTexture(tex_info);
      }
    }
    if (!m_mtl_texture.handle && m_is_reserved) {
      // Older Metal falls back to the existing private sparse heap path. Clear
      // the placement-only descriptor field before creating that texture.
      tex_info.placement_sparse_page_size = 0;
      WMTHeapInfo heap_info = {};
      heap_info.size = SparseHeapSizeForResource(m_desc);
      heap_info.options = WMTResourceStorageModePrivate |
                          WMTResourceHazardTrackingModeTracked;
      heap_info.type = WMTHeapTypeSparse;
      // Metal's 16 KiB sparse page is the largest page granularity available
      // on the proof host and four pages make one D3D12 64 KiB tile.
      heap_info.sparse_page_size = WMTSparsePageSize16;
      m_sparse_heap = wmt_device.newHeap(heap_info);
      if (m_sparse_heap.handle)
        m_mtl_texture = m_sparse_heap.newTexture(tex_info);
    }
    if (tex_info.gpu_resource_id)
      m_tex_gpu_resource_id = tex_info.gpu_resource_id;
    if (!m_mtl_texture.handle) {
      RTRACE("ctor: texture creation FAILED type=%u fmt=%u %ux%u arr=%u",
        tex_info.type, tex_info.pixel_format, (unsigned)tex_info.width, (unsigned)tex_info.height, (unsigned)tex_info.array_length);
    } else {
      RTRACE("ctor: texture created fmt=%u %ux%u arr=%u handle=%llu %s",
        tex_info.pixel_format, (unsigned)tex_info.width, (unsigned)tex_info.height, (unsigned)tex_info.array_length,
        (unsigned long long)m_mtl_texture.handle, cpu_accessible ? "cpu" : "gpu");
    }
    // D3D12 texture resources do not expose GPU virtual addresses. The
    // provider uses the Metal texture/resource id for descriptor identity;
    // never manufacture a buffer address that callers could dereference.
    m_gpu_addr = 0;
    RTRACE("ctor: texture gpu_addr=0 (texture resources have no VA)");
  }

  Logger::info(str::format("D3D12Resource: dim=", m_desc.Dimension,
                            " ", m_desc.Width, "x", m_desc.Height,
                            " gpu=", m_gpu_addr));
  m_device->RegisterResource(this);
}

WMT::Reference<WMT::Texture> MTLD3D12Resource::GetMTLTexture() {
  if (m_is_reserved)
    return m_mtl_texture;
  if (!m_mtl_texture.handle && m_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    bool cpu_accessible = IsCPUAccessibleHeap(m_heap_properties);
    auto wmt_device = m_device->GetDXMTDevice().device();
    WMTTextureInfo tex_info = {};
    tex_info.width = m_desc.Width ? m_desc.Width : 1;
    tex_info.height = m_desc.Height ? m_desc.Height : 1;
    tex_info.depth = (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                          ? m_desc.DepthOrArraySize : 1;
    // Keep lazy texture creation consistent with the constructor path for
    // 1D/2D arrays.
    tex_info.array_length =
        (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
         m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            ? m_desc.DepthOrArraySize
            : 1;
    tex_info.mipmap_level_count = m_desc.MipLevels ? m_desc.MipLevels : 1;
    if (m_writable_msaa_emulated) {
      tex_info.type = WMTTextureType2DArray;
      tex_info.array_length =
          std::max<UINT16>(m_desc.DepthOrArraySize, 1) *
          std::max<UINT>(m_desc.SampleDesc.Count, 1);
      tex_info.sample_count = 1;
      tex_info.mipmap_level_count = 1;
    } else {
      tex_info.type = TextureTypeForResourceDesc(m_desc);
      tex_info.sample_count = SampleCountForResourceDesc(m_desc, tex_info.type);
      if (tex_info.sample_count > 1)
        tex_info.mipmap_level_count = 1;
    }
    tex_info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite | WMTTextureUsagePixelFormatView);
    tex_info.options = cpu_accessible ? WMTResourceStorageModeShared : WMTResourceStorageModePrivate;
    tex_info.pixel_format = MTLD3D12PipelineState::DXGIToMTLPixelFormat(static_cast<DXGI_FORMAT>(m_desc.Format));
    if ((m_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
        m_desc.Format == DXGI_FORMAT_R32_TYPELESS)
      tex_info.pixel_format = WMTPixelFormatDepth32Float;
    if (tex_info.pixel_format == WMTPixelFormatInvalid) {
      RTRACE("GetMTLTexture: unsupported texture format=%u; refusing fallback",
             (unsigned)m_desc.Format);
      return m_mtl_texture;
    }
    RTRACE("GetMTLTexture: creating type=%u fmt=%u %ux%ux%u arr=%u mip=%u sample=%u opts=%u",
      tex_info.type, tex_info.pixel_format, (unsigned)tex_info.width, (unsigned)tex_info.height,
      (unsigned)tex_info.depth, (unsigned)tex_info.array_length, (unsigned)tex_info.mipmap_level_count,
      (unsigned)tex_info.sample_count, (unsigned)tex_info.options);
    m_mtl_texture = wmt_device.newTexture(tex_info);
    if (tex_info.gpu_resource_id)
      m_tex_gpu_resource_id = tex_info.gpu_resource_id;
    if (!m_mtl_texture.handle) {
      RTRACE("GetMTLTexture: newTexture returned NULL handle");
      return m_mtl_texture;
    }
    RTRACE("GetMTLTexture: handle=%llu", (unsigned long long)m_mtl_texture.handle);
  }
  return m_mtl_texture;
}

uint32_t MTLD3D12Resource::GetTextureArrayLength() const {
  if (m_writable_msaa_emulated)
    return std::max<UINT16>(m_desc.DepthOrArraySize, 1) *
           std::max<UINT>(m_desc.SampleDesc.Count, 1);
  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
      m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return m_desc.DepthOrArraySize ? m_desc.DepthOrArraySize : 1;
  return 1;
}

void MTLD3D12Resource::UpdateShadingRateImage(
    const void *data, uint32_t row_pitch, uint32_t dst_x, uint32_t dst_y,
    uint32_t width, uint32_t height) {
  if (!m_is_shading_rate_image || !data || !row_pitch ||
      dst_x >= m_desc.Width || dst_y >= std::max<UINT>(m_desc.Height, 1) ||
      width > m_desc.Width - dst_x ||
      height > std::max<UINT>(m_desc.Height, 1) - dst_y ||
      m_shading_rate_image_data.empty())
    return;
  const uint8_t *source = static_cast<const uint8_t *>(data);
  const uint32_t image_width = static_cast<uint32_t>(m_desc.Width);
  for (uint32_t row = 0; row < height; ++row) {
    std::memcpy(m_shading_rate_image_data.data() +
                    uint64_t(dst_y + row) * image_width + dst_x,
                source + uint64_t(row) * row_pitch, width);
  }
  m_shading_rate_image_initialized = true;
}

uint64_t MTLD3D12Resource::GetBufferByteLength() const {
  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return m_desc.Width;
  return m_buf_info.length;
}

D3D12_TILE_SHAPE MTLD3D12Resource::GetTiledResourceTileShape() const {
  if (IsBuffer())
    return {D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, 1, 1};
  return TileShapeForFormat(
      m_desc.Format, m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D,
      m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D);
}

bool MTLD3D12Resource::ConfigureSamplerFeedback(
    const D3D12_MIP_REGION &region) {
  if (m_desc.Format != DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE &&
      m_desc.Format != DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE)
    return false;

  const uint32_t region_width = std::max<UINT>(region.Width, 1);
  const uint32_t region_height = std::max<UINT>(region.Height, 1);
  m_sampler_feedback_data_offset = 512;
  const uint64_t array_length = std::max<uint16_t>(m_desc.DepthOrArraySize, 1);
  const bool min_mip =
      m_desc.Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
  const uint32_t level_count =
      min_mip ? 1u : std::min<uint32_t>(std::max<uint16_t>(m_desc.MipLevels, 1),
                                        16u);
  m_sampler_feedback_levels.clear();
  m_sampler_feedback_levels.reserve(level_count);
  uint64_t next_offset = m_sampler_feedback_data_offset;
  for (uint32_t mip = 0; mip < level_count; ++mip) {
    const uint64_t logical_width = std::max<uint64_t>(m_desc.Width >> mip, 1);
    const uint32_t logical_height =
        std::max<uint32_t>(std::max<UINT>(m_desc.Height, 1) >> mip, 1);
    D3D12SamplerFeedbackLevelLayout level = {};
    level.width = static_cast<uint32_t>(
        (logical_width + region_width - 1) / region_width);
    level.height = (logical_height + region_height - 1) / region_height;
    level.row_pitch = (std::max<uint32_t>(level.width, 1) + 255u) & ~255u;
    level.offset = next_offset;
    next_offset += uint64_t(level.row_pitch) * level.height * array_length;
    m_sampler_feedback_levels.push_back(level);
  }
  m_sampler_feedback_width = m_sampler_feedback_levels[0].width;
  m_sampler_feedback_height = m_sampler_feedback_levels[0].height;
  m_sampler_feedback_row_pitch = m_sampler_feedback_levels[0].row_pitch;

  WMTBufferInfo info = {};
  info.length = next_offset;
  info.options = WMTResourceStorageModeShared;
  auto buffer = m_device->GetDXMTDevice().device().newBuffer(info);
  if (!buffer.handle) {
    RTRACE("ConfigureSamplerFeedback: buffer creation failed width=%u height=%u row=%u",
           m_sampler_feedback_width, m_sampler_feedback_height,
           m_sampler_feedback_row_pitch);
    return false;
  }

  uint32_t header[128] = {};
  header[0] = 0x4d534642u;
  header[1] = m_sampler_feedback_width;
  header[2] = m_sampler_feedback_height;
  header[3] = m_sampler_feedback_row_pitch;
  header[4] = min_mip ? 0u : 1u;
  header[5] = static_cast<uint32_t>(array_length);
  header[6] = static_cast<uint32_t>(m_sampler_feedback_data_offset);
  header[8] = 0u; // software lock, deliberately 32-bit aligned
  header[9] = level_count;
  for (uint32_t mip = 0; mip < level_count; ++mip) {
    const auto &level = m_sampler_feedback_levels[mip];
    header[10 + mip * 4 + 0] = static_cast<uint32_t>(level.offset);
    header[10 + mip * 4 + 1] = level.width;
    header[10 + mip * 4 + 2] = level.height;
    header[10 + mip * 4 + 3] = level.row_pitch;
  }
  buffer.updateContents(0, header, sizeof(header));
  m_mtl_buffer = std::move(buffer);
  m_buf_info = info;
  m_is_sampler_feedback = true;
  RTRACE("ConfigureSamplerFeedback: fmt=%u logical=%llux%u region=%ux%u physical=%ux%u row=%u bytes=%llu",
         (unsigned)m_desc.Format, (unsigned long long)m_desc.Width,
         (unsigned)m_desc.Height, region_width, region_height,
         m_sampler_feedback_width, m_sampler_feedback_height,
         m_sampler_feedback_row_pitch, (unsigned long long)info.length);
  return true;
}

MTLD3D12Resource::~MTLD3D12Resource() {
  m_device->UnregisterResource(this);
  if (m_shared_mapping_view)
    UnmapViewOfFile(m_shared_mapping_view);
  if (m_shared_mapping)
    CloseHandle(m_shared_mapping);
  m_shared_mapping_view = nullptr;
  m_shared_mapping = nullptr;
  if (m_parent_heap) {
    m_parent_heap->Release();
    m_parent_heap = nullptr;
  }
  m_mtl_buffer = nullptr;
  m_mtl_texture = nullptr;
  m_device->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
      riid == IID_ID3D12Resource || riid == IID_ID3D12Resource1 ||
      riid == IID_ID3D12Resource2) {
    *ppvObject = ref(this);
    return S_OK;
  }
  if (riid == __uuidof(IDXGIObject) ||
      riid == __uuidof(IDXGIDeviceSubObject) ||
      riid == __uuidof(IDXGIResource) ||
      riid == __uuidof(IDXGIResource1)) {
    *ppvObject = ref(m_dxgi_resource.get());
    return S_OK;
  }
  RTRACE("QI unknown IID %s -> E_NOINTERFACE", str::format(riid).c_str());
  return E_NOINTERFACE;
}

HRESULT MTLD3D12Resource::GetSharedHandle(HANDLE *handle) {
  if (!handle)
    return E_POINTER;
  *handle = nullptr;
  if (!m_device)
    return DXGI_ERROR_INVALID_CALL;
  return m_device->CreateSharedHandle(
      static_cast<ID3D12DeviceChild *>(this), nullptr, GENERIC_ALL, nullptr,
      handle);
}

HRESULT MTLD3D12Resource::CreateSharedHandle(
    const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name,
    HANDLE *handle) {
  if (!m_device)
    return DXGI_ERROR_INVALID_CALL;
  return m_device->CreateSharedHandle(
      static_cast<ID3D12DeviceChild *>(this), attributes, access, name, handle);
}

HRESULT MTLD3D12Resource::GetDXGIUsage(DXGI_USAGE *usage) {
  if (!usage)
    return E_INVALIDARG;
  *usage = 0;
  return S_OK;
}

void MTLD3D12Resource::SetEvictionPriority(UINT priority) {
  switch (priority) {
  case DXGI_RESOURCE_PRIORITY_MINIMUM:
    m_residency.setPriority(D3D12_RESIDENCY_PRIORITY_MINIMUM);
    break;
  case DXGI_RESOURCE_PRIORITY_LOW:
    m_residency.setPriority(D3D12_RESIDENCY_PRIORITY_LOW);
    break;
  case DXGI_RESOURCE_PRIORITY_NORMAL:
    m_residency.setPriority(D3D12_RESIDENCY_PRIORITY_NORMAL);
    break;
  case DXGI_RESOURCE_PRIORITY_HIGH:
    m_residency.setPriority(D3D12_RESIDENCY_PRIORITY_HIGH);
    break;
  case DXGI_RESOURCE_PRIORITY_MAXIMUM:
    m_residency.setPriority(D3D12_RESIDENCY_PRIORITY_MAXIMUM);
    break;
  default:
    RTRACE("SetEvictionPriority ignored invalid priority=0x%x", priority);
    break;
  }
}

UINT MTLD3D12Resource::GetEvictionPriority() const {
  switch (m_residency.priority()) {
  case D3D12_RESIDENCY_PRIORITY_MINIMUM:
    return DXGI_RESOURCE_PRIORITY_MINIMUM;
  case D3D12_RESIDENCY_PRIORITY_LOW:
    return DXGI_RESOURCE_PRIORITY_LOW;
  case D3D12_RESIDENCY_PRIORITY_NORMAL:
    return DXGI_RESOURCE_PRIORITY_NORMAL;
  case D3D12_RESIDENCY_PRIORITY_HIGH:
    return DXGI_RESOURCE_PRIORITY_HIGH;
  case D3D12_RESIDENCY_PRIORITY_MAXIMUM:
    return DXGI_RESOURCE_PRIORITY_MAXIMUM;
  default:
    return DXGI_RESOURCE_PRIORITY_NORMAL;
  }
}

ULONG STDMETHODCALLTYPE MTLD3D12Resource::AddRef() { return ++m_refCount; }

ULONG STDMETHODCALLTYPE MTLD3D12Resource::Release() {
  uint32_t rc = --m_refCount;
  if (!rc) {
    uint32_t rp = --m_refPrivate;
    if (!rp) {
      m_refPrivate += 0x80000000;
      delete this;
    }
  }
  return rc;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::GetPrivateData(REFGUID guid, UINT *data_size, void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::SetPrivateData(REFGUID guid, UINT data_size,
                                 const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::SetPrivateDataInterface(REFGUID guid, const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Resource::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::GetDevice(REFIID riid, void **device) {
  return m_device->QueryInterface(riid, device);
}

bool MTLD3D12Resource::IsResident() const {
  return m_residency.isResident() &&
         (!m_parent_heap || m_parent_heap->IsResident());
}

void MTLD3D12Resource::MakeResident() {
  m_residency.makeResident();
  if (m_parent_heap)
    m_parent_heap->MakeResident();
}

void MTLD3D12Resource::Evict() { m_residency.evict(); }

void MTLD3D12Resource::ClearStencil(UINT mip, UINT slice, UINT8 value) {
  if (m_stencil_shadow.empty())
    return;
  const size_t offset = StencilShadowOffset(m_desc, mip, slice);
  const uint64_t width = std::max<uint64_t>(1, m_desc.Width >> mip);
  const uint64_t height = std::max<uint64_t>(1, m_desc.Height >> mip);
  if (offset == SIZE_MAX || offset > m_stencil_shadow.size() ||
      height > UINT64_MAX / width)
    return;
  const uint64_t size = width * height;
  if (size > m_stencil_shadow.size() - offset)
    return;
  for (uint64_t y = 0; y < height; ++y)
    std::memset(m_stencil_shadow.data() + offset + y * width, value,
                static_cast<size_t>(width));
}

void MTLD3D12Resource::SetParentHeap(MTLD3D12Heap *heap) {
  if (m_parent_heap == heap)
    return;
  if (heap)
    heap->AddRef();
  if (m_parent_heap)
    m_parent_heap->Release();
  m_parent_heap = heap;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::Map(UINT sub_resource,
                                                 const D3D12_RANGE *read_range,
                                                 void **data) {
  RTRACE("Map sub=%u", sub_resource);
  (void)read_range;
  if (!data)
    return E_POINTER;
  if (!IsResident()) {
    *data = nullptr;
    return DXGI_ERROR_INVALID_CALL;
  }
  if (read_range && m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
      (read_range->Begin > read_range->End ||
       read_range->End > m_desc.Width)) {
    *data = nullptr;
    return E_INVALIDARG;
  }
  if (m_desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    RTRACE("Map: invalid resource dimension=%u", (unsigned)m_desc.Dimension);
    *data = nullptr;
    return E_INVALIDARG;
  }
  const UINT mip_levels = std::max<UINT>(m_desc.MipLevels, 1);
  const UINT array_size =
      m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(m_desc.DepthOrArraySize, 1);
  if (sub_resource >= mip_levels * array_size) {
    *data = nullptr;
    return E_INVALIDARG;
  }
  if (m_cpu_addr &&
      !(m_is_reserved && m_heap_properties.Type == D3D12_HEAP_TYPE_DEFAULT)) {
    *data = m_cpu_addr;
    RTRACE("Map returning cpu_addr=%p gpu_addr=0x%llx", m_cpu_addr, (unsigned long long)m_gpu_addr);
    return S_OK;
  }
  RTRACE("Map FAILED - no cpu_addr");
  return E_FAIL;
}

void STDMETHODCALLTYPE MTLD3D12Resource::Unmap(
    UINT sub_resource, const D3D12_RANGE *written_range) {
  RTRACE("Unmap sub=%u written_range=%p", sub_resource, written_range);
  if (!m_cpu_addr || !m_mtl_buffer.handle ||
      m_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
    return;
  if (sub_resource != 0)
    return;
  const UINT64 start = written_range ? written_range->Begin : 0;
  const UINT64 end = written_range ? written_range->End : m_desc.Width;
  if (start >= end || start >= m_desc.Width)
    return;
  m_mtl_buffer.didModifyRange(
      start, std::min<UINT64>(end, m_desc.Width) - start);
}

D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
MTLD3D12Resource::GetDesc(D3D12_RESOURCE_DESC *__ret) {
  if (!__ret) {
    RTRACE("GetDesc called with null return pointer");
    return nullptr;
  }
  *__ret = m_desc;
  return __ret;
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
MTLD3D12Resource::GetGPUVirtualAddress() {
  RTRACE("GetGPUVirtualAddress -> 0x%llx this=%p is_buffer=%d", (unsigned long long)m_gpu_addr, (void*)this, IsBuffer());
  return m_gpu_addr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Resource::WriteToSubresource(
    UINT dst_sub_resource, const D3D12_BOX *dst_box, const void *src_data,
    UINT src_row_pitch, UINT src_slice_pitch) {
  RTRACE("WriteToSubresource sub=%u box=%p", dst_sub_resource, dst_box);
  if (!src_data)
    return E_POINTER;
  if (!IsResident())
    return DXGI_ERROR_INVALID_CALL;
  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (dst_sub_resource || dst_box || !src_row_pitch)
      return E_INVALIDARG;
    const uint64_t length = src_slice_pitch ? src_slice_pitch : src_row_pitch;
    if (!length || length > m_desc.Width || !m_mtl_buffer.handle)
      return E_INVALIDARG;
    if (m_cpu_addr) {
      std::memcpy(m_cpu_addr, src_data, static_cast<size_t>(length));
      return S_OK;
    }
    WMTBufferInfo staging_info = {};
    staging_info.length = length;
    staging_info.options = WMTResourceStorageModeShared;
    auto staging = m_device->GetDXMTDevice().device().newBuffer(staging_info);
    void *staging_data = staging_info.memory.get_accessible_or_null();
    if (!staging.handle || !staging_data)
      return E_FAIL;
    std::memcpy(staging_data, src_data, static_cast<size_t>(length));
    if (!SubmitBufferCopy(m_device->GetDXMTDevice().device(), staging, 0,
                          m_mtl_buffer, 0, length))
      return E_FAIL;
    return S_OK;
  }
  if (!src_row_pitch)
    return E_INVALIDARG;

  UINT mip = 0;
  UINT slice = 0;
  UINT plane = 0;
  DXGI_FORMAT plane_format = DXGI_FORMAT_UNKNOWN;
  WMTOrigin origin = {};
  WMTSize size = {};
  if (!ResolveSubresourceRegion(m_desc, dst_sub_resource, dst_box, mip, slice,
                                plane, plane_format, origin, size))
    return E_INVALIDARG;
  const uint64_t row_bytes =
      PackedTextureRowBytes(plane_format, size.width);
  if (src_row_pitch < row_bytes)
    return E_INVALIDARG;
  const uint32_t row_count = PackedTextureRowCount(plane_format, size.height);
  const uint64_t image_bytes = src_slice_pitch
                                   ? src_slice_pitch
                                   : uint64_t(src_row_pitch) * row_count;
  if (image_bytes < uint64_t(src_row_pitch) * row_count)
    return E_INVALIDARG;

  if (IsPlanarFormat(m_desc.Format)) {
    size_t shadow_offset = SIZE_MAX;
    uint64_t full_row_bytes = 0;
    uint64_t full_slice_bytes = 0;
    if (!PlanarShadowLayout(m_desc, plane, mip, slice, origin, size,
                            shadow_offset, full_row_bytes,
                            full_slice_bytes) ||
        shadow_offset > m_planar_shadow.size())
      return E_INVALIDARG;
    const uint64_t x_offset = PackedTextureRowBytes(plane_format, origin.x);
    const uint64_t copy_row_bytes =
        PackedTextureRowBytes(plane_format, size.width);
    const uint64_t copy_rows =
        PackedTextureRowCount(plane_format, size.height);
    for (uint64_t z = 0; z < size.depth; ++z) {
      for (uint64_t y = 0; y < copy_rows; ++y) {
        const uint64_t destination_offset =
            uint64_t(shadow_offset) + z * full_slice_bytes +
            (uint64_t(origin.y) + y) * full_row_bytes + x_offset;
        if (destination_offset > m_planar_shadow.size() ||
            copy_row_bytes > m_planar_shadow.size() - destination_offset)
          return E_INVALIDARG;
        std::memcpy(m_planar_shadow.data() + destination_offset,
                    static_cast<const uint8_t *>(src_data) +
                        z * image_bytes + y * src_row_pitch,
                    static_cast<size_t>(copy_row_bytes));
      }
    }
    return S_OK;
  }
  if (plane == 1 && IsStencilPlaneFormat(m_desc.Format)) {
    const size_t shadow_offset = StencilShadowOffset(m_desc, mip, slice);
    const uint64_t width = std::max<uint64_t>(1, m_desc.Width >> mip);
    if (shadow_offset == SIZE_MAX ||
        shadow_offset > m_stencil_shadow.size() ||
        size.width > width ||
        uint64_t(origin.y) + size.height >
            std::max<uint64_t>(1, m_desc.Height >> mip))
      return E_INVALIDARG;
    for (uint64_t y = 0; y < size.height; ++y)
      std::memcpy(m_stencil_shadow.data() + shadow_offset +
                      (uint64_t(origin.y) + y) * width + origin.x,
                  static_cast<const uint8_t *>(src_data) + y * src_row_pitch,
                  static_cast<size_t>(size.width));
    return S_OK;
  }
  if (plane != 0)
    return E_NOTIMPL;
  if (m_mtl_texture.handle) {
    if (m_heap_properties.Type == D3D12_HEAP_TYPE_DEFAULT) {
      if (image_bytes > UINT32_MAX ||
          (size.depth && image_bytes > UINT64_MAX / size.depth))
        return E_INVALIDARG;
      const uint64_t source_length = image_bytes * size.depth;
      return SubmitTextureUpload(
                 m_device->GetDXMTDevice().device(), m_mtl_texture, slice,
                 mip, origin, size, src_data, source_length, src_row_pitch,
                 static_cast<uint32_t>(image_bytes))
                 ? S_OK
                 : E_FAIL;
    }
    m_mtl_texture.replaceRegion(origin, size, mip, slice, src_data,
                                src_row_pitch, image_bytes);
    return S_OK;
  }
  RTRACE("WriteToSubresource failed: texture backing is unavailable");
  return E_FAIL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Resource::ReadFromSubresource(
    void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
    UINT src_sub_resource, const D3D12_BOX *src_box) {
  RTRACE("ReadFromSubresource dst=%p row_pitch=%u slice_pitch=%u sub=%u "
         "box=%p dim=%u this=%p",
         dst_data, dst_row_pitch, dst_slice_pitch, src_sub_resource, src_box,
         m_desc.Dimension, (void *)this);
  if (!dst_data)
    return E_POINTER;
  if (!IsResident())
    return DXGI_ERROR_INVALID_CALL;

  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (src_sub_resource || !dst_row_pitch || dst_row_pitch > m_desc.Width)
      return E_INVALIDARG;
    const uint64_t length = dst_row_pitch;
    if (m_cpu_addr) {
      std::memcpy(dst_data, m_cpu_addr, static_cast<size_t>(length));
      return S_OK;
    }
    if (!m_mtl_buffer.handle)
      return E_FAIL;
    WMTBufferInfo staging_info = {};
    staging_info.length = length;
    staging_info.options = WMTResourceStorageModeShared;
    auto staging = m_device->GetDXMTDevice().device().newBuffer(staging_info);
    void *staging_data = staging_info.memory.get_accessible_or_null();
    if (!staging.handle || !staging_data ||
        !SubmitBufferCopy(m_device->GetDXMTDevice().device(), m_mtl_buffer, 0,
                          staging, 0, length))
      return E_FAIL;
    std::memcpy(dst_data, staging_data, static_cast<size_t>(length));
    return S_OK;
  }

  if (m_desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
      !dst_row_pitch)
    return E_INVALIDARG;
  UINT mip = 0;
  UINT slice = 0;
  UINT plane = 0;
  DXGI_FORMAT plane_format = DXGI_FORMAT_UNKNOWN;
  WMTOrigin origin = {};
  WMTSize size = {};
  if (!ResolveSubresourceRegion(m_desc, src_sub_resource, src_box, mip, slice,
                                plane, plane_format, origin, size))
    return E_INVALIDARG;
  const uint64_t row_bytes = PackedTextureRowBytes(plane_format, size.width);
  if (dst_row_pitch < row_bytes)
    return E_INVALIDARG;
  const uint32_t row_count = PackedTextureRowCount(plane_format, size.height);
  const uint64_t slice_pitch = dst_slice_pitch
                                   ? dst_slice_pitch
                                   : uint64_t(dst_row_pitch) * row_count;
  if (slice_pitch < uint64_t(dst_row_pitch) * row_count)
    return E_INVALIDARG;
  if (IsPlanarFormat(m_desc.Format)) {
    size_t shadow_offset = SIZE_MAX;
    uint64_t full_row_bytes = 0;
    uint64_t full_slice_bytes = 0;
    if (!PlanarShadowLayout(m_desc, plane, mip, slice, origin, size,
                            shadow_offset, full_row_bytes,
                            full_slice_bytes) ||
        shadow_offset > m_planar_shadow.size())
      return E_INVALIDARG;
    const uint64_t x_offset = PackedTextureRowBytes(plane_format, origin.x);
    const uint64_t copy_row_bytes =
        PackedTextureRowBytes(plane_format, size.width);
    const uint64_t copy_rows =
        PackedTextureRowCount(plane_format, size.height);
    for (uint64_t z = 0; z < size.depth; ++z) {
      for (uint64_t y = 0; y < copy_rows; ++y) {
        const uint64_t source_offset =
            uint64_t(shadow_offset) + z * full_slice_bytes +
            (uint64_t(origin.y) + y) * full_row_bytes + x_offset;
        if (source_offset > m_planar_shadow.size() ||
            copy_row_bytes > m_planar_shadow.size() - source_offset)
          return E_INVALIDARG;
        std::memcpy(static_cast<uint8_t *>(dst_data) +
                        z * slice_pitch + y * dst_row_pitch,
                    m_planar_shadow.data() + source_offset,
                    static_cast<size_t>(copy_row_bytes));
      }
    }
    return S_OK;
  }
  if (plane == 1 && IsStencilPlaneFormat(m_desc.Format)) {
    const size_t shadow_offset = StencilShadowOffset(m_desc, mip, slice);
    const uint64_t width = std::max<uint64_t>(1, m_desc.Width >> mip);
    if (shadow_offset == SIZE_MAX ||
        shadow_offset > m_stencil_shadow.size() ||
        size.width > width ||
        uint64_t(origin.y) + size.height >
            std::max<uint64_t>(1, m_desc.Height >> mip))
      return E_INVALIDARG;
    for (uint64_t y = 0; y < size.height; ++y)
      std::memcpy(static_cast<uint8_t *>(dst_data) + y * dst_row_pitch,
                  m_stencil_shadow.data() + shadow_offset +
                      (uint64_t(origin.y) + y) * width + origin.x,
                  static_cast<size_t>(size.width));
    return S_OK;
  }
  if (plane != 0)
    return E_NOTIMPL;
  if (!m_mtl_texture.handle)
    return E_FAIL;

  WMTBufferInfo staging_info = {};
  staging_info.length = slice_pitch * size.depth;
  staging_info.options = WMTResourceStorageModeShared;
  auto staging = m_device->GetDXMTDevice().device().newBuffer(staging_info);
  void *staging_data = staging_info.memory.get_accessible_or_null();
  if (!staging.handle || !staging_data ||
      !SubmitTextureReadback(m_device->GetDXMTDevice().device(), m_mtl_texture,
                             slice, mip, origin, size, staging, 0,
                             dst_row_pitch, static_cast<uint32_t>(slice_pitch)))
    return E_FAIL;

  const auto *source = static_cast<const uint8_t *>(staging_data);
  auto *destination = static_cast<uint8_t *>(dst_data);
  for (uint64_t z = 0; z < size.depth; z++) {
    for (uint64_t y = 0; y < row_count; y++) {
      std::memcpy(destination + z * slice_pitch + y * dst_row_pitch,
                  source + z * slice_pitch + y * dst_row_pitch,
                  static_cast<size_t>(row_bytes));
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::GetHeapProperties(D3D12_HEAP_PROPERTIES *heap_properties,
                                    D3D12_HEAP_FLAGS *flags) {
  if (heap_properties)
    *heap_properties = m_heap_properties;
  if (flags)
    *flags = m_heap_flags;
  return S_OK;
}

} // namespace dxmt
