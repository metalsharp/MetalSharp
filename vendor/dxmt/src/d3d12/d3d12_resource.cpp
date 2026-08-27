#include "d3d12_resource.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pipeline_state.hpp"
#include "d3d12_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

#define RTRACE(fmt, ...) DXMTD3D12Trace("Resource", fmt, ##__VA_ARGS__)

namespace dxmt {

static std::atomic<uint64_t> g_next_texture_virtual_address{0x200000000000ull};

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

static D3D12_TILE_SHAPE TileShapeForFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
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
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return {64, 64, 1};
  default:
    return {128, 128, 1};
  }
}

static uint64_t SparseHeapSizeForResource(const D3D12_RESOURCE_DESC &desc) {
  const D3D12_TILE_SHAPE shape = TileShapeForFormat(desc.Format);
  const uint64_t slices = std::max<uint32_t>(1, desc.DepthOrArraySize);
  const uint64_t mips = std::max<uint32_t>(1, desc.MipLevels);
  uint64_t tile_count = 0;
  for (uint64_t slice = 0; slice < slices; slice++) {
    for (uint64_t mip = 0; mip < mips; mip++) {
      const uint64_t width = std::max<uint64_t>(1, desc.Width >> mip);
      const uint64_t height = std::max<uint64_t>(1, desc.Height >> mip);
      const uint64_t tiles_x =
          (width + shape.WidthInTexels - 1) / shape.WidthInTexels;
      const uint64_t tiles_y =
          (height + shape.HeightInTexels - 1) / shape.HeightInTexels;
      tile_count += tiles_x * tiles_y;
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
    : m_device(device), m_desc(desc), m_state(initial_state),
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
    : m_device(device), m_desc(desc), m_state(initial_state),
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
    : m_device(device), m_desc(desc), m_state(initial_state),
      m_heap_properties(heap_properties), m_heap_flags(heap_flags),
      m_tex_gpu_resource_id(backing_texture_gpu_id),
      m_backing_offset(backing_offset) {
  m_mtl_texture = std::move(backing_texture);
  InitializeResource(WMT::Reference<WMT::Buffer>{}, nullptr, 0, 0);
}

void MTLD3D12Resource::InitializeResource(
    WMT::Reference<WMT::Buffer> backing_buffer, void *backing_cpu_addr,
    uint64_t backing_gpu_addr, uint64_t backing_offset) {
  m_device->AddRef();

  auto wmt_device = m_device->GetDXMTDevice().device();
  RTRACE("ctor: wmt_device=%llu dim=%u fmt=%u w=%llu h=%u depth_or_arr=%u",
    (unsigned long long)wmt_device.handle, m_desc.Dimension, m_desc.Format,
    m_desc.Width, m_desc.Height, m_desc.DepthOrArraySize);

  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    bool cpu_accessible =
        m_is_reserved || m_heap_properties.Type == D3D12_HEAP_TYPE_UPLOAD ||
        m_heap_properties.Type == D3D12_HEAP_TYPE_READBACK;
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
    bool cpu_accessible = (m_heap_properties.Type == D3D12_HEAP_TYPE_UPLOAD ||
                           m_heap_properties.Type == D3D12_HEAP_TYPE_READBACK);
    WMTTextureInfo tex_info = {};
    tex_info.width = m_desc.Width;
    tex_info.height = m_desc.Height;
    tex_info.depth = (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                         ? m_desc.DepthOrArraySize
                         : 1;
    tex_info.array_length =
        (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            ? m_desc.DepthOrArraySize
            : 1;
    tex_info.mipmap_level_count = m_desc.MipLevels ? m_desc.MipLevels : 1;
    tex_info.type = TextureTypeForResourceDesc(m_desc);
    tex_info.sample_count = SampleCountForResourceDesc(m_desc, tex_info.type);
    if (tex_info.sample_count > 1)
      tex_info.mipmap_level_count = 1;
    tex_info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget |
                                      WMTTextureUsageShaderRead |
                                      WMTTextureUsageShaderWrite |
                                      WMTTextureUsagePixelFormatView);
    tex_info.options = cpu_accessible ? WMTResourceStorageModeShared : WMTResourceStorageModePrivate;
    tex_info.pixel_format = MTLD3D12PipelineState::DXGIToMTLPixelFormat(static_cast<DXGI_FORMAT>(m_desc.Format));
    if (tex_info.pixel_format == WMTPixelFormatInvalid)
      tex_info.pixel_format = WMTPixelFormatBGRA8Unorm;

    RTRACE("ctor: about to newTexture type=%u fmt=%u %ux%u depth=%u arr=%u mip=%u sample=%u opts=%u",
      tex_info.type, tex_info.pixel_format, (unsigned)tex_info.width, (unsigned)tex_info.height,
      (unsigned)tex_info.depth, (unsigned)tex_info.array_length,
       (unsigned)tex_info.mipmap_level_count, (unsigned)tex_info.sample_count, (unsigned)tex_info.options);
    if (!m_mtl_texture.handle) {
      if (m_is_reserved) {
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
      } else {
        m_mtl_texture = wmt_device.newTexture(tex_info);
      }
      m_tex_gpu_resource_id = tex_info.gpu_resource_id;
    }
    if (!m_mtl_texture.handle) {
      RTRACE("ctor: texture creation FAILED type=%u fmt=%u %ux%u arr=%u",
        tex_info.type, tex_info.pixel_format, (unsigned)tex_info.width, (unsigned)tex_info.height, (unsigned)tex_info.array_length);
    } else {
      RTRACE("ctor: texture created fmt=%u %ux%u arr=%u handle=%llu %s",
        tex_info.pixel_format, (unsigned)tex_info.width, (unsigned)tex_info.height, (unsigned)tex_info.array_length,
        (unsigned long long)m_mtl_texture.handle, cpu_accessible ? "cpu" : "gpu");
    }
    // Textures do not expose a real D3D12 GPU virtual address. Older bridge
    // code allocated a same-sized fake Metal buffer here, which doubled memory
    // pressure for large render targets before UE5/Nanite transient heaps.
    m_gpu_addr = g_next_texture_virtual_address.fetch_add(0x10000ull);
    RTRACE("ctor: texture synthetic gpu_addr=0x%llx (no backing buffer)", (unsigned long long)m_gpu_addr);
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
    bool cpu_accessible = (m_heap_properties.Type == D3D12_HEAP_TYPE_UPLOAD ||
                           m_heap_properties.Type == D3D12_HEAP_TYPE_READBACK);
    auto wmt_device = m_device->GetDXMTDevice().device();
    WMTTextureInfo tex_info = {};
    tex_info.width = m_desc.Width ? m_desc.Width : 1;
    tex_info.height = m_desc.Height ? m_desc.Height : 1;
    tex_info.depth = (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                          ? m_desc.DepthOrArraySize : 1;
    tex_info.array_length = (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                                  ? m_desc.DepthOrArraySize : 1;
    tex_info.mipmap_level_count = m_desc.MipLevels ? m_desc.MipLevels : 1;
    tex_info.type = TextureTypeForResourceDesc(m_desc);
    tex_info.sample_count = SampleCountForResourceDesc(m_desc, tex_info.type);
    if (tex_info.sample_count > 1)
      tex_info.mipmap_level_count = 1;
    tex_info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite | WMTTextureUsagePixelFormatView);
    tex_info.options = cpu_accessible ? WMTResourceStorageModeShared : WMTResourceStorageModePrivate;
    tex_info.pixel_format = MTLD3D12PipelineState::DXGIToMTLPixelFormat(static_cast<DXGI_FORMAT>(m_desc.Format));
    if (tex_info.pixel_format == WMTPixelFormatInvalid)
      tex_info.pixel_format = WMTPixelFormatBGRA8Unorm;
    RTRACE("GetMTLTexture: creating type=%u fmt=%u %ux%ux%u arr=%u mip=%u sample=%u opts=%u",
      tex_info.type, tex_info.pixel_format, (unsigned)tex_info.width, (unsigned)tex_info.height,
      (unsigned)tex_info.depth, (unsigned)tex_info.array_length, (unsigned)tex_info.mipmap_level_count,
      (unsigned)tex_info.sample_count, (unsigned)tex_info.options);
    m_mtl_texture = wmt_device.newTexture(tex_info);
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
  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return m_desc.DepthOrArraySize ? m_desc.DepthOrArraySize : 1;
  return 1;
}

uint64_t MTLD3D12Resource::GetBufferByteLength() const {
  if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return m_desc.Width;
  return m_buf_info.length;
}

D3D12_TILE_SHAPE MTLD3D12Resource::GetTiledResourceTileShape() const {
  if (IsBuffer())
    return {D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, 1, 1};
  return TileShapeForFormat(m_desc.Format);
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
  RTRACE("QI unknown IID %s -> E_NOINTERFACE", str::format(riid).c_str());
  return E_NOINTERFACE;
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
HRESULT STDMETHODCALLTYPE
MTLD3D12Resource::Map(UINT sub_resource,
                                                 const D3D12_RANGE *read_range,
                                                 void **data) {
  RTRACE("Map sub=%u", sub_resource);
  (void)read_range;
  if (!data)
    return E_POINTER;
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
    UINT sub_resource, const D3D12_RANGE *written_range) {}

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
  if (m_desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return E_INVALIDARG;
  const UINT mip_levels = std::max<UINT>(m_desc.MipLevels, 1);
  const UINT array_size =
      m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(m_desc.DepthOrArraySize, 1);
  if (dst_sub_resource >= mip_levels * array_size)
    return E_INVALIDARG;
  if (m_cpu_addr) {
    if (dst_box) {
      UINT rows = dst_box->bottom - dst_box->top;
      UINT depth = dst_box->back - dst_box->front;
      for (UINT z = 0; z < depth; z++) {
        for (UINT y = 0; y < rows; y++) {
          memcpy((char *)m_cpu_addr + (dst_box->front + z) * src_slice_pitch + (dst_box->top + y) * src_row_pitch + dst_box->left,
                 (char *)src_data + z * src_slice_pitch + y * src_row_pitch,
                 dst_box->right - dst_box->left);
        }
      }
    } else {
      memcpy(m_cpu_addr, src_data, src_slice_pitch ? src_slice_pitch : src_row_pitch);
    }
    return S_OK;
  }
  RTRACE("WriteToSubresource unsupported without CPU-visible backing");
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Resource::ReadFromSubresource(
    void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
    UINT src_sub_resource, const D3D12_BOX *src_box) {
  void *vtable = *(void**)this;
  RTRACE("ReadFromSubresource dst=%p row_pitch=%u slice_pitch=%u sub=%u box=%p dim=%u this=%p vtable=%p", dst_data, dst_row_pitch, dst_slice_pitch, src_sub_resource, src_box, m_desc.Dimension, (void*)this, vtable);
  if (!dst_data)
    return E_POINTER;
  if (m_desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    RTRACE("ReadFromSubresource: invalid dimension %u", m_desc.Dimension);
    return E_INVALIDARG;
  }
  const UINT mip_levels = std::max<UINT>(m_desc.MipLevels, 1);
  const UINT array_size =
      m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(m_desc.DepthOrArraySize, 1);
  if (src_sub_resource >= mip_levels * array_size)
    return E_INVALIDARG;
  if (m_cpu_addr) {
    UINT rows = m_desc.Height ? m_desc.Height : 1;
    if (src_box) {
      UINT copy_rows = src_box->bottom - src_box->top;
      UINT copy_depth = src_box->back - src_box->front;
      UINT copy_width = src_box->right - src_box->left;
      for (UINT z = 0; z < copy_depth; z++) {
        for (UINT y = 0; y < copy_rows; y++) {
          memcpy((char *)dst_data + z * dst_slice_pitch + y * dst_row_pitch,
                 (char *)m_cpu_addr + (src_box->front + z) * rows * dst_row_pitch + (src_box->top + y) * dst_row_pitch + src_box->left,
                 copy_width);
        }
      }
    } else {
      memcpy(dst_data, m_cpu_addr, dst_slice_pitch ? dst_slice_pitch : dst_row_pitch);
    }
    return S_OK;
  }
  RTRACE("ReadFromSubresource unsupported without CPU-visible backing");
  return E_NOTIMPL;
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
