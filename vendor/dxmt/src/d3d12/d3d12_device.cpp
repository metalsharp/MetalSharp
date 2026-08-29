#define INITGUID
#include "d3d12_command_queue.hpp"
#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_device.hpp"
#include "d3d12_trace.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_native_tessellation_path.hpp"
#include "d3d12_pipeline_state.hpp"
#include "d3d12_query_heap.hpp"
#include "d3d12_resource.hpp"

#define TRACE(fmt, ...) DXMTD3D12Trace("Device", fmt, ##__VA_ARGS__)
#define PLTRACE(fmt, ...) TRACE(fmt, ##__VA_ARGS__)
#include "d3d12_root_signature.hpp"
#include "com/com_object.hpp"
#include "config/config.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_string.hpp"
#include "d3d12_resource.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <type_traits>
#include <utility>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <windows.h>

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
      ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
    FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
    if (f) {
      fprintf(f, "!!! EXCEPTION code=0x%lx addr=%p flags=0x%lx\n",
              ep->ExceptionRecord->ExceptionCode,
              ep->ExceptionRecord->ExceptionAddress,
              ep->ExceptionRecord->ExceptionFlags);
#if defined(__x86_64__) || defined(_M_X64)
      CONTEXT *ctx = ep->ContextRecord;
      fprintf(f,
              "!!! CONTEXT rip=%p rsp=%p rbp=%p rax=%p rbx=%p rcx=%p rdx=%p "
              "r8=%p r9=%p r10=%p r11=%p\n",
              (void *)ctx->Rip, (void *)ctx->Rsp, (void *)ctx->Rbp,
              (void *)ctx->Rax, (void *)ctx->Rbx, (void *)ctx->Rcx,
              (void *)ctx->Rdx, (void *)ctx->R8, (void *)ctx->R9,
              (void *)ctx->R10, (void *)ctx->R11);
      uintptr_t *stack = reinterpret_cast<uintptr_t *>(ctx->Rsp);
      fprintf(f, "!!! STACK:");
      for (int i = 0; i < 16; i++) {
        fprintf(f, " [%02d]=%p", i, (void *)stack[i]);
      }
      fprintf(f, "\n");
      auto readable = [](uintptr_t value, size_t bytes) -> bool {
        if (!value || value < 0x10000)
          return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void *>(value), &mbi,
                          sizeof(mbi))) {
          return false;
        }
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) {
          return false;
        }
        uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return value + bytes <= region_end;
      };
      auto dump_object = [&](const char *name, uintptr_t value) {
        if (!readable(value, sizeof(uintptr_t)))
          return;
        uintptr_t *object = reinterpret_cast<uintptr_t *>(value);
        uintptr_t vtable = object[0];
        fprintf(f, "!!! %s object=%p vtable=%p", name, (void *)value,
                (void *)vtable);
        if (readable(vtable, sizeof(uintptr_t) * 96)) {
          uintptr_t *slots = reinterpret_cast<uintptr_t *>(vtable);
          for (int i = 0; i < 96; i++) {
            fprintf(f, " [%02d]=%p", i, (void *)slots[i]);
          }
        } else {
          fprintf(f, " slots-unreadable");
        }
        fprintf(f, "\n");
      };
      dump_object("RCX", ctx->Rcx);
      dump_object("RDX", ctx->Rdx);
      dump_object("R8", ctx->R8);
      dump_object("R9", ctx->R9);
#endif
      void *buf[32];
      ULONG n = RtlCaptureStackBackTrace(0, 32, buf, nullptr);
      for (ULONG i = 0; i < n; i++) {
        fprintf(f, "  [%lu] %p\n", (unsigned long)i, buf[i]);
      }
      fclose(f);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void install_crash_handler() { AddVectoredExceptionHandler(1, crash_handler); }

namespace dxmt {

D3D_FEATURE_LEVEL D3D12ConfiguredMaximumFeatureLevel() {
  static const D3D_FEATURE_LEVEL maximum = [] {
    const auto configured = Config::getInstance().getOption<std::string>(
        "d3d12.maxFeatureLevel", "");
    if (configured.empty())
      return kD3D12BuildMaximumFeatureLevel;

    static const std::pair<const char *, D3D_FEATURE_LEVEL> levels[] = {
        {"11_0", D3D_FEATURE_LEVEL_11_0},
        {"11_1", D3D_FEATURE_LEVEL_11_1},
        {"12_0", D3D_FEATURE_LEVEL_12_0},
        {"12_1", D3D_FEATURE_LEVEL_12_1},
        {"12_2", D3D_FEATURE_LEVEL_12_2},
    };
    D3D_FEATURE_LEVEL requested = kD3D12BuildMaximumFeatureLevel;
    bool valid = false;
    for (const auto &[name, level] : levels) {
      if (configured == name) {
        requested = level;
        valid = true;
        break;
      }
    }
    if (!valid) {
      Logger::warn(str::format("D3D12 invalid maxFeatureLevel=", configured,
                               "; using build maximum"));
      return kD3D12BuildMaximumFeatureLevel;
    }
    return std::min(requested, kD3D12BuildMaximumFeatureLevel);
  }();
  return maximum;
}

static const GUID IID_ID3D12Device11_ = {
    0x5405c344,
    0xd457,
    0x444e,
    {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
static const GUID IID_ID3D12Device12_ = {
    0x5af5c532,
    0x4c91,
    0x4cd0,
    {0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5}};
static const GUID IID_ID3D12PipelineLibrary_ = {
    0xc64226a8,
    0x9201,
    0x46af,
    {0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f}};
static const GUID IID_ID3D12PipelineLibrary1_ = {
    0x80eabf42,
    0x2568,
    0x4e5e,
    {0xbd, 0x82, 0xc3, 0x7f, 0x86, 0x96, 0x1d, 0xc3}};
static const GUID IID_ID3D12StateObjectProperties1_ = {
    0x460caac7,
    0x1d24,
    0x446a,
    {0xa1, 0x84, 0xca, 0x67, 0xdb, 0x49, 0x41, 0x38}};
static const GUID IID_ID3D12StateObjectProperties2_ = {
    0xd5e82917,
    0xf0f1,
    0x44cf,
    {0xae, 0x5e, 0xce, 0x22, 0x2d, 0xd0, 0xb8, 0x84}};

namespace {

struct D3D12SharedHandleEntry {
  IUnknown *object = nullptr;
  HANDLE retained_handle = nullptr;
};

static bool IsValidSharedHandleAccess(DWORD access) {
  if (!access)
    return false;
  constexpr DWORD kGenericAccess = GENERIC_READ | GENERIC_WRITE | GENERIC_ALL;
  return (access & ~kGenericAccess) == 0;
}

static DWORD FileMappingAccessForSharedHandle(DWORD access) {
  if (access & GENERIC_ALL)
    return FILE_MAP_ALL_ACCESS;
  DWORD mapping_access = 0;
  if (access & GENERIC_READ)
    mapping_access |= FILE_MAP_READ;
  if (access & GENERIC_WRITE)
    mapping_access |= FILE_MAP_WRITE;
  return mapping_access;
}

std::mutex g_shared_handle_mutex;
std::unordered_map<HANDLE, D3D12SharedHandleEntry> g_shared_handles;
std::unordered_map<std::wstring, HANDLE> g_named_shared_handles;

void ReleaseSharedHandleEntry(D3D12SharedHandleEntry &entry) {
  if (entry.object)
    entry.object->Release();
  if (entry.retained_handle)
    CloseHandle(entry.retained_handle);
  entry = {};
}

enum class D3D12ResidencyObjectKind {
  Invalid,
  Resource,
  Heap,
  DescriptorHeap,
  QueryHeap,
};

static D3D12ResidencyObjectKind
ClassifyResidencyObject(ID3D12Pageable *object) {
  if (!object)
    return D3D12ResidencyObjectKind::Invalid;
  ID3D12Resource *resource = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
    resource->Release();
    return D3D12ResidencyObjectKind::Resource;
  }
  ID3D12Heap *heap = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&heap)))) {
    heap->Release();
    return D3D12ResidencyObjectKind::Heap;
  }
  ID3D12DescriptorHeap *descriptor_heap = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&descriptor_heap)))) {
    descriptor_heap->Release();
    return D3D12ResidencyObjectKind::DescriptorHeap;
  }
  ID3D12QueryHeap *query_heap = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&query_heap)))) {
    query_heap->Release();
    return D3D12ResidencyObjectKind::QueryHeap;
  }
  return D3D12ResidencyObjectKind::Invalid;
}

static bool IsValidResidencyPriority(D3D12_RESIDENCY_PRIORITY priority) {
  switch (priority) {
  case D3D12_RESIDENCY_PRIORITY_MINIMUM:
  case D3D12_RESIDENCY_PRIORITY_LOW:
  case D3D12_RESIDENCY_PRIORITY_NORMAL:
  case D3D12_RESIDENCY_PRIORITY_HIGH:
  case D3D12_RESIDENCY_PRIORITY_MAXIMUM:
    return true;
  default:
    return false;
  }
}

struct D3D12SharedHandleRegistryCleanup {
  ~D3D12SharedHandleRegistryCleanup() {
    std::lock_guard lock(g_shared_handle_mutex);
    for (auto &[handle, entry] : g_shared_handles) {
      (void)handle;
      ReleaseSharedHandleEntry(entry);
    }
    g_shared_handles.clear();
    g_named_shared_handles.clear();
  }
};

D3D12SharedHandleRegistryCleanup g_shared_handle_registry_cleanup;

static UINT64 AlignTo(UINT64 value, UINT64 alignment) {
  return alignment ? ((value + alignment - 1) & ~(alignment - 1)) : value;
}

// The stable 1.619 headers add this flag, while the host MinGW headers used
// for the Wine build may predate it.
static constexpr D3D12_RESOURCE_FLAGS kD3D12ResourceFlagUseTightAlignment =
    static_cast<D3D12_RESOURCE_FLAGS>(0x400);

static UINT FormatBlockSize(DXGI_FORMAT format) {
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
    return 4;
  default:
    return 1;
  }
}

static UINT FormatBytesPerTexel(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return 16;
  case DXGI_FORMAT_R32G32B32_TYPELESS:
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
    return 12;
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
    return 8;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_R16G16_TYPELESS:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_TYPELESS:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    return 4;
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
    return 2;
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_A8_UNORM:
    return 1;
  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS:
  case DXGI_FORMAT_BC4_UNORM:
  case DXGI_FORMAT_BC4_SNORM:
    return 8;
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
    return 16;
  default:
    return 0;
  }
}

static UINT FormatPlaneCount(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
  case DXGI_FORMAT_420_OPAQUE:
    return 2;
  default:
    return 1;
  }
}

static DXGI_FORMAT CopyFootprintPlaneFormat(DXGI_FORMAT format,
                                             UINT plane_slice) {
  switch (format) {
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    return plane_slice ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_420_OPAQUE:
    return plane_slice ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
    return plane_slice ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
  default:
    return format;
  }
}

static void AdjustCopyFootprintPlaneDimensions(DXGI_FORMAT format,
                                                UINT plane_slice,
                                                UINT64 &width, UINT64 &height) {
  if (plane_slice != 1)
    return;
  switch (format) {
  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
  case DXGI_FORMAT_420_OPAQUE:
    width = std::max<UINT64>(1, (width + 1) / 2);
    height = std::max<UINT64>(1, (height + 1) / 2);
    break;
  default:
    break;
  }
}

static UINT64 ResourcePlacementAlignment(const D3D12_RESOURCE_DESC &desc) {
  if ((desc.Flags & kD3D12ResourceFlagUseTightAlignment) &&
      desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    // GetResourceAllocationInfo is used to place resources in explicit heaps;
    // the tight-alignment buffer contract permits at most 256-byte placement
    // alignment and defaults to that maximum when no floor is supplied.
    return desc.Alignment ? std::max<UINT64>(desc.Alignment, 8) : 256;
  }
  if (desc.Alignment)
    return desc.Alignment;
  if (desc.SampleDesc.Count > 1)
    return D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
  return D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
}

static bool IsPowerOfTwo(UINT64 value) {
  return value && (value & (value - 1)) == 0;
}

static UINT FullMipLevelCount(const D3D12_RESOURCE_DESC &desc) {
  UINT64 largest = std::max<UINT64>(desc.Width, 1);
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    largest = std::max<UINT64>(largest, std::max<UINT>(desc.Height, 1));
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    largest = std::max<UINT64>(
        largest, std::max<UINT16>(desc.DepthOrArraySize, 1));
  UINT levels = 1;
  while (largest > 1) {
    largest >>= 1;
    ++levels;
  }
  return levels;
}

static D3D12_RESOURCE_DESC NormalizeResourceDesc(
    const D3D12_RESOURCE_DESC &desc) {
  D3D12_RESOURCE_DESC normalized = desc;
  if (normalized.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
      normalized.MipLevels == 0 && normalized.SampleDesc.Count == 1)
    normalized.MipLevels = static_cast<UINT16>(FullMipLevelCount(normalized));
  return normalized;
}

static bool IsDepthStencilFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_D16_UNORM:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_D32_FLOAT:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    return true;
  default:
    return false;
  }
}

static bool IsDepthStencilResourceFormat(DXGI_FORMAT format) {
  return IsDepthStencilFormat(format) ||
         format == DXGI_FORMAT_R32_TYPELESS ||
         format == DXGI_FORMAT_R24G8_TYPELESS ||
         format == DXGI_FORMAT_R32G8X24_TYPELESS;
}

static bool AreClearFormatsCompatible(DXGI_FORMAT resource_format,
                                       DXGI_FORMAT clear_format) {
  if (resource_format == clear_format)
    return true;
  switch (resource_format) {
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    return clear_format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           clear_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           clear_format == DXGI_FORMAT_R8G8B8A8_UINT ||
           clear_format == DXGI_FORMAT_R8G8B8A8_SINT;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    return clear_format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           clear_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  case DXGI_FORMAT_R32_TYPELESS:
    return clear_format == DXGI_FORMAT_R32_FLOAT ||
           clear_format == DXGI_FORMAT_R32_UINT ||
           clear_format == DXGI_FORMAT_R32_SINT ||
           clear_format == DXGI_FORMAT_D32_FLOAT;
  case DXGI_FORMAT_R24G8_TYPELESS:
    return clear_format == DXGI_FORMAT_D24_UNORM_S8_UINT;
  case DXGI_FORMAT_R32G8X24_TYPELESS:
    return clear_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  default:
    return false;
  }
}

static bool IsValidOptimizedClearValue(
    const D3D12_RESOURCE_DESC &desc,
    const D3D12_CLEAR_VALUE *clear_value) {
  if (!clear_value)
    return true;
  const bool render_target =
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
  const bool depth_stencil =
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  if (!render_target && !depth_stencil)
    return false;
  const bool clear_is_depth = IsDepthStencilFormat(clear_value->Format);
  if (clear_is_depth != depth_stencil ||
      !AreClearFormatsCompatible(desc.Format, clear_value->Format))
    return false;
  return true;
}

static bool IsValidHeapProperties(
    const D3D12_HEAP_PROPERTIES &properties) {
  const UINT type = static_cast<UINT>(properties.Type);
  if (type < static_cast<UINT>(D3D12_HEAP_TYPE_DEFAULT) || type > 5)
    return false;
  const UINT creation_mask = properties.CreationNodeMask;
  const UINT visible_mask = properties.VisibleNodeMask;
  if (creation_mask && (creation_mask & (creation_mask - 1)) != 0)
    return false;
  if (creation_mask && visible_mask &&
      (visible_mask & creation_mask) != creation_mask)
    return false;
  const bool custom = type == static_cast<UINT>(D3D12_HEAP_TYPE_CUSTOM);
  if (custom)
    return properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
           properties.MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN;
  return properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
         properties.MemoryPoolPreference == D3D12_MEMORY_POOL_UNKNOWN;
}

static bool IsValidInitialResourceState(D3D12_HEAP_TYPE heap_type,
                                         D3D12_RESOURCE_STATES state) {
  switch (static_cast<UINT>(heap_type)) {
  case static_cast<UINT>(D3D12_HEAP_TYPE_UPLOAD):
  case 5: // D3D12_HEAP_TYPE_GPU_UPLOAD in newer headers.
    return state == D3D12_RESOURCE_STATE_GENERIC_READ;
  case static_cast<UINT>(D3D12_HEAP_TYPE_READBACK):
    return state == D3D12_RESOURCE_STATE_COPY_DEST;
  default:
    return true;
  }
}

static bool IsResourceAllowedByHeapFlags(
    const D3D12_RESOURCE_DESC &desc, D3D12_HEAP_FLAGS heap_flags) {
  const UINT flags = static_cast<UINT>(heap_flags);
  const bool buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
  if (buffer)
    return (flags & static_cast<UINT>(D3D12_HEAP_FLAG_DENY_BUFFERS)) == 0;
  if (flags & static_cast<UINT>(D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS))
    return false;
  const bool render_target =
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
  const bool depth_stencil =
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  const bool rt_or_ds = render_target || depth_stencil;
  if ((flags & static_cast<UINT>(D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES)) &&
      rt_or_ds)
    return false;
  if ((flags & static_cast<UINT>(D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES)) &&
      !rt_or_ds)
    return false;
  return true;
}

static bool IsValidResourceDesc(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension < D3D12_RESOURCE_DIMENSION_BUFFER ||
      desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D || !desc.Width ||
      !desc.SampleDesc.Count)
    return false;

  const bool tight_alignment =
      (desc.Flags & kD3D12ResourceFlagUseTightAlignment) != 0;
  const bool render_target =
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
  const bool depth_stencil =
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  const bool unordered_access =
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
  if ((render_target && depth_stencil) ||
      (depth_stencil && unordered_access) ||
      (depth_stencil && !IsDepthStencilResourceFormat(desc.Format)) ||
      (render_target && IsDepthStencilResourceFormat(desc.Format)) ||
      (unordered_access && IsDepthStencilResourceFormat(desc.Format)))
    return false;
  if (desc.Alignment && !IsPowerOfTwo(desc.Alignment))
    return false;
  if (!tight_alignment && desc.Alignment &&
      desc.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
      desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
      desc.Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
    return false;

  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (desc.Height != 1 || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.Format != DXGI_FORMAT_UNKNOWN ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
        desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR || render_target ||
        depth_stencil)
      return false;
    if (tight_alignment &&
        ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) ||
         (desc.Alignment && desc.Alignment > 4096)))
      return false;
    return true;
  }

  if (tight_alignment) {
    // Tier 1 is deliberately scoped to buffers. A texture request using the
    // flag must not be accepted as though it received tight placement.
    return false;
  }

  if (!desc.Height || !desc.DepthOrArraySize ||
      (desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality != 0) ||
      ((desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010 ||
        desc.Format == DXGI_FORMAT_P016 || desc.Format == DXGI_FORMAT_420_OPAQUE) &&
       (desc.Height & 1)) ||
      (desc.MipLevels && desc.MipLevels > FullMipLevelCount(desc)) ||
      desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
      (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
       (desc.Height != 1 || desc.SampleDesc.Count != 1)) ||
      (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
       desc.SampleDesc.Count != 1))
    return false;

  // A multisampled resource has exactly one mip and uses the normal tiled
  // texture layout. Do not silently normalize malformed descriptors.
  if (desc.SampleDesc.Count > 1 &&
      (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
       desc.MipLevels != 1 || desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN))
    return false;
  return true;
}

static UINT64 EstimateResourceAllocationSize(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return AlignTo(std::max<UINT64>(desc.Width, 1),
                   ResourcePlacementAlignment(desc));

  UINT mip_levels = std::max<UINT>(desc.MipLevels, 1);
  // A 3D resource stores depth in each mip and has no array slices. Treating
  // DepthOrArraySize as an array count here double-counts every volume.
  UINT array_size = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                         ? 1
                         : std::max<UINT>(desc.DepthOrArraySize, 1);
  const UINT plane_count = FormatPlaneCount(desc.Format);

  UINT64 total = 0;
  for (UINT plane = 0; plane < plane_count; ++plane) {
    const DXGI_FORMAT plane_format =
        CopyFootprintPlaneFormat(desc.Format, plane);
    UINT bytes_per_texel = FormatBytesPerTexel(plane_format);
    UINT block_size = FormatBlockSize(plane_format);
    if (!bytes_per_texel)
      bytes_per_texel = 4;
    for (UINT array_or_plane = 0; array_or_plane < array_size;
         array_or_plane++) {
      for (UINT mip = 0; mip < mip_levels; mip++) {
        UINT64 width = std::max<UINT64>(1, desc.Width >> mip);
        UINT64 height = std::max<UINT64>(1, desc.Height >> mip);
        AdjustCopyFootprintPlaneDimensions(desc.Format, plane, width, height);
        UINT64 depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                           ? std::max<UINT64>(1, desc.DepthOrArraySize >> mip)
                           : 1;
        UINT64 width_blocks =
            std::max<UINT64>(1, AlignTo(width, block_size) / block_size);
        UINT64 rows =
            std::max<UINT64>(1, AlignTo(height, block_size) / block_size);
        UINT64 row_pitch = AlignTo(width_blocks * bytes_per_texel,
                                   D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        total += AlignTo(row_pitch * rows * depth,
                         D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }
    }
  }

  return AlignTo(
      std::max<UINT64>(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
      ResourcePlacementAlignment(desc));
}

static WMTTextureType
TextureTypeForSrvView(const D3D12_SHADER_RESOURCE_VIEW_DESC &desc,
                      const D3D12_RESOURCE_DESC &resource_desc) {
  switch (desc.ViewDimension) {
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    return WMTTextureType1D;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    return WMTTextureType1DArray;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    return resource_desc.SampleDesc.Count > 1 ? WMTTextureType2DMultisample
                                              : WMTTextureType2D;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    return resource_desc.SampleDesc.Count > 1 ? WMTTextureType2DMultisampleArray
                                              : WMTTextureType2DArray;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    return WMTTextureTypeCube;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return WMTTextureTypeCubeArray;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    return WMTTextureType3D;
  default:
    return resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? WMTTextureType3D
               : (resource_desc.DepthOrArraySize > 1 ? WMTTextureType2DArray
                                                     : WMTTextureType2D);
  }
}

static bool IsWritableMSAAResourceDesc(const D3D12_RESOURCE_DESC &desc) {
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         desc.SampleDesc.Count > 1 &&
         (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
         !(desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
}

static WMTTextureType
TextureTypeForUavView(const D3D12_UNORDERED_ACCESS_VIEW_DESC &desc,
                      const D3D12_RESOURCE_DESC &resource_desc) {
  switch (desc.ViewDimension) {
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    return WMTTextureType1D;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    return WMTTextureType1DArray;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    return IsWritableMSAAResourceDesc(resource_desc)
               ? WMTTextureType2DArray
               : WMTTextureType2D;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    return WMTTextureType2DArray;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    return WMTTextureType3D;
  default:
    return resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? WMTTextureType3D
               : (resource_desc.DepthOrArraySize > 1 ? WMTTextureType2DArray
                                                     : WMTTextureType2D);
  }
}

static void SrvViewRange(const D3D12_SHADER_RESOURCE_VIEW_DESC &desc,
                         const D3D12_RESOURCE_DESC &resource_desc,
                         uint16_t &mip_start, uint16_t &mip_count,
                         uint16_t &slice_start, uint16_t &slice_count) {
  uint32_t total_mips = resource_desc.MipLevels ? resource_desc.MipLevels : 1;
  uint32_t total_slices =
      resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(resource_desc.DepthOrArraySize, 1);
  mip_start = 0;
  mip_count = static_cast<uint16_t>(total_mips);
  slice_start = 0;
  slice_count = static_cast<uint16_t>(total_slices);
  switch (desc.ViewDimension) {
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    mip_start = desc.Texture1D.MostDetailedMip;
    mip_count = static_cast<uint16_t>(desc.Texture1D.MipLevels == UINT32_MAX
                                          ? total_mips - mip_start
                                          : desc.Texture1D.MipLevels);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    mip_start = desc.Texture1DArray.MostDetailedMip;
    mip_count =
        static_cast<uint16_t>(desc.Texture1DArray.MipLevels == UINT32_MAX
                                  ? total_mips - mip_start
                                  : desc.Texture1DArray.MipLevels);
    slice_start = desc.Texture1DArray.FirstArraySlice;
    slice_count = desc.Texture1DArray.ArraySize;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    mip_start = desc.Texture2D.MostDetailedMip;
    mip_count = static_cast<uint16_t>(desc.Texture2D.MipLevels == UINT32_MAX
                                          ? total_mips - mip_start
                                          : desc.Texture2D.MipLevels);
    slice_count = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    mip_start = desc.Texture2DArray.MostDetailedMip;
    mip_count =
        static_cast<uint16_t>(desc.Texture2DArray.MipLevels == UINT32_MAX
                                  ? total_mips - mip_start
                                  : desc.Texture2DArray.MipLevels);
    slice_start = desc.Texture2DArray.FirstArraySlice;
    slice_count = desc.Texture2DArray.ArraySize;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    mip_start = desc.TextureCube.MostDetailedMip;
    mip_count = static_cast<uint16_t>(desc.TextureCube.MipLevels == UINT32_MAX
                                          ? total_mips - mip_start
                                          : desc.TextureCube.MipLevels);
    slice_count = std::min<uint16_t>(6, total_slices);
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    mip_start = desc.TextureCubeArray.MostDetailedMip;
    mip_count =
        static_cast<uint16_t>(desc.TextureCubeArray.MipLevels == UINT32_MAX
                                  ? total_mips - mip_start
                                  : desc.TextureCubeArray.MipLevels);
    slice_start = desc.TextureCubeArray.First2DArrayFace;
    slice_count = desc.TextureCubeArray.NumCubes * 6;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    mip_start = desc.Texture3D.MostDetailedMip;
    mip_count = static_cast<uint16_t>(desc.Texture3D.MipLevels == UINT32_MAX
                                          ? total_mips - mip_start
                                          : desc.Texture3D.MipLevels);
    slice_count = 1;
    break;
  default:
    break;
  }
  mip_count = std::max<uint16_t>(1, mip_count);
  slice_count = std::max<uint16_t>(1, slice_count);
}

static void UavViewRange(const D3D12_UNORDERED_ACCESS_VIEW_DESC &desc,
                         const D3D12_RESOURCE_DESC &resource_desc,
                         uint16_t &mip_start, uint16_t &mip_count,
                         uint16_t &slice_start, uint16_t &slice_count) {
  uint32_t total_slices =
      resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(resource_desc.DepthOrArraySize, 1);
  mip_start = 0;
  mip_count = 1;
  slice_start = 0;
  slice_count = static_cast<uint16_t>(total_slices);
  switch (desc.ViewDimension) {
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    mip_start = desc.Texture1D.MipSlice;
    slice_count = 1;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    mip_start = desc.Texture1DArray.MipSlice;
    slice_start = desc.Texture1DArray.FirstArraySlice;
    slice_count = desc.Texture1DArray.ArraySize;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    mip_start = desc.Texture2D.MipSlice;
    slice_count = IsWritableMSAAResourceDesc(resource_desc)
                      ? std::max<UINT>(resource_desc.SampleDesc.Count, 1) *
                            std::max<UINT>(resource_desc.DepthOrArraySize, 1)
                      : 1;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    mip_start = desc.Texture2DArray.MipSlice;
    if (IsWritableMSAAResourceDesc(resource_desc)) {
      const UINT samples = std::max<UINT>(resource_desc.SampleDesc.Count, 1);
      slice_start = static_cast<uint16_t>(desc.Texture2DArray.FirstArraySlice * samples);
      slice_count = static_cast<uint16_t>(desc.Texture2DArray.ArraySize * samples);
    } else {
      slice_start = desc.Texture2DArray.FirstArraySlice;
      slice_count = desc.Texture2DArray.ArraySize;
    }
    break;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    mip_start = desc.Texture3D.MipSlice;
    slice_start = desc.Texture3D.FirstWSlice;
    slice_count = desc.Texture3D.WSize == UINT32_MAX ? 1 : desc.Texture3D.WSize;
    break;
  default:
    break;
  }
  slice_count = std::max<uint16_t>(1, slice_count);
}

static void CreateDescriptorTextureView(D3D12Descriptor *descriptor,
                                        MTLD3D12Resource *resource,
                                        DXGI_FORMAT format, WMTTextureType type,
                                        uint16_t mip_start, uint16_t mip_count,
                                        uint16_t slice_start,
                                        uint16_t slice_count) {
  if (!descriptor || !resource || resource->IsBuffer())
    return;
  D3D12_RESOURCE_DESC resource_desc = {};
  resource->GetDesc(&resource_desc);
  if (format == DXGI_FORMAT_UNKNOWN)
    format = resource_desc.Format;
  if (!resource->IsViewFormatAllowed(format)) {
    TRACE("CreateDescriptorTextureView rejected undeclared cast res=%p "
          "resource_fmt=%u view_fmt=%u",
          (void *)resource, (unsigned)resource_desc.Format, (unsigned)format);
    return;
  }
  WMTPixelFormat metal_format =
      MTLD3D12PipelineState::DXGIToMTLPixelFormat(format);
  if (metal_format == WMTPixelFormatInvalid)
    return;
  auto base = resource->GetMTLTexture();
  if (!base.handle)
    return;
  uint16_t requested_mip_start = mip_start;
  uint16_t requested_mip_count = mip_count;
  uint16_t requested_slice_start = slice_start;
  uint16_t requested_slice_count = slice_count;
  uint32_t total_mips =
      std::max<uint32_t>(1, static_cast<uint32_t>(base.mipmapLevelCount()));
  uint32_t total_slices =
      std::max<uint32_t>(1, static_cast<uint32_t>(base.arrayLength()));
  mip_start = std::min<uint16_t>(mip_start, total_mips - 1);
  mip_count = std::min<uint16_t>(std::max<uint16_t>(1, mip_count),
                                 total_mips - mip_start);
  slice_start = std::min<uint16_t>(slice_start, total_slices - 1);
  slice_count = std::min<uint16_t>(std::max<uint16_t>(1, slice_count),
                                   total_slices - slice_start);
  if (mip_start != requested_mip_start || mip_count != requested_mip_count ||
      slice_start != requested_slice_start ||
      slice_count != requested_slice_count) {
    TRACE("CreateDescriptorTextureView clamp res=%p fmt=%u type=%u "
          "mip=%u+%u->%u+%u slice=%u+%u->%u+%u metal_bounds=%ux%u",
          (void *)resource, (unsigned)format, (unsigned)type,
          requested_mip_start, requested_mip_count, mip_start, mip_count,
          requested_slice_start, requested_slice_count, slice_start,
          slice_count, total_mips, total_slices);
  }
  uint64_t gpu_id = 0;
  descriptor->metal_texture_view = base.newTextureView(
      metal_format, type, mip_start, mip_count, slice_start, slice_count,
      {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue,
       WMTTextureSwizzleAlpha},
      gpu_id);
  descriptor->metal_texture_gpu_id = gpu_id;
  TRACE("CreateDescriptorTextureView desc=%p res=%p view=%llu gpu=0x%llx "
        "fmt=%u type=%u mip=%u+%u slice=%u+%u",
        (void *)descriptor, (void *)resource,
        (unsigned long long)descriptor->metal_texture_view.handle,
        (unsigned long long)gpu_id, (unsigned)format, (unsigned)type, mip_start,
        mip_count, slice_start, slice_count);
}

static uint32_t DescriptorFormatByteSize(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return 16;
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
    return 12;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
    return 8;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
    return 4;
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
    return 2;
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
    return 1;
  default:
    return 4;
  }
}

static uint64_t DescriptorSamplerLodBiasBits(const D3D12Descriptor *descriptor) {
  uint32_t bits = 0;
  if (!descriptor)
    return 0;
  static_assert(sizeof(bits) == sizeof(descriptor->sampler.MipLODBias));
  memcpy(&bits, &descriptor->sampler.MipLODBias, sizeof(bits));
  return bits;
}

static uint64_t DescriptorBufferStride(DXGI_FORMAT format,
                                       UINT structure_byte_stride) {
  return structure_byte_stride ? structure_byte_stride
                               : DescriptorFormatByteSize(format);
}

static void UpdateDescriptorTableMirror(MTLD3D12Device *device,
                                        D3D12Descriptor *descriptor) {
  if (!device || !descriptor || !descriptor->owner)
    return;

  D3D12DescriptorTableEntry entry = {};
  if (descriptor->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ||
      descriptor->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
    entry.gpu_va = descriptor->metal_sampler_gpu_id;
    entry.metadata = DescriptorSamplerLodBiasBits(descriptor);
    descriptor->owner->UpdateShaderVisibleDescriptor(descriptor, entry);
    return;
  }

  if (descriptor->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
    entry.gpu_va = descriptor->cbv.BufferLocation;
    entry.metadata = descriptor->cbv.SizeInBytes;
    descriptor->owner->UpdateShaderVisibleDescriptor(descriptor, entry);
    return;
  }

  auto *resource = descriptor->resource
                       ? static_cast<MTLD3D12Resource *>(descriptor->resource)
                       : nullptr;
  if (!resource) {
    descriptor->owner->UpdateShaderVisibleDescriptor(descriptor, entry);
    return;
  }

  const bool is_srv =
      descriptor->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  const bool is_uav =
      descriptor->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  if (!is_srv && !is_uav) {
    descriptor->owner->UpdateShaderVisibleDescriptor(descriptor, entry);
    return;
  }

  if ((is_srv && descriptor->srv.ViewDimension ==
                         D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)) {
    entry.gpu_va = resource->GetRaytracingHeaderGPUAddress();
    if (!entry.gpu_va)
      entry.gpu_va = resource->GetGPUVirtualAddress();
  } else if (resource->IsBuffer()) {
    const DXGI_FORMAT format = is_srv ? descriptor->srv.Format
                                      : descriptor->uav.Format;
    const UINT structure_byte_stride =
        is_srv ? descriptor->srv.Buffer.StructureByteStride
               : descriptor->uav.Buffer.StructureByteStride;
    const uint64_t stride = DescriptorBufferStride(format, structure_byte_stride);
    const uint64_t first_element =
        is_srv ? descriptor->srv.Buffer.FirstElement
               : descriptor->uav.Buffer.FirstElement;
    const uint64_t num_elements =
        is_srv ? descriptor->srv.Buffer.NumElements
               : descriptor->uav.Buffer.NumElements;
    entry.gpu_va = resource->GetGPUVirtualAddress() + first_element * stride;
    entry.metadata = (num_elements * stride) & UINT64_C(0xffffffff);
    const bool raw_buffer =
        is_srv ? (descriptor->srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0
               : (descriptor->uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
    if (!structure_byte_stride && format != DXGI_FORMAT_UNKNOWN &&
        !raw_buffer)
      entry.metadata |= UINT64_C(1) << 63;
    entry.texture_view_id = descriptor->metal_texture_gpu_id;
  } else {
    entry.texture_view_id = descriptor->metal_texture_gpu_id
                                ? descriptor->metal_texture_gpu_id
                                : resource->GetTextureGPUResourceID();
  }
  descriptor->owner->UpdateShaderVisibleDescriptor(descriptor, entry);
}

} // namespace

class MTLD3D12InfoQueue : public ID3D12InfoQueue {
  struct Filter {
    std::vector<D3D12_MESSAGE_CATEGORY> allow_categories;
    std::vector<D3D12_MESSAGE_SEVERITY> allow_severities;
    std::vector<D3D12_MESSAGE_ID> allow_ids;
    std::vector<D3D12_MESSAGE_CATEGORY> deny_categories;
    std::vector<D3D12_MESSAGE_SEVERITY> deny_severities;
    std::vector<D3D12_MESSAGE_ID> deny_ids;

    static bool contains(const auto &values, const auto &value) {
      return std::find(values.begin(), values.end(), value) != values.end();
    }

    bool matches(D3D12_MESSAGE_CATEGORY category,
                 D3D12_MESSAGE_SEVERITY severity,
                 D3D12_MESSAGE_ID id) const {
      const bool allowed =
          (allow_categories.empty() || contains(allow_categories, category)) &&
          (allow_severities.empty() || contains(allow_severities, severity)) &&
          (allow_ids.empty() || contains(allow_ids, id));
      const bool denied = contains(deny_categories, category) ||
                          contains(deny_severities, severity) ||
                          contains(deny_ids, id);
      return allowed && !denied;
    }

    HRESULT append(const D3D12_INFO_QUEUE_FILTER &source) {
      auto append_values = [](auto &destination, const auto *values,
                              UINT count) -> HRESULT {
        if (count && !values)
          return E_INVALIDARG;
        try {
          if (values)
            destination.insert(destination.end(), values, values + count);
        } catch (const std::bad_alloc &) {
          return E_OUTOFMEMORY;
        }
        return S_OK;
      };

      HRESULT hr = append_values(allow_categories,
                                 source.AllowList.pCategoryList,
                                 source.AllowList.NumCategories);
      if (FAILED(hr))
        return hr;
      hr = append_values(allow_severities, source.AllowList.pSeverityList,
                         source.AllowList.NumSeverities);
      if (FAILED(hr))
        return hr;
      hr = append_values(allow_ids, source.AllowList.pIDList,
                         source.AllowList.NumIDs);
      if (FAILED(hr))
        return hr;
      hr = append_values(deny_categories, source.DenyList.pCategoryList,
                         source.DenyList.NumCategories);
      if (FAILED(hr))
        return hr;
      hr = append_values(deny_severities, source.DenyList.pSeverityList,
                         source.DenyList.NumSeverities);
      if (FAILED(hr))
        return hr;
      return append_values(deny_ids, source.DenyList.pIDList,
                           source.DenyList.NumIDs);
    }

    SIZE_T serializedSize() const {
      return sizeof(D3D12_INFO_QUEUE_FILTER) +
             allow_categories.size() * sizeof(D3D12_MESSAGE_CATEGORY) +
             allow_severities.size() * sizeof(D3D12_MESSAGE_SEVERITY) +
             allow_ids.size() * sizeof(D3D12_MESSAGE_ID) +
             deny_categories.size() * sizeof(D3D12_MESSAGE_CATEGORY) +
             deny_severities.size() * sizeof(D3D12_MESSAGE_SEVERITY) +
             deny_ids.size() * sizeof(D3D12_MESSAGE_ID);
    }

    void serialize(D3D12_INFO_QUEUE_FILTER *destination) const {
      auto *cursor = reinterpret_cast<uint8_t *>(destination) +
                     sizeof(D3D12_INFO_QUEUE_FILTER);
      auto copy = [&cursor](const auto &values, auto *&pointer, UINT &count) {
        count = static_cast<UINT>(values.size());
        if (values.empty()) {
          pointer = nullptr;
          return;
        }
        pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(cursor);
        std::memcpy(cursor, values.data(), values.size() * sizeof(values[0]));
        cursor += values.size() * sizeof(values[0]);
      };
      copy(allow_categories, destination->AllowList.pCategoryList,
           destination->AllowList.NumCategories);
      copy(allow_severities, destination->AllowList.pSeverityList,
           destination->AllowList.NumSeverities);
      copy(allow_ids, destination->AllowList.pIDList,
           destination->AllowList.NumIDs);
      copy(deny_categories, destination->DenyList.pCategoryList,
           destination->DenyList.NumCategories);
      copy(deny_severities, destination->DenyList.pSeverityList,
           destination->DenyList.NumSeverities);
      copy(deny_ids, destination->DenyList.pIDList, destination->DenyList.NumIDs);
    }
  };

  struct Message {
    D3D12_MESSAGE_CATEGORY category;
    D3D12_MESSAGE_SEVERITY severity;
    D3D12_MESSAGE_ID id;
    std::string description;
  };

  static bool validFilter(const D3D12_INFO_QUEUE_FILTER *filter) {
    if (!filter)
      return false;
    return (!filter->AllowList.NumCategories || filter->AllowList.pCategoryList) &&
           (!filter->AllowList.NumSeverities || filter->AllowList.pSeverityList) &&
           (!filter->AllowList.NumIDs || filter->AllowList.pIDList) &&
           (!filter->DenyList.NumCategories || filter->DenyList.pCategoryList) &&
           (!filter->DenyList.NumSeverities || filter->DenyList.pSeverityList) &&
           (!filter->DenyList.NumIDs || filter->DenyList.pIDList);
  }

  static SIZE_T filterSize(const Filter &filter) {
    return filter.serializedSize();
  }

  HRESULT addFilterEntries(Filter &current,
                           const D3D12_INFO_QUEUE_FILTER *filter) {
    if (!validFilter(filter))
      return E_INVALIDARG;
    Filter updated = current;
    HRESULT hr = updated.append(*filter);
    if (SUCCEEDED(hr))
      current = std::move(updated);
    return hr;
  }

  HRESULT getFilter(const Filter &filter, D3D12_INFO_QUEUE_FILTER *destination,
                    SIZE_T *length) const {
    if (!length)
      return E_INVALIDARG;
    const SIZE_T required = filterSize(filter);
    if (!destination || *length < required) {
      *length = required;
      return DXGI_ERROR_MORE_DATA;
    }
    filter.serialize(destination);
    *length = required;
    return S_OK;
  }

  template <typename Predicate>
  const Message *messageAt(UINT64 index, Predicate predicate) const {
    UINT64 current = 0;
    for (const auto &message : m_messages) {
      if (!predicate(message))
        continue;
      if (current++ == index)
        return &message;
    }
    return nullptr;
  }

public:
  virtual ~MTLD3D12InfoQueue() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12InfoQueue) {
      *ppvObject = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG rc = --m_refCount;
    if (!rc)
      delete this;
    return rc;
  }

  HRESULT STDMETHODCALLTYPE SetMessageCountLimit(UINT64 limit) override {
    std::lock_guard lock(m_mutex);
    m_messageCountLimit = limit;
    while (m_messages.size() > m_messageCountLimit) {
      m_messages.erase(m_messages.begin());
      ++m_messages_discarded_by_limit;
    }
    return S_OK;
  }

  void STDMETHODCALLTYPE ClearStoredMessages() override {
    std::lock_guard lock(m_mutex);
    m_messages.clear();
  }

  HRESULT STDMETHODCALLTYPE GetMessage(UINT64 index, D3D12_MESSAGE *message,
                                       SIZE_T *length) override {
    if (!length)
      return E_INVALIDARG;
    std::lock_guard lock(m_mutex);
    const Message *stored = messageAt(index, [this](const Message &value) {
      return m_retrieval_filter.matches(value.category, value.severity,
                                         value.id);
    });
    if (!stored) {
      *length = 0;
      return DXGI_ERROR_NOT_FOUND;
    }
    const SIZE_T required = sizeof(D3D12_MESSAGE) + stored->description.size() + 1;
    if (!message || *length < required) {
      *length = required;
      return DXGI_ERROR_MORE_DATA;
    }
    message->Category = stored->category;
    message->Severity = stored->severity;
    message->ID = stored->id;
    auto *description = reinterpret_cast<char *>(message) + sizeof(D3D12_MESSAGE);
    std::memcpy(description, stored->description.c_str(),
                stored->description.size() + 1);
    message->pDescription = description;
    message->DescriptionByteLength = stored->description.size();
    *length = required;
    return S_OK;
  }

  UINT64 STDMETHODCALLTYPE GetNumMessagesAllowedByStorageFilter() override {
    std::lock_guard lock(m_mutex);
    return m_messages_allowed_by_storage_filter;
  }
  UINT64 STDMETHODCALLTYPE GetNumMessagesDeniedByStorageFilter() override {
    std::lock_guard lock(m_mutex);
    return m_messages_denied_by_storage_filter;
  }
  UINT64 STDMETHODCALLTYPE GetNumStoredMessages() override {
    std::lock_guard lock(m_mutex);
    return m_messages.size();
  }
  UINT64 STDMETHODCALLTYPE GetNumStoredMessagesAllowedByRetrievalFilter() override {
    std::lock_guard lock(m_mutex);
    return static_cast<UINT64>(std::count_if(
        m_messages.begin(), m_messages.end(), [this](const Message &value) {
          return m_retrieval_filter.matches(value.category, value.severity,
                                             value.id);
        }));
  }
  UINT64 STDMETHODCALLTYPE GetNumMessagesDiscardedByMessageCountLimit() override {
    std::lock_guard lock(m_mutex);
    return m_messages_discarded_by_limit;
  }
  UINT64 STDMETHODCALLTYPE GetMessageCountLimit() override {
    std::lock_guard lock(m_mutex);
    return m_messageCountLimit;
  }

  HRESULT STDMETHODCALLTYPE
  AddStorageFilterEntries(D3D12_INFO_QUEUE_FILTER *filter) override {
    std::lock_guard lock(m_mutex);
    return addFilterEntries(m_storage_filter, filter);
  }
  HRESULT STDMETHODCALLTYPE GetStorageFilter(D3D12_INFO_QUEUE_FILTER *filter,
                                             SIZE_T *length) override {
    std::lock_guard lock(m_mutex);
    return getFilter(m_storage_filter, filter, length);
  }
  void STDMETHODCALLTYPE ClearStorageFilter() override {
    std::lock_guard lock(m_mutex);
    m_storage_filter = {};
  }
  HRESULT STDMETHODCALLTYPE PushEmptyStorageFilter() override {
    std::lock_guard lock(m_mutex);
    try {
      m_storage_filter_stack.push_back({});
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PushCopyOfStorageFilter() override {
    std::lock_guard lock(m_mutex);
    try {
      m_storage_filter_stack.push_back(m_storage_filter);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  PushStorageFilter(D3D12_INFO_QUEUE_FILTER *filter) override {
    std::lock_guard lock(m_mutex);
    if (!validFilter(filter))
      return E_INVALIDARG;
    Filter next;
    HRESULT hr = next.append(*filter);
    if (FAILED(hr))
      return hr;
    try {
      m_storage_filter_stack.push_back(std::move(m_storage_filter));
      m_storage_filter = std::move(next);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }
  void STDMETHODCALLTYPE PopStorageFilter() override {
    std::lock_guard lock(m_mutex);
    if (!m_storage_filter_stack.empty()) {
      m_storage_filter = std::move(m_storage_filter_stack.back());
      m_storage_filter_stack.pop_back();
    }
  }
  UINT STDMETHODCALLTYPE GetStorageFilterStackSize() override {
    std::lock_guard lock(m_mutex);
    return static_cast<UINT>(m_storage_filter_stack.size());
  }

  HRESULT STDMETHODCALLTYPE
  AddRetrievalFilterEntries(D3D12_INFO_QUEUE_FILTER *filter) override {
    std::lock_guard lock(m_mutex);
    return addFilterEntries(m_retrieval_filter, filter);
  }
  HRESULT STDMETHODCALLTYPE GetRetrievalFilter(D3D12_INFO_QUEUE_FILTER *filter,
                                               SIZE_T *length) override {
    std::lock_guard lock(m_mutex);
    return getFilter(m_retrieval_filter, filter, length);
  }
  void STDMETHODCALLTYPE ClearRetrievalFilter() override {
    std::lock_guard lock(m_mutex);
    m_retrieval_filter = {};
  }
  HRESULT STDMETHODCALLTYPE PushEmptyRetrievalFilter() override {
    std::lock_guard lock(m_mutex);
    try {
      m_retrieval_filter_stack.push_back({});
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PushCopyOfRetrievalFilter() override {
    std::lock_guard lock(m_mutex);
    try {
      m_retrieval_filter_stack.push_back(m_retrieval_filter);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  PushRetrievalFilter(D3D12_INFO_QUEUE_FILTER *filter) override {
    std::lock_guard lock(m_mutex);
    if (!validFilter(filter))
      return E_INVALIDARG;
    Filter next;
    HRESULT hr = next.append(*filter);
    if (FAILED(hr))
      return hr;
    try {
      m_retrieval_filter_stack.push_back(std::move(m_retrieval_filter));
      m_retrieval_filter = std::move(next);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }
  void STDMETHODCALLTYPE PopRetrievalFilter() override {
    std::lock_guard lock(m_mutex);
    if (!m_retrieval_filter_stack.empty()) {
      m_retrieval_filter = std::move(m_retrieval_filter_stack.back());
      m_retrieval_filter_stack.pop_back();
    }
  }
  UINT STDMETHODCALLTYPE GetRetrievalFilterStackSize() override {
    std::lock_guard lock(m_mutex);
    return static_cast<UINT>(m_retrieval_filter_stack.size());
  }

  HRESULT STDMETHODCALLTYPE AddMessage(D3D12_MESSAGE_CATEGORY category,
                                       D3D12_MESSAGE_SEVERITY severity,
                                       D3D12_MESSAGE_ID id,
                                       const char *description) override {
    if (!description)
      return E_INVALIDARG;
    std::lock_guard lock(m_mutex);
    if (!m_storage_filter.matches(category, severity, id)) {
      ++m_messages_denied_by_storage_filter;
      return S_OK;
    }
    ++m_messages_allowed_by_storage_filter;
    if (m_messages.size() >= m_messageCountLimit) {
      ++m_messages_discarded_by_limit;
      return S_OK;
    }
    try {
      m_messages.push_back({category, severity, id, description});
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE AddApplicationMessage(D3D12_MESSAGE_SEVERITY severity,
                                                  const char *description) override {
    return AddMessage(D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED, severity,
                      static_cast<D3D12_MESSAGE_ID>(0), description);
  }
  HRESULT STDMETHODCALLTYPE SetBreakOnCategory(D3D12_MESSAGE_CATEGORY category,
                                               WINBOOL enable) override {
    std::lock_guard lock(m_mutex);
    if (enable)
      m_break_categories.insert(category);
    else
      m_break_categories.erase(category);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY severity,
                                               WINBOOL enable) override {
    std::lock_guard lock(m_mutex);
    if (enable)
      m_break_severities.insert(severity);
    else
      m_break_severities.erase(severity);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetBreakOnID(D3D12_MESSAGE_ID id,
                                         WINBOOL enable) override {
    std::lock_guard lock(m_mutex);
    if (enable)
      m_break_ids.insert(id);
    else
      m_break_ids.erase(id);
    return S_OK;
  }
  WINBOOL STDMETHODCALLTYPE
  GetBreakOnCategory(D3D12_MESSAGE_CATEGORY category) override {
    std::lock_guard lock(m_mutex);
    return m_break_categories.count(category) ? TRUE : FALSE;
  }
  WINBOOL STDMETHODCALLTYPE
  GetBreakOnSeverity(D3D12_MESSAGE_SEVERITY severity) override {
    std::lock_guard lock(m_mutex);
    return m_break_severities.count(severity) ? TRUE : FALSE;
  }
  WINBOOL STDMETHODCALLTYPE GetBreakOnID(D3D12_MESSAGE_ID id) override {
    std::lock_guard lock(m_mutex);
    return m_break_ids.count(id) ? TRUE : FALSE;
  }
  void STDMETHODCALLTYPE SetMuteDebugOutput(WINBOOL mute) override {
    std::lock_guard lock(m_mutex);
    m_muteDebugOutput = mute;
  }
  WINBOOL STDMETHODCALLTYPE GetMuteDebugOutput() override {
    std::lock_guard lock(m_mutex);
    return m_muteDebugOutput;
  }

private:
  std::atomic<ULONG> m_refCount = 1;
  mutable std::mutex m_mutex;
  UINT64 m_messageCountLimit = UINT64_MAX;
  UINT64 m_messages_allowed_by_storage_filter = 0;
  UINT64 m_messages_denied_by_storage_filter = 0;
  UINT64 m_messages_discarded_by_limit = 0;
  WINBOOL m_muteDebugOutput = FALSE;
  std::vector<Message> m_messages;
  Filter m_storage_filter;
  Filter m_retrieval_filter;
  std::vector<Filter> m_storage_filter_stack;
  std::vector<Filter> m_retrieval_filter_stack;
  std::unordered_set<D3D12_MESSAGE_CATEGORY> m_break_categories;
  std::unordered_set<D3D12_MESSAGE_SEVERITY> m_break_severities;
  std::unordered_set<D3D12_MESSAGE_ID> m_break_ids;
};

struct D3D12ProgramIdentifierCompat {
  UINT64 OpaqueData[4];
};

struct ID3D12StateObjectProperties1Compat : public ID3D12StateObjectProperties {
  virtual D3D12ProgramIdentifierCompat *STDMETHODCALLTYPE GetProgramIdentifier(
      D3D12ProgramIdentifierCompat *ret, LPCWSTR program_name) = 0;
};

struct ID3D12StateObjectProperties2Compat
    : public ID3D12StateObjectProperties1Compat {
  virtual HRESULT STDMETHODCALLTYPE GetGlobalRootSignatureForProgram(
      LPCWSTR program_name, REFIID riid, void **root_signature) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGlobalRootSignatureForShader(
      LPCWSTR export_name, REFIID riid, void **root_signature) = 0;
};

Logger Logger::s_instance("d3d12.log");

static bool has_format_capability(FormatCapability capabilities,
                                  FormatCapability capability) {
  return (static_cast<int>(capabilities) & static_cast<int>(capability)) != 0;
}

static FormatCapability
query_format_capability(const FormatCapabilityInspector &inspector,
                        WMTPixelFormat format) {
  format = ORIGINAL_FORMAT(format);
  auto iter = inspector.textureCapabilities.find(format);
  if (iter == inspector.textureCapabilities.end())
    return FormatCapability::None;
  return iter->second;
}

template <typename T> static size_t pipeline_stream_payload_offset() {
  size_t offset = sizeof(UINT);
  size_t alignment = alignof(T);
  return (offset + alignment - 1) & ~(alignment - 1);
}

template <typename T> static size_t pipeline_stream_subobject_size() {
  size_t size = pipeline_stream_payload_offset<T>() + sizeof(T);
  size_t alignment = alignof(void *);
  return (size + alignment - 1) & ~(alignment - 1);
}

template <typename T>
static bool read_pipeline_stream_subobject(uint8_t *stream, uint8_t *end,
                                           T *value) {
  size_t offset = pipeline_stream_payload_offset<T>();
  if (stream + offset + sizeof(T) > end)
    return false;
  *value = *reinterpret_cast<T *>(stream + offset);
  return true;
}

template <typename T>
static bool advance_pipeline_stream(uint8_t **stream, uint8_t *end) {
  size_t size = pipeline_stream_subobject_size<T>();
  if (*stream + size > end)
    return false;
  *stream += size;
  return true;
}

struct D3D12RTFormatArray {
  DXGI_FORMAT RTFormats[8];
  UINT NumRenderTargets;
};

struct D3D12DepthStencilDesc1 {
  BOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  BOOL StencilEnable;
  UINT8 StencilReadMask;
  UINT8 StencilWriteMask;
  D3D12_DEPTH_STENCILOP_DESC FrontFace;
  D3D12_DEPTH_STENCILOP_DESC BackFace;
  BOOL DepthBoundsTestEnable;
};

struct D3D12DepthStencilOpDesc1 {
  D3D12_STENCIL_OP StencilFailOp;
  D3D12_STENCIL_OP StencilDepthFailOp;
  D3D12_STENCIL_OP StencilPassOp;
  D3D12_COMPARISON_FUNC StencilFunc;
  UINT8 StencilReadMask;
  UINT8 StencilWriteMask;
};

struct D3D12DepthStencilDesc2 {
  BOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  BOOL StencilEnable;
  D3D12DepthStencilOpDesc1 FrontFace;
  D3D12DepthStencilOpDesc1 BackFace;
  BOOL DepthBoundsTestEnable;
};

struct D3D12RasterizerDesc1 {
  D3D12_FILL_MODE FillMode;
  D3D12_CULL_MODE CullMode;
  BOOL FrontCounterClockwise;
  FLOAT DepthBias;
  FLOAT DepthBiasClamp;
  FLOAT SlopeScaledDepthBias;
  BOOL DepthClipEnable;
  BOOL MultisampleEnable;
  BOOL AntialiasedLineEnable;
  UINT ForcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};

struct D3D12RasterizerDesc2 {
  D3D12_FILL_MODE FillMode;
  D3D12_CULL_MODE CullMode;
  BOOL FrontCounterClockwise;
  FLOAT DepthBias;
  FLOAT DepthBiasClamp;
  FLOAT SlopeScaledDepthBias;
  BOOL DepthClipEnable;
  UINT LineRasterizationMode;
  UINT ForcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};

struct D3D12FeatureOptions12 {
  INT MSPrimitivesPipelineStatisticIncludesCulledPrimitives;
  BOOL EnhancedBarriersSupported;
  BOOL RelaxedFormatCastingSupported;
};

struct D3D12FeatureOptions13 {
  BOOL UnrestrictedBufferTextureCopyPitchSupported;
  BOOL UnrestrictedVertexElementAlignmentSupported;
  BOOL InvertedViewportHeightFlipsYSupported;
  BOOL InvertedViewportDepthFlipsZSupported;
  BOOL TextureCopyBetweenDimensionsSupported;
  BOOL AlphaBlendFactorSupported;
};

struct D3D12FeatureOptions14 {
  BOOL AdvancedTextureOpsSupported;
  BOOL WriteableMSAATexturesSupported;
  BOOL IndependentFrontAndBackStencilRefMaskSupported;
};

struct D3D12FeatureOptions15 {
  BOOL TriangleFanSupported;
  BOOL DynamicIndexBufferStripCutSupported;
};

struct D3D12FeatureOptions16 {
  BOOL DynamicDepthBiasSupported;
  BOOL GPUUploadHeapSupported;
};

struct D3D12FeatureOptions17 {
  BOOL NonNormalizedCoordinateSamplersSupported;
  BOOL ManualWriteTrackingResourceSupported;
};

struct D3D12FeatureOptions18 {
  BOOL RenderPassesValid;
};

struct D3D12FeatureOptions19 {
  BOOL MismatchingOutputDimensionsSupported;
  UINT SupportedSampleCountsWithNoOutputs;
  BOOL PointSamplingAddressesNeverRoundUp;
  BOOL RasterizerDesc2Supported;
  BOOL NarrowQuadrilateralLinesSupported;
  BOOL AnisoFilterWithPointMipSupported;
  UINT MaxSamplerDescriptorHeapSize;
  UINT MaxSamplerDescriptorHeapSizeWithStaticSamplers;
  UINT MaxViewDescriptorHeapSize;
  BOOL ComputeOnlyCustomHeapSupported;
};

struct D3D12FeatureOptions20 {
  BOOL ComputeOnlyWriteWatchSupported;
  UINT RecreateAtTier;
};

struct D3D12FeatureOptions21 {
  UINT WorkGraphsTier;
  UINT ExecuteIndirectTier;
  BOOL SampleCmpGradientAndBiasSupported;
  BOOL ExtendedCommandInfoSupported;
};

struct D3D12FeatureOptions22 {
  BOOL ShaderExecutionReorderingActuallyReorders;
  BOOL CreateByteOffsetViewsSupported;
  UINT Max1DDispatchSize;
  UINT Max1DDispatchMeshSize;
};

struct D3D12FeatureOptionsMlir {
  UINT MlirProgramsTier;
};

struct D3D12FeatureBoolSupport {
  BOOL Supported;
};

struct D3D12FeaturePlacedResourceSupportInfo {
  DXGI_FORMAT Format;
  D3D12_RESOURCE_DIMENSION Dimension;
  D3D12_HEAP_PROPERTIES DestHeapProperties;
  BOOL Supported;
};

struct D3D12FeatureTightAlignment {
  UINT SupportTier;
};

struct D3D12FeatureFenceBarriers {
  UINT NodeIndex;
  D3D12_COMMAND_LIST_TYPE CommandListType;
  UINT FenceBarriersTier;
};

struct D3D12FeatureHardwareSchedulingQueueGroupings {
  UINT ComputeQueuesPer3DQueue;
};

struct D3D12VersionNumber {
  UINT64 Version;
  UINT16 VersionParts[4];
};

struct D3D12FeatureShaderCacheAbiSupport {
  WCHAR AdapterFamily[128];
  UINT64 MinimumABISupportVersion;
  UINT64 MaximumABISupportVersion;
  D3D12VersionNumber CompilerVersion;
  D3D12VersionNumber ApplicationProfileVersion;
};

struct D3D12FeatureBarrierLayout {
  D3D12_COMMAND_LIST_TYPE CommandListType;
  UINT Layout;
  BOOL Supported;
};

struct D3D12FeatureMlirExchange {
  GUID MlirInterface;
  const void *InputData;
  SIZE_T InputDataSizeInBytes;
  void *OutputData;
  SIZE_T *OutputDataSizeInBytes;
};

struct D3D12FeatureMlirInterfaceSupport {
  UINT NumMlirInterfaces;
  const GUID *MlirInterfacesRequested;
  BOOL *MlirInterfacesSupported;
};

struct D3D12FeatureOptionsPreview {
  UINT MaxGroupSharedMemoryPerGroupCS;
  UINT MaxGroupSharedMemoryPerGroupAS;
  UINT MaxGroupSharedMemoryPerGroupMS;
};

struct D3D12FeatureLinearAlgebraSupport {
  UINT LinearAlgebraTier;
};

struct D3D12ViewInstanceLocation {
  UINT ViewportArrayIndex;
  UINT RenderTargetArrayIndex;
};

struct D3D12ViewInstancingDesc {
  UINT ViewInstanceCount;
  const D3D12ViewInstanceLocation *pViewInstanceLocations;
  UINT Flags;
};

static D3D12_DEPTH_STENCIL_DESC
convert_depth_stencil_desc1(const D3D12DepthStencilDesc1 &desc1) {
  D3D12_DEPTH_STENCIL_DESC desc = {};
  desc.DepthEnable = desc1.DepthEnable;
  desc.DepthWriteMask = desc1.DepthWriteMask;
  desc.DepthFunc = desc1.DepthFunc;
  desc.StencilEnable = desc1.StencilEnable;
  desc.StencilReadMask = desc1.StencilReadMask;
  desc.StencilWriteMask = desc1.StencilWriteMask;
  desc.FrontFace = desc1.FrontFace;
  desc.BackFace = desc1.BackFace;
  return desc;
}

static D3D12_DEPTH_STENCILOP_DESC
convert_depth_stencil_op_desc1(const D3D12DepthStencilOpDesc1 &desc1) {
  D3D12_DEPTH_STENCILOP_DESC desc = {};
  desc.StencilFailOp = desc1.StencilFailOp;
  desc.StencilDepthFailOp = desc1.StencilDepthFailOp;
  desc.StencilPassOp = desc1.StencilPassOp;
  desc.StencilFunc = desc1.StencilFunc;
  return desc;
}

static D3D12_DEPTH_STENCIL_DESC
convert_depth_stencil_desc2(const D3D12DepthStencilDesc2 &desc2) {
  D3D12_DEPTH_STENCIL_DESC desc = {};
  desc.DepthEnable = desc2.DepthEnable;
  desc.DepthWriteMask = desc2.DepthWriteMask;
  desc.DepthFunc = desc2.DepthFunc;
  desc.StencilEnable = desc2.StencilEnable;
  desc.StencilReadMask = desc2.FrontFace.StencilReadMask;
  desc.StencilWriteMask = desc2.FrontFace.StencilWriteMask;
  desc.FrontFace = convert_depth_stencil_op_desc1(desc2.FrontFace);
  desc.BackFace = convert_depth_stencil_op_desc1(desc2.BackFace);
  return desc;
}

static D3D12_RASTERIZER_DESC
convert_rasterizer_desc1(const D3D12RasterizerDesc1 &desc1) {
  D3D12_RASTERIZER_DESC desc = {};
  desc.FillMode = desc1.FillMode;
  desc.CullMode = desc1.CullMode;
  desc.FrontCounterClockwise = desc1.FrontCounterClockwise;
  desc.DepthBias = (INT)desc1.DepthBias;
  desc.DepthBiasClamp = desc1.DepthBiasClamp;
  desc.SlopeScaledDepthBias = desc1.SlopeScaledDepthBias;
  desc.DepthClipEnable = desc1.DepthClipEnable;
  desc.MultisampleEnable = desc1.MultisampleEnable;
  desc.AntialiasedLineEnable = desc1.AntialiasedLineEnable;
  desc.ForcedSampleCount = desc1.ForcedSampleCount;
  desc.ConservativeRaster = desc1.ConservativeRaster;
  return desc;
}

static D3D12_RASTERIZER_DESC
convert_rasterizer_desc2(const D3D12RasterizerDesc2 &desc2) {
  D3D12_RASTERIZER_DESC desc = {};
  desc.FillMode = desc2.FillMode;
  desc.CullMode = desc2.CullMode;
  desc.FrontCounterClockwise = desc2.FrontCounterClockwise;
  desc.DepthBias = (INT)desc2.DepthBias;
  desc.DepthBiasClamp = desc2.DepthBiasClamp;
  desc.SlopeScaledDepthBias = desc2.SlopeScaledDepthBias;
  desc.DepthClipEnable = desc2.DepthClipEnable;
  desc.MultisampleEnable = FALSE;
  desc.AntialiasedLineEnable = desc2.LineRasterizationMode == 1;
  desc.ForcedSampleCount = desc2.ForcedSampleCount;
  desc.ConservativeRaster = desc2.ConservativeRaster;
  return desc;
}

class MTLD3D12CommandSignature : public ComObject<ID3D12CommandSignature> {
public:
  MTLD3D12CommandSignature(MTLD3D12Device *device,
                           const D3D12_COMMAND_SIGNATURE_DESC &desc)
      : m_device(device), m_desc(desc) {
    if (desc.pArgumentDescs && desc.NumArgumentDescs) {
      m_argument_descs.assign(desc.pArgumentDescs,
                              desc.pArgumentDescs + desc.NumArgumentDescs);
      m_desc.pArgumentDescs = m_argument_descs.data();
    }
    m_device->AddRef();
  }
  ~MTLD3D12CommandSignature() { m_device->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12CommandSignature) {
      *ppv = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size,
                                           void *data) override {
    return m_private_data.getData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size,
                                           const void *data) override {
    return m_private_data.setData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
      REFGUID guid, const IUnknown *data) override {
    return m_private_data.setInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
    return m_private_data.setName(name);
  }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return m_device->QueryInterface(riid, device);
  }
  const D3D12_COMMAND_SIGNATURE_DESC &GetDesc() const { return m_desc; }

private:
  MTLD3D12Device *m_device;
  ComPrivateData m_private_data;
  D3D12_COMMAND_SIGNATURE_DESC m_desc;
  std::vector<D3D12_INDIRECT_ARGUMENT_DESC> m_argument_descs;
};

struct ID3D12PipelineLibraryCompat : public ID3D12DeviceChild {
  virtual HRESULT STDMETHODCALLTYPE
  StorePipeline(LPCWSTR name, ID3D12PipelineState *pipeline) = 0;
  virtual HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(
      LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state) = 0;
  virtual HRESULT STDMETHODCALLTYPE LoadComputePipeline(
      LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state) = 0;
  virtual SIZE_T STDMETHODCALLTYPE GetSerializedSize() = 0;
  virtual HRESULT STDMETHODCALLTYPE Serialize(void *data, SIZE_T data_size) = 0;
};

struct ID3D12PipelineLibrary1Compat : public ID3D12PipelineLibraryCompat {
  virtual HRESULT STDMETHODCALLTYPE
  LoadPipeline(LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
               REFIID riid, void **pipeline_state) = 0;
};

class MTLD3D12PipelineLibrary : public ComObject<ID3D12PipelineLibrary1Compat> {
public:
  MTLD3D12PipelineLibrary(MTLD3D12Device *device, const void *blob,
                          SIZE_T blob_size)
      : m_device(device) {
    m_device->AddRef();
    PLTRACE("PipelineLibrary create blob=%p size=%zu", blob, blob_size);
  }

  ~MTLD3D12PipelineLibrary() {
    for (auto &entry : m_entries) {
      if (entry.second)
        entry.second->Release();
    }
    m_device->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12PipelineLibrary_ ||
        riid == IID_ID3D12PipelineLibrary1_) {
      *ppv = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size,
                                           void *data) override {
    return m_private_data.getData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size,
                                           const void *data) override {
    return m_private_data.setData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
      REFGUID guid, const IUnknown *data) override {
    return m_private_data.setInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
    return m_private_data.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return m_device->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE
  StorePipeline(LPCWSTR name, ID3D12PipelineState *pipeline) override {
    if (!pipeline)
      return E_POINTER;
    auto key = key_from_name(name);
    auto iter = m_entries.find(key);
    if (iter != m_entries.end() && iter->second)
      iter->second->Release();
    pipeline->AddRef();
    m_entries[key] = pipeline;
    PLTRACE("PipelineLibrary StorePipeline name=%ls pipeline=%p entries=%zu",
            name ? name : L"(null)", pipeline, m_entries.size());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(
      LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state) override {
    if (!pipeline_state)
      return E_POINTER;
    *pipeline_state = nullptr;
    if (auto *pipeline = lookup(name)) {
      PLTRACE("PipelineLibrary LoadGraphicsPipeline cache hit name=%ls",
              name ? name : L"(null)");
      return pipeline->QueryInterface(riid, pipeline_state);
    }
    PLTRACE("PipelineLibrary LoadGraphicsPipeline miss name=%ls -> create",
            name ? name : L"(null)");
    return m_device->CreateGraphicsPipelineState(desc, riid, pipeline_state);
  }

  HRESULT STDMETHODCALLTYPE LoadComputePipeline(
      LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state) override {
    if (!pipeline_state)
      return E_POINTER;
    *pipeline_state = nullptr;
    if (auto *pipeline = lookup(name)) {
      PLTRACE("PipelineLibrary LoadComputePipeline cache hit name=%ls",
              name ? name : L"(null)");
      return pipeline->QueryInterface(riid, pipeline_state);
    }
    PLTRACE("PipelineLibrary LoadComputePipeline miss name=%ls -> create",
            name ? name : L"(null)");
    return m_device->CreateComputePipelineState(desc, riid, pipeline_state);
  }

  SIZE_T STDMETHODCALLTYPE GetSerializedSize() override {
    return sizeof(uint32_t) * 2;
  }

  HRESULT STDMETHODCALLTYPE Serialize(void *data, SIZE_T data_size) override {
    if (!data)
      return E_POINTER;
    if (data_size < GetSerializedSize())
      return E_INVALIDARG;
    uint32_t *words = reinterpret_cast<uint32_t *>(data);
    words[0] = 0x4c505844; // DXPL
    words[1] = static_cast<uint32_t>(m_entries.size());
    PLTRACE("PipelineLibrary Serialize entries=%zu bytes=%zu", m_entries.size(),
            data_size);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  LoadPipeline(LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
               REFIID riid, void **pipeline_state) override {
    if (!pipeline_state)
      return E_POINTER;
    *pipeline_state = nullptr;
    if (auto *pipeline = lookup(name)) {
      PLTRACE("PipelineLibrary LoadPipeline cache hit name=%ls",
              name ? name : L"(null)");
      return pipeline->QueryInterface(riid, pipeline_state);
    }
    PLTRACE("PipelineLibrary LoadPipeline miss name=%ls -> create",
            name ? name : L"(null)");
    return m_device->CreatePipelineState(desc, riid, pipeline_state);
  }

private:
  static std::wstring key_from_name(LPCWSTR name) {
    return name ? std::wstring(name) : std::wstring();
  }

  ID3D12PipelineState *lookup(LPCWSTR name) {
    auto iter = m_entries.find(key_from_name(name));
    return iter == m_entries.end() ? nullptr : iter->second;
  }

  MTLD3D12Device *m_device;
  ComPrivateData m_private_data;
  std::unordered_map<std::wstring, ID3D12PipelineState *> m_entries;
};

class MTLD3D12StateObject : public ID3D12StateObject,
                            public ID3D12StateObjectProperties2Compat {
public:
  MTLD3D12StateObject(MTLD3D12Device *device,
                      const D3D12_STATE_OBJECT_DESC *desc,
                      ID3D12StateObject *base = nullptr)
      : m_device(device) {
    m_device->AddRef();
    if (base) {
      base->AddRef();
      m_base = base;
      auto *source = static_cast<MTLD3D12StateObject *>(base);
      m_type = source->m_type;
      m_subobject_types = source->m_subobject_types;
      m_exports = source->m_exports;
      m_export_imports = source->m_export_imports;
      m_shader_identifiers = source->m_shader_identifiers;
      m_shader_stack_sizes = source->m_shader_stack_sizes;
      m_pipeline_stack_size = source->m_pipeline_stack_size;
      m_max_trace_recursion_depth = source->m_max_trace_recursion_depth;
      m_raygen_compute_pipeline = source->m_raygen_compute_pipeline;
      m_raygen_visible_function_table =
          source->m_raygen_visible_function_table;
      m_intersection_function_table = source->m_intersection_function_table;
      for (const auto &entry : source->m_local_root_signatures) {
        if (entry.second)
          entry.second->AddRef();
        m_local_root_signatures.emplace(entry.first, entry.second);
      }
      m_global_root_signature = source->m_global_root_signature;
      if (m_global_root_signature)
        m_global_root_signature->AddRef();
    }
    if (desc && !base) {
      m_type = desc->Type;
      m_subobject_types.reserve(desc->NumSubobjects);
      for (UINT i = 0; i < desc->NumSubobjects; i++) {
        m_subobject_types.push_back(desc->pSubobjects[i].Type);
      }
    }
    TRACE("StateObject create type=%u subobjects=%zu base=%p", (unsigned)m_type,
          m_subobject_types.size(), base);
  }

  virtual ~MTLD3D12StateObject() {
    if (m_global_root_signature)
      m_global_root_signature->Release();
    for (const auto &entry : m_local_root_signatures) {
      if (entry.second)
        entry.second->Release();
    }
    for (auto *collection : m_existing_collections) {
      if (collection)
        collection->Release();
    }
    if (m_base)
      m_base->Release();
    m_device->Release();
  }

  bool Initialize(const D3D12_STATE_OBJECT_DESC *desc) {
    if (!desc ||
        (desc->Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE &&
         desc->Type != D3D12_STATE_OBJECT_TYPE_COLLECTION))
      return false;

    D3D12_SHADER_BYTECODE raytracing_library = {};
    bool collection_relink_required = false;
    std::vector<const D3D12_EXISTING_COLLECTION_DESC *>
        existing_collections;
    std::unordered_map<const D3D12_STATE_SUBOBJECT *, ID3D12RootSignature *>
        local_root_subobjects;
    std::vector<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>
        local_root_associations;
    for (UINT i = 0; i < desc->NumSubobjects; i++) {
      const auto &subobject = desc->pSubobjects[i];
      if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY &&
          subobject.pDesc) {
        const auto *library =
            static_cast<const D3D12_DXIL_LIBRARY_DESC *>(subobject.pDesc);
        if (!library->DXILLibrary.pShaderBytecode ||
            !library->DXILLibrary.BytecodeLength)
          return false;
        raytracing_library = library->DXILLibrary;
        for (UINT e = 0; e < library->NumExports; e++) {
          if (library->pExports[e].Name) {
            m_exports.emplace_back(library->pExports[e].Name);
            m_export_imports[library->pExports[e].Name] =
                library->pExports[e].ExportToRename
                    ? library->pExports[e].ExportToRename
                    : library->pExports[e].Name;
          }
        }
      } else if (subobject.Type ==
                     D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE &&
                 subobject.pDesc) {
        const auto *root = static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(
            subobject.pDesc);
        if (root->pGlobalRootSignature) {
          if (m_global_root_signature)
            m_global_root_signature->Release();
          m_global_root_signature = root->pGlobalRootSignature;
          m_global_root_signature->AddRef();
        }
      } else if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP &&
                 subobject.pDesc) {
        const auto *hit_group =
            static_cast<const D3D12_HIT_GROUP_DESC *>(subobject.pDesc);
        if (hit_group->HitGroupExport) {
          m_exports.emplace_back(hit_group->HitGroupExport);
          collection_relink_required = true;
        }
      } else if (subobject.Type ==
                     D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION &&
                 subobject.pDesc) {
        const auto *collection =
            static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(
                subobject.pDesc);
        if (!collection->pExistingCollection ||
            (collection->NumExports && !collection->pExports))
          return false;
        existing_collections.push_back(collection);
      } else if (subobject.Type ==
                     D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE &&
                 subobject.pDesc) {
        const auto *local_root =
            static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>(subobject.pDesc);
        if (!local_root->pLocalRootSignature)
          return false;
        local_root_subobjects.emplace(&subobject,
                                      local_root->pLocalRootSignature);
      } else if (subobject.Type ==
                     D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION &&
                 subobject.pDesc) {
        local_root_associations.push_back(
            static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(
                subobject.pDesc));
      } else if (subobject.Type ==
                     D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG &&
                 subobject.pDesc) {
        const auto *config =
            static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG *>(
                subobject.pDesc);
        m_max_trace_recursion_depth = config->MaxTraceRecursionDepth;
      }
    }
    for (const auto *association : local_root_associations) {
      if (!association || !association->pSubobjectToAssociate ||
          !association->NumExports || !association->pExports)
        return false;
      auto local_root =
          local_root_subobjects.find(association->pSubobjectToAssociate);
      // SUBOBJECT_TO_EXPORTS_ASSOCIATION is also used for shader and pipeline
      // configuration. Only associations whose target is a local-root
      // subobject contribute local-root record metadata here.
      if (local_root == local_root_subobjects.end())
        continue;
      for (UINT e = 0; e < association->NumExports; e++) {
        const WCHAR *export_name = association->pExports[e];
        if (!export_name || m_local_root_signatures.count(export_name))
          return false;
        local_root->second->AddRef();
        m_local_root_signatures.emplace(export_name, local_root->second);
      }
    }
    if (!existing_collections.empty() &&
        !raytracing_library.pShaderBytecode) {
      if (collection_relink_required)
        return false;
      auto import_export = [this](MTLD3D12StateObject *source,
                                  const wchar_t *name,
                                  const wchar_t *source_name) {
        if (!name || !source_name || m_shader_identifiers.count(name))
          return false;
        auto identifier = source->m_shader_identifiers.find(source_name);
        if (identifier == source->m_shader_identifiers.end())
          return false;
        std::array<uint8_t, 32> imported_identifier = identifier->second;
        if (wcscmp(name, source_name) != 0) {
          uint64_t export_hash = 1469598103934665603ull;
          for (const WCHAR *p = name; *p; p++) {
            export_hash ^= static_cast<uint16_t>(*p);
            export_hash *= 1099511628211ull;
          }
          memcpy(imported_identifier.data() + 24, &export_hash,
                 sizeof(export_hash));
        }
        std::wstring canonical_name(source_name);
        auto canonical = source->m_export_imports.find(source_name);
        if (canonical != source->m_export_imports.end())
          canonical_name = canonical->second;
        auto local_root = source->m_local_root_signatures.find(source_name);
        if (local_root != source->m_local_root_signatures.end() &&
            local_root->second) {
          local_root->second->AddRef();
          m_local_root_signatures[name] = local_root->second;
        }
        m_exports.emplace_back(name);
        m_export_imports[name] = canonical_name;
        m_shader_identifiers[name] = imported_identifier;
        return true;
      };

      for (const auto *collection : existing_collections) {
        auto *source = static_cast<MTLD3D12StateObject *>(
            collection->pExistingCollection);
        if (!source->m_raygen_compute_pipeline.handle ||
            !source->m_raygen_visible_function_table.handle)
          return false;
        if (!m_raygen_compute_pipeline.handle) {
          m_raygen_compute_pipeline = source->m_raygen_compute_pipeline;
          m_raygen_visible_function_table =
              source->m_raygen_visible_function_table;
          m_intersection_function_table =
              source->m_intersection_function_table;
        } else if (m_raygen_compute_pipeline.handle !=
                       source->m_raygen_compute_pipeline.handle ||
                   m_raygen_visible_function_table.handle !=
                       source->m_raygen_visible_function_table.handle ||
                   m_intersection_function_table.handle !=
                       source->m_intersection_function_table.handle) {
          // Merging independently linked Metal function tables requires a
          // relink. Collections derived from the same executable table can be
          // merged without changing any shader identifiers or function slots.
          return false;
        }
        if (m_global_root_signature && source->m_global_root_signature &&
            m_global_root_signature != source->m_global_root_signature)
          return false;
        if (!m_global_root_signature && source->m_global_root_signature) {
          m_global_root_signature = source->m_global_root_signature;
          m_global_root_signature->AddRef();
        }

        if (collection->NumExports) {
          for (UINT e = 0; e < collection->NumExports; e++) {
            const auto &export_desc = collection->pExports[e];
            if (export_desc.Flags != D3D12_EXPORT_FLAG_NONE ||
                !import_export(source, export_desc.Name,
                               export_desc.ExportToRename
                                   ? export_desc.ExportToRename
                                   : export_desc.Name))
              return false;
          }
        } else {
          for (const auto &export_name : source->m_exports) {
            if (!import_export(source, export_name.c_str(),
                               export_name.c_str()))
              return false;
          }
        }
        m_pipeline_stack_size =
            std::max(m_pipeline_stack_size, source->m_pipeline_stack_size);
        collection->pExistingCollection->AddRef();
        m_existing_collections.push_back(collection->pExistingCollection);
      }
      TRACE("StateObject merged collections=%zu exports=%zu pso=%llu",
            existing_collections.size(), m_exports.size(),
            (unsigned long long)m_raygen_compute_pipeline.handle);
      RebuildShaderStackSizes();
      return !m_exports.empty();
    }
    if (!existing_collections.empty())
      return false;
    if (!raytracing_library.pShaderBytecode ||
        !raytracing_library.BytecodeLength)
      return false;
    if (m_exports.empty())
      m_exports.emplace_back(L"raygen");
    const bool has_miss_shader =
        std::find(m_exports.begin(), m_exports.end(), L"miss_shader") !=
        m_exports.end();
    const bool has_closest_hit_shader =
        std::find(m_exports.begin(), m_exports.end(), L"closest_hit") !=
            m_exports.end() &&
        std::find(m_exports.begin(), m_exports.end(), L"hit_group") !=
            m_exports.end();
    const bool has_callable_shader =
        std::find(m_exports.begin(), m_exports.end(), L"callable_shader") !=
        m_exports.end();
    const bool has_any_hit_shader =
        std::find(m_exports.begin(), m_exports.end(), L"any_hit") !=
            m_exports.end() &&
        has_closest_hit_shader;
    const bool has_procedural_hit_group =
        std::find(m_exports.begin(), m_exports.end(),
                  L"procedural_intersection") != m_exports.end() &&
        std::find(m_exports.begin(), m_exports.end(),
                  L"procedural_closest_hit") != m_exports.end() &&
        std::find(m_exports.begin(), m_exports.end(),
                  L"procedural_hit_group") != m_exports.end();

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
    compute_desc.pRootSignature = m_global_root_signature;
    compute_desc.CS = raytracing_library;
    auto *pipeline = new MTLD3D12PipelineState(m_device, true);
    pipeline->SetComputeDesc(compute_desc);
    std::string cache_hash = pipeline->GetCSCacheHash();
    const char *cache_env = std::getenv("DXMT_SHADER_CACHE_PATH");
    std::string cache_dir =
        cache_env && cache_env[0] ? cache_env : "/tmp/dxmt_shader_cache";
    while (cache_dir.size() > 1 &&
           (cache_dir.back() == '/' || cache_dir.back() == '\\'))
      cache_dir.pop_back();
    std::string raygen_path = cache_dir + "/" + cache_hash + ".metallib";
    std::string dispatch_path =
        cache_dir + "/" + cache_hash + ".raydispatch.metallib";
    std::string miss_path = cache_dir + "/" + cache_hash + ".miss.metallib";
    std::string closest_hit_path =
        cache_dir + "/" + cache_hash + ".closesthit.metallib";
    std::string callable_path =
        cache_dir + "/" + cache_hash + ".callable.metallib";
    std::string any_hit_path =
        cache_dir + "/" + cache_hash + ".anyhit.metallib";
    std::string intersection_path =
        cache_dir + "/" + cache_hash + ".rayintersection.metallib";
    std::string procedural_intersection_path =
        cache_dir + "/" + cache_hash + ".proceduralintersection.metallib";
    std::string procedural_closest_hit_path =
        cache_dir + "/" + cache_hash + ".proceduralclosesthit.metallib";
    std::string procedural_wrapper_path =
        cache_dir + "/" + cache_hash + ".proceduralwrapper.metallib";

    auto read_file = [](const std::string &path, std::vector<uint8_t> &data) {
      FILE *file = fopen(path.c_str(), "rb");
      if (!file)
        return false;
      fseek(file, 0, SEEK_END);
      long size = ftell(file);
      fseek(file, 0, SEEK_SET);
      if (size <= 0) {
        fclose(file);
        return false;
      }
      data.resize(static_cast<size_t>(size));
      bool ok = fread(data.data(), 1, data.size(), file) == data.size();
      fclose(file);
      return ok;
    };
    std::vector<uint8_t> raygen_data;
    std::vector<uint8_t> dispatch_data;
    std::vector<uint8_t> miss_data;
    std::vector<uint8_t> closest_hit_data;
    std::vector<uint8_t> callable_data;
    std::vector<uint8_t> any_hit_data;
    std::vector<uint8_t> intersection_data;
    std::vector<uint8_t> procedural_intersection_data;
    std::vector<uint8_t> procedural_closest_hit_data;
    std::vector<uint8_t> procedural_wrapper_data;
    if (!read_file(raygen_path, raygen_data) ||
        !read_file(dispatch_path, dispatch_data) ||
        (has_miss_shader && !read_file(miss_path, miss_data)) ||
        (has_closest_hit_shader &&
         !read_file(closest_hit_path, closest_hit_data)) ||
        (has_callable_shader && !read_file(callable_path, callable_data)) ||
        (has_any_hit_shader && !read_file(any_hit_path, any_hit_data)) ||
        (has_any_hit_shader &&
         !read_file(intersection_path, intersection_data)) ||
        (has_procedural_hit_group &&
         (!read_file(procedural_intersection_path,
                     procedural_intersection_data) ||
          !read_file(procedural_closest_hit_path,
                     procedural_closest_hit_data) ||
          !read_file(procedural_wrapper_path, procedural_wrapper_data)))) {
      pipeline->RequestCompile(false);
      TRACE("StateObject raygen cache miss visible=%s dispatch=%s miss=%s",
            raygen_path.c_str(), dispatch_path.c_str(), miss_path.c_str());
      pipeline->Release();
      return false;
    }
    pipeline->Release();

    auto metal_device = m_device->GetMTLDevice();
    WMT::Reference<WMT::Error> error;
    auto raygen_library_handle = metal_device.newLibrary(
        raygen_data.data(), raygen_data.size(), error);
    if (!raygen_library_handle.handle || error.handle)
      return false;
    error = nullptr;
    auto dispatch_library_handle = metal_device.newLibrary(
        dispatch_data.data(), dispatch_data.size(), error);
    if (!dispatch_library_handle.handle || error.handle)
      return false;
    WMT::Reference<WMT::Library> miss_library_handle;
    WMT::Reference<WMT::Function> miss_function;
    if (has_miss_shader) {
      error = nullptr;
      miss_library_handle = metal_device.newLibrary(
          miss_data.data(), miss_data.size(), error);
      if (!miss_library_handle.handle || error.handle)
        return false;
      miss_function = miss_library_handle.newFunction("miss_shader");
      if (!miss_function.handle)
        return false;
    }
    WMT::Reference<WMT::Library> closest_hit_library_handle;
    WMT::Reference<WMT::Function> closest_hit_function;
    if (has_closest_hit_shader) {
      error = nullptr;
      closest_hit_library_handle = metal_device.newLibrary(
          closest_hit_data.data(), closest_hit_data.size(), error);
      if (!closest_hit_library_handle.handle || error.handle)
        return false;
      closest_hit_function =
          closest_hit_library_handle.newFunction("closest_hit");
      if (!closest_hit_function.handle)
        return false;
    }
    WMT::Reference<WMT::Library> callable_library_handle;
    WMT::Reference<WMT::Function> callable_function;
    if (has_callable_shader) {
      error = nullptr;
      callable_library_handle = metal_device.newLibrary(
          callable_data.data(), callable_data.size(), error);
      if (!callable_library_handle.handle || error.handle)
        return false;
      callable_function =
          callable_library_handle.newFunction("callable_shader");
      if (!callable_function.handle)
        return false;
    }
    WMT::Reference<WMT::Library> any_hit_library_handle;
    WMT::Reference<WMT::Function> any_hit_function;
    if (has_any_hit_shader) {
      error = nullptr;
      any_hit_library_handle = metal_device.newLibrary(
          any_hit_data.data(), any_hit_data.size(), error);
      if (!any_hit_library_handle.handle || error.handle)
        return false;
      any_hit_function = any_hit_library_handle.newFunction("any_hit");
      if (!any_hit_function.handle)
        return false;
    }
    WMT::Reference<WMT::Library> intersection_library_handle;
    WMT::Reference<WMT::Function> intersection_function;
    if (has_any_hit_shader) {
      error = nullptr;
      intersection_library_handle = metal_device.newLibrary(
          intersection_data.data(), intersection_data.size(), error);
      if (!intersection_library_handle.handle || error.handle)
        return false;
      intersection_function = intersection_library_handle.newFunction(
          "irconverter.wrapper.intersection.function.triangle");
      if (!intersection_function.handle)
        return false;
    }
    WMT::Reference<WMT::Library> procedural_intersection_library_handle;
    WMT::Reference<WMT::Library> procedural_closest_hit_library_handle;
    WMT::Reference<WMT::Library> procedural_wrapper_library_handle;
    WMT::Reference<WMT::Function> procedural_intersection_function;
    WMT::Reference<WMT::Function> procedural_closest_hit_function;
    WMT::Reference<WMT::Function> procedural_wrapper_function;
    if (has_procedural_hit_group) {
      error = nullptr;
      procedural_intersection_library_handle = metal_device.newLibrary(
          procedural_intersection_data.data(),
          procedural_intersection_data.size(), error);
      if (!procedural_intersection_library_handle.handle || error.handle)
        return false;
      procedural_intersection_function =
          procedural_intersection_library_handle.newFunction(
              "procedural_intersection");
      error = nullptr;
      procedural_closest_hit_library_handle = metal_device.newLibrary(
          procedural_closest_hit_data.data(),
          procedural_closest_hit_data.size(), error);
      if (!procedural_closest_hit_library_handle.handle || error.handle)
        return false;
      procedural_closest_hit_function =
          procedural_closest_hit_library_handle.newFunction(
              "procedural_closest_hit");
      error = nullptr;
      procedural_wrapper_library_handle = metal_device.newLibrary(
          procedural_wrapper_data.data(), procedural_wrapper_data.size(),
          error);
      if (!procedural_wrapper_library_handle.handle || error.handle)
        return false;
      procedural_wrapper_function =
          procedural_wrapper_library_handle.newFunction(
              "irconverter.wrapper.intersection.function.procedural");
      if (!procedural_intersection_function.handle ||
          !procedural_closest_hit_function.handle ||
          !procedural_wrapper_function.handle)
        return false;
    }
    auto raygen_function = raygen_library_handle.newFunction("raygen");
    auto dispatch_function =
        dispatch_library_handle.newFunction("RaygenIndirection");
    if (!raygen_function.handle || !dispatch_function.handle)
      return false;
    WMTRaytracingComputePipelineInfo pipeline_info = {};
    pipeline_info.dispatch_function = dispatch_function.handle;
    pipeline_info.raygen_function = raygen_function.handle;
    pipeline_info.miss_function = miss_function.handle;
    pipeline_info.closest_hit_function = closest_hit_function.handle;
    pipeline_info.callable_function = callable_function.handle;
    pipeline_info.any_hit_function = any_hit_function.handle;
    pipeline_info.intersection_function = intersection_function.handle;
    pipeline_info.procedural_intersection_function =
        procedural_intersection_function.handle;
    pipeline_info.procedural_closest_hit_function =
        procedural_closest_hit_function.handle;
    pipeline_info.procedural_wrapper_function =
        procedural_wrapper_function.handle;
    error = nullptr;
    m_raygen_compute_pipeline = metal_device.newRaytracingComputePipelineState(
        pipeline_info, m_raygen_visible_function_table,
        m_intersection_function_table, error);
    if (!m_raygen_compute_pipeline.handle ||
        !m_raygen_visible_function_table.handle || error.handle)
      return false;

    for (const auto &export_name : m_exports) {
      std::array<uint8_t, 32> identifier = {};
      auto import = m_export_imports.find(export_name);
      const std::wstring &canonical_name =
          import == m_export_imports.end() ? export_name : import->second;
      const uint64_t visible_function_index =
          canonical_name == L"raygen"       ? 1ull
          : canonical_name == L"miss_shader" ? 2ull
          : canonical_name == L"hit_group" ||
                    canonical_name == L"closest_hit"
              ? 3ull
          : canonical_name == L"callable_shader" ? 4ull
          : canonical_name == L"any_hit" ? 5ull
          : canonical_name == L"procedural_intersection"
              ? 6ull
          : canonical_name == L"procedural_closest_hit" ||
                    canonical_name == L"procedural_hit_group"
              ? 7ull
              : 0ull;
      const uint64_t intersection_function_index =
          canonical_name == L"hit_group" && has_any_hit_shader
              ? 5ull
          : canonical_name == L"procedural_hit_group" &&
                    has_procedural_hit_group
              ? 6ull
              : 0ull;
      memcpy(identifier.data(), &intersection_function_index,
             sizeof(intersection_function_index));
      memcpy(identifier.data() + sizeof(uint64_t),
             &visible_function_index, sizeof(visible_function_index));
      uint64_t export_hash = 1469598103934665603ull;
      for (WCHAR c : export_name) {
        export_hash ^= static_cast<uint16_t>(c);
        export_hash *= 1099511628211ull;
      }
      memcpy(identifier.data() + 24, &export_hash, sizeof(export_hash));
      m_shader_identifiers.emplace(export_name, identifier);
    }
    RebuildShaderStackSizes();
    TRACE("StateObject raygen pipeline compiled exports=%zu pso=%llu "
          "visible_table=%llu",
          m_exports.size(),
          (unsigned long long)m_raygen_compute_pipeline.handle,
          (unsigned long long)m_raygen_visible_function_table.handle);
    return true;
  }

  bool InitializeAddition(const D3D12_STATE_OBJECT_DESC *desc) {
    if (!desc || !m_base ||
        desc->Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE ||
        !m_raygen_compute_pipeline.handle ||
        !m_raygen_visible_function_table.handle)
      return false;
    for (UINT i = 0; i < desc->NumSubobjects; i++) {
      const auto &subobject = desc->pSubobjects[i];
      m_subobject_types.push_back(subobject.Type);
      if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP &&
          subobject.pDesc) {
        const auto *hit_group =
            static_cast<const D3D12_HIT_GROUP_DESC *>(subobject.pDesc);
        if (!hit_group->HitGroupExport)
          return false;
        const wchar_t *template_name =
            hit_group->Type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE
                ? L"procedural_hit_group"
                : L"hit_group";
        auto template_identifier = m_shader_identifiers.find(template_name);
        if (template_identifier == m_shader_identifiers.end())
          return false;
        std::array<uint8_t, 32> identifier = template_identifier->second;
        uint64_t export_hash = 1469598103934665603ull;
        for (const WCHAR *p = hit_group->HitGroupExport; *p; p++) {
          export_hash ^= static_cast<uint16_t>(*p);
          export_hash *= 1099511628211ull;
        }
        memcpy(identifier.data() + 24, &export_hash, sizeof(export_hash));
        m_exports.emplace_back(hit_group->HitGroupExport);
        m_shader_identifiers[hit_group->HitGroupExport] = identifier;
        m_export_imports[hit_group->HitGroupExport] = template_name;
        RebuildShaderStackSizes();
        auto local_root = m_local_root_signatures.find(template_name);
        if (local_root != m_local_root_signatures.end() &&
            local_root->second) {
          local_root->second->AddRef();
          m_local_root_signatures[hit_group->HitGroupExport] =
              local_root->second;
        }
      } else if (subobject.Type ==
                     D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE &&
                 subobject.pDesc) {
        const auto *root = static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(
            subobject.pDesc);
        if (root->pGlobalRootSignature != m_global_root_signature)
          return false;
      } else if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
        // New Metal functions require relinking the visible-function tables;
        // alias growth only accepts hit groups assembled from base exports.
        return false;
      } else if (subobject.Type !=
                     D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG &&
                 subobject.Type !=
                     D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG &&
                 subobject.Type !=
                     D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG) {
        return false;
      }
    }
    TRACE("StateObject addition initialized base=%p exports=%zu subobjects=%zu",
          (void *)m_base, m_exports.size(), m_subobject_types.size());
    return true;
  }

  WMT::Reference<WMT::ComputePipelineState> RaygenComputePipeline() const {
    return m_raygen_compute_pipeline;
  }

  WMT::Reference<WMT::VisibleFunctionTable> RaygenVisibleFunctionTable() const {
    return m_raygen_visible_function_table;
  }

  WMT::Reference<WMT::IntersectionFunctionTable>
  IntersectionFunctionTable() const {
    return m_intersection_function_table;
  }

  ID3D12RootSignature *GlobalRootSignature() const {
    return m_global_root_signature;
  }

  bool ShaderRecordLocalRootSignature(
      const void *shader_identifier,
      ID3D12RootSignature **local_root_signature) const {
    if (!shader_identifier || !local_root_signature)
      return false;
    *local_root_signature = nullptr;
    for (const auto &identifier : m_shader_identifiers) {
      if (memcmp(identifier.second.data(), shader_identifier,
                 identifier.second.size()) != 0)
        continue;
      auto local_root = m_local_root_signatures.find(identifier.first);
      if (local_root != m_local_root_signatures.end())
        *local_root_signature = local_root->second;
      return true;
    }
    return false;
  }

  void RebuildShaderStackSizes() {
    m_shader_stack_sizes.clear();
    uint64_t maximum_stack_size = 0;
    for (const auto &export_name : m_exports) {
      auto import = m_export_imports.find(export_name);
      const std::wstring &canonical_name =
          import == m_export_imports.end() ? export_name : import->second;
      uint64_t stack_size = 0;
      if (canonical_name == L"raygen")
        stack_size = 64;
      else if (canonical_name == L"miss_shader")
        stack_size = 64;
      else if (canonical_name == L"callable_shader")
        stack_size = 64;
      else if (canonical_name == L"closest_hit")
        stack_size = 96;
      else if (canonical_name == L"any_hit")
        stack_size = 64;
      else if (canonical_name == L"procedural_intersection")
        stack_size = 64;
      else if (canonical_name == L"procedural_closest_hit")
        stack_size = 96;
      else if (canonical_name == L"hit_group") {
        m_shader_stack_sizes[export_name + L"::anyhit"] = 64;
        m_shader_stack_sizes[export_name + L"::closesthit"] = 96;
      } else if (canonical_name == L"procedural_hit_group") {
        m_shader_stack_sizes[export_name + L"::intersection"] = 64;
        m_shader_stack_sizes[export_name + L"::closesthit"] = 96;
      }
      if (stack_size) {
        m_shader_stack_sizes[export_name] = stack_size;
        maximum_stack_size = std::max(maximum_stack_size, stack_size);
      }
    }
    for (const auto &entry : m_shader_stack_sizes)
      maximum_stack_size = std::max(maximum_stack_size, entry.second);
    if (m_type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE &&
        !m_pipeline_stack_size && maximum_stack_size) {
      const uint64_t recursion = std::max<uint32_t>(
          1, std::min<uint32_t>(m_max_trace_recursion_depth, 2));
      m_pipeline_stack_size = std::min<uint64_t>(
          UINT64_C(0xfffffffe), maximum_stack_size * (recursion + 1));
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
        riid == IID_ID3D12StateObject) {
      *ppv = static_cast<ID3D12StateObject *>(this);
    } else if (riid == IID_ID3D12StateObjectProperties ||
               riid == IID_ID3D12StateObjectProperties1_ ||
               riid == IID_ID3D12StateObjectProperties2_) {
      *ppv = static_cast<ID3D12StateObjectProperties2Compat *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref_count; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --m_ref_count;
    if (!count)
      delete this;
    return count;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size,
                                           void *data) override {
    return m_private_data.getData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size,
                                           const void *data) override {
    return m_private_data.setData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
      REFGUID guid, const IUnknown *data) override {
    return m_private_data.setInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
    return m_private_data.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return m_device->QueryInterface(riid, device);
  }

  void *STDMETHODCALLTYPE GetShaderIdentifier(LPCWSTR export_name) override {
    TRACE("StateObjectProperties::GetShaderIdentifier export=%ls",
          export_name ? export_name : L"(null)");
    if (!export_name)
      return nullptr;
    auto identifier = m_shader_identifiers.find(export_name);
    return identifier == m_shader_identifiers.end()
               ? nullptr
               : identifier->second.data();
  }

  UINT64 STDMETHODCALLTYPE GetShaderStackSize(LPCWSTR export_name) override {
    TRACE("StateObjectProperties::GetShaderStackSize export=%ls",
          export_name ? export_name : L"(null)");
    if (!export_name)
      return UINT64_C(0xffffffff);
    auto stack_size = m_shader_stack_sizes.find(export_name);
    return stack_size == m_shader_stack_sizes.end()
               ? UINT64_C(0xffffffff)
               : stack_size->second;
  }

  UINT64 STDMETHODCALLTYPE GetPipelineStackSize() override {
    TRACE("StateObjectProperties::GetPipelineStackSize -> %llu",
          (unsigned long long)m_pipeline_stack_size);
    return m_pipeline_stack_size;
  }

  void STDMETHODCALLTYPE SetPipelineStackSize(UINT64 stack_size) override {
    TRACE("StateObjectProperties::SetPipelineStackSize %llu",
          (unsigned long long)stack_size);
    if (m_type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE ||
        stack_size >= UINT64_C(0xffffffff))
      return;
    m_pipeline_stack_size = stack_size;
  }

  D3D12ProgramIdentifierCompat *STDMETHODCALLTYPE GetProgramIdentifier(
      D3D12ProgramIdentifierCompat *ret, LPCWSTR program_name) override {
    TRACE("StateObjectProperties1::GetProgramIdentifier program=%ls",
          program_name ? program_name : L"(null)");
    if (!ret)
      return nullptr;
    for (size_t i = 0; i < 4; i++) {
      uint64_t word =
          0x4d313250524f4755ull ^ (uint64_t)i * 0x9e3779b97f4a7c15ull;
      if (program_name) {
        for (const WCHAR *p = program_name; *p; p++)
          word = (word ^ (uint16_t)*p) * 1099511628211ull;
      }
      word ^=
          ((uint64_t)m_type << 48) ^ ((uint64_t)m_subobject_types.size() << 32);
      ret->OpaqueData[i] = word;
    }
    return ret;
  }

  HRESULT STDMETHODCALLTYPE GetGlobalRootSignatureForProgram(
      LPCWSTR program_name, REFIID riid, void **root_signature) override {
    TRACE("StateObjectProperties2::GetGlobalRootSignatureForProgram "
          "program=%ls root=%p",
          program_name ? program_name : L"(null)",
          (void *)m_global_root_signature);
    if (!root_signature)
      return E_POINTER;
    *root_signature = nullptr;
    return m_global_root_signature
               ? m_global_root_signature->QueryInterface(riid, root_signature)
               : E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetGlobalRootSignatureForShader(
      LPCWSTR export_name, REFIID riid, void **root_signature) override {
    TRACE("StateObjectProperties2::GetGlobalRootSignatureForShader export=%ls "
          "root=%p",
          export_name ? export_name : L"(null)",
          (void *)m_global_root_signature);
    if (!root_signature)
      return E_POINTER;
    *root_signature = nullptr;
    return m_global_root_signature
               ? m_global_root_signature->QueryInterface(riid, root_signature)
               : E_NOINTERFACE;
  }

private:
  MTLD3D12Device *m_device = nullptr;
  ID3D12StateObject *m_base = nullptr;
  std::vector<ID3D12StateObject *> m_existing_collections;
  ID3D12RootSignature *m_global_root_signature = nullptr;
  std::unordered_map<std::wstring, ID3D12RootSignature *>
      m_local_root_signatures;
  WMT::Reference<WMT::ComputePipelineState> m_raygen_compute_pipeline;
  WMT::Reference<WMT::VisibleFunctionTable>
      m_raygen_visible_function_table;
  WMT::Reference<WMT::IntersectionFunctionTable>
      m_intersection_function_table;
  ComPrivateData m_private_data;
  std::atomic<ULONG> m_ref_count{1};
  D3D12_STATE_OBJECT_TYPE m_type = D3D12_STATE_OBJECT_TYPE_COLLECTION;
  std::vector<D3D12_STATE_SUBOBJECT_TYPE> m_subobject_types;
  std::vector<std::wstring> m_exports;
  std::unordered_map<std::wstring, std::wstring> m_export_imports;
  std::unordered_map<std::wstring, std::array<uint8_t, 32>>
      m_shader_identifiers;
  std::unordered_map<std::wstring, UINT64> m_shader_stack_sizes;
  UINT64 m_pipeline_stack_size = 0;
  UINT32 m_max_trace_recursion_depth = 1;
};

WMT::Reference<WMT::ComputePipelineState>
GetD3D12StateObjectRaygenComputePipeline(ID3D12StateObject *state_object) {
  if (!state_object)
    return {};
  return static_cast<MTLD3D12StateObject *>(state_object)
      ->RaygenComputePipeline();
}

WMT::Reference<WMT::VisibleFunctionTable>
GetD3D12StateObjectRaygenVisibleFunctionTable(
    ID3D12StateObject *state_object) {
  if (!state_object)
    return {};
  return static_cast<MTLD3D12StateObject *>(state_object)
      ->RaygenVisibleFunctionTable();
}

WMT::Reference<WMT::IntersectionFunctionTable>
GetD3D12StateObjectIntersectionFunctionTable(
    ID3D12StateObject *state_object) {
  if (!state_object)
    return {};
  return static_cast<MTLD3D12StateObject *>(state_object)
      ->IntersectionFunctionTable();
}

ID3D12RootSignature *
GetD3D12StateObjectGlobalRootSignature(ID3D12StateObject *state_object) {
  if (!state_object)
    return nullptr;
  return static_cast<MTLD3D12StateObject *>(state_object)
      ->GlobalRootSignature();
}

bool GetD3D12StateObjectShaderRecordLocalRootSignature(
    ID3D12StateObject *state_object, const void *shader_identifier,
    ID3D12RootSignature **local_root_signature) {
  if (!state_object)
    return false;
  return static_cast<MTLD3D12StateObject *>(state_object)
      ->ShaderRecordLocalRootSignature(shader_identifier,
                                       local_root_signature);
}

class MTLD3D12ShaderCacheSession : public ComObject<ID3D12ShaderCacheSession> {
public:
  MTLD3D12ShaderCacheSession(MTLD3D12Device *device,
                             const D3D12_SHADER_CACHE_SESSION_DESC &desc)
      : m_device(device), m_desc(desc) {
    m_device->AddRef();
    TRACE("ShaderCacheSession create mode=%u flags=0x%x max_bytes=%u "
          "max_entries=%u version=%llu",
          (unsigned)m_desc.Mode, (unsigned)m_desc.Flags,
          m_desc.MaximumInMemoryCacheSizeBytes,
          m_desc.MaximumInMemoryCacheEntries,
          (unsigned long long)m_desc.Version);
  }

  ~MTLD3D12ShaderCacheSession() { m_device->Release(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12ShaderCacheSession) {
      *ppv = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size,
                                           void *data) override {
    return m_private_data.getData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size,
                                           const void *data) override {
    return m_private_data.setData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
      REFGUID guid, const IUnknown *data) override {
    return m_private_data.setInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
    return m_private_data.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return m_device->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE FindValue(const void *key, UINT key_size,
                                      void *value, UINT *value_size) override {
    if (!value_size || (!key && key_size))
      return E_POINTER;
    auto iter = m_values.find(key_from_bytes(key, key_size));
    if (iter == m_values.end()) {
      TRACE("ShaderCacheSession FindValue miss key_size=%u", key_size);
      *value_size = 0;
      return DXGI_ERROR_NOT_FOUND;
    }

    UINT required = static_cast<UINT>(iter->second.size());
    if (!value) {
      *value_size = required;
      TRACE("ShaderCacheSession FindValue size query key_size=%u value_size=%u",
            key_size, required);
      return S_OK;
    }
    if (*value_size < required) {
      *value_size = required;
      TRACE("ShaderCacheSession FindValue buffer too small key_size=%u "
            "required=%u",
            key_size, required);
      return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }
    memcpy(value, iter->second.data(), required);
    *value_size = required;
    TRACE("ShaderCacheSession FindValue hit key_size=%u value_size=%u",
          key_size, required);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE StoreValue(const void *key, UINT key_size,
                                       const void *value,
                                       UINT value_size) override {
    if ((!key && key_size) || (!value && value_size))
      return E_POINTER;
    if (m_desc.MaximumInMemoryCacheEntries &&
        m_values.size() >= m_desc.MaximumInMemoryCacheEntries) {
      TRACE("ShaderCacheSession StoreValue rejected: entry limit=%u",
            m_desc.MaximumInMemoryCacheEntries);
      return E_OUTOFMEMORY;
    }
    if (m_desc.MaximumInMemoryCacheSizeBytes &&
        value_size > m_desc.MaximumInMemoryCacheSizeBytes) {
      TRACE("ShaderCacheSession StoreValue rejected: value_size=%u limit=%u",
            value_size, m_desc.MaximumInMemoryCacheSizeBytes);
      return E_OUTOFMEMORY;
    }
    auto &entry = m_values[key_from_bytes(key, key_size)];
    entry.resize(value_size);
    if (value_size)
      memcpy(entry.data(), value, value_size);
    TRACE("ShaderCacheSession StoreValue key_size=%u value_size=%u entries=%zu",
          key_size, value_size, m_values.size());
    return S_OK;
  }

  void STDMETHODCALLTYPE SetDeleteOnDestroy() override {
    m_delete_on_destroy = true;
    TRACE("ShaderCacheSession SetDeleteOnDestroy");
  }

  D3D12_SHADER_CACHE_SESSION_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_SHADER_CACHE_SESSION_DESC *__ret) override {
    if (__ret)
      *__ret = m_desc;
    return __ret;
  }

private:
  static std::string key_from_bytes(const void *key, UINT key_size) {
    if (!key || !key_size)
      return std::string();
    return std::string(static_cast<const char *>(key), key_size);
  }

  MTLD3D12Device *m_device;
  ComPrivateData m_private_data;
  D3D12_SHADER_CACHE_SESSION_DESC m_desc;
  std::unordered_map<std::string, std::vector<uint8_t>> m_values;
  bool m_delete_on_destroy = false;
};

const D3D12_COMMAND_SIGNATURE_DESC *
GetD3D12CommandSignatureDesc(ID3D12CommandSignature *signature) {
  if (!signature)
    return nullptr;
  return &static_cast<MTLD3D12CommandSignature *>(signature)->GetDesc();
}

} // namespace dxmt

namespace dxmt {

static void *g_device_this = nullptr;
static void *g_device_expected_vtable = nullptr;
static uint64_t g_device_expected_m_device = 0;
static std::atomic<bool> g_device_watcher_running{false};
static int g_watcher_restore_count = 0;

static bool device_vtable_watcher_enabled() {
  char value[8] = {};
  DWORD len = GetEnvironmentVariableA("DXMT_D3D12_VTABLE_WATCHER", value,
                                      sizeof(value));
  return len > 0 && len < sizeof(value) && value[0] == '1';
}

static void device_vtable_watcher() {
  int check_count = 0;
  int snapshot_count = 0;
  while (g_device_watcher_running.load()) {
    if (g_device_this) {
      void *current = *(void **)g_device_this;
      uint64_t current_m_device = *((uint64_t *)((char *)g_device_this + 8));
      bool vtable_bad = (current != g_device_expected_vtable);
      bool m_device_bad = (g_device_expected_m_device != 0 &&
                           current_m_device != g_device_expected_m_device);
      if (vtable_bad || m_device_bad) {
        g_watcher_restore_count++;
        FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
        if (f) {
          fprintf(f,
                  "!!! CORRUPTION #%d after %d checks: this=%p "
                  "vtable_expected=%p vtable_now=%p m_device_expected=0x%llx "
                  "m_device_now=0x%llx watcher_tid=%lu\n",
                  g_watcher_restore_count, check_count, g_device_this,
                  g_device_expected_vtable, current,
                  (unsigned long long)g_device_expected_m_device,
                  (unsigned long long)current_m_device,
                  (unsigned long)GetCurrentThreadId());
          unsigned char *raw = (unsigned char *)g_device_this;
          fprintf(f, "!!! DEVICE DUMP [0x00-0x3F]:");
          for (int i = 0; i < 64; i += 8) {
            fprintf(f, " %02x%02x%02x%02x%02x%02x%02x%02x", raw[i], raw[i + 1],
                    raw[i + 2], raw[i + 3], raw[i + 4], raw[i + 5], raw[i + 6],
                    raw[i + 7]);
          }
          fprintf(f, "\n!!! DEVICE DUMP [0x40-0x7F]:");
          for (int i = 64; i < 128; i += 8) {
            fprintf(f, " %02x%02x%02x%02x%02x%02x%02x%02x", raw[i], raw[i + 1],
                    raw[i + 2], raw[i + 3], raw[i + 4], raw[i + 5], raw[i + 6],
                    raw[i + 7]);
          }
          fprintf(f, "\n");
          fclose(f);
        }
        *(void **)g_device_this = g_device_expected_vtable;
        if (g_device_expected_m_device != 0) {
          *((uint64_t *)((char *)g_device_this + 8)) =
              g_device_expected_m_device;
        }
        check_count = 0;
        continue;
      }
      check_count++;
      snapshot_count++;
      if (snapshot_count % 10000 == 0) {
        FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
        if (f) {
          fprintf(f, "watcher snapshot #%d: vtable=%p m_device=0x%llx OK\n",
                  snapshot_count, current,
                  (unsigned long long)current_m_device);
          fclose(f);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

static const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER &
RaytracingSerializationIdentifierForProcess() {
  static const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER identifier =
      []() {
        D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER value = {};
        value.DriverOpaqueGUID =
            {0x4d545341, 0x5345, 0x5231,
             {0x81, 0x27, 0x4d, 0x65, 0x74, 0x61, 0x6c, 0x34}};
        const uint32_t process_id = GetCurrentProcessId();
        LARGE_INTEGER counter = {};
        QueryPerformanceCounter(&counter);
        const uint32_t format_version = 1;
        std::memcpy(value.DriverOpaqueVersioningData, &process_id,
                    sizeof(process_id));
        std::memcpy(value.DriverOpaqueVersioningData + sizeof(process_id),
                    &counter.QuadPart, sizeof(counter.QuadPart));
        std::memcpy(value.DriverOpaqueVersioningData + sizeof(process_id) +
                        sizeof(counter.QuadPart),
                    &format_version, sizeof(format_version));
        return value;
      }();
  return identifier;
}

MTLD3D12Device::MTLD3D12Device(std::unique_ptr<Device> &&device,
                               IMTLDXGIAdapter *pAdapter)
    : m_device(std::move(device)), m_adapter(pAdapter) {
  m_format_inspector.Inspect(GetMTLDevice());
  const auto &host_capabilities = m_device->capabilities();
  m_metal_raytracing_supported =
      host_capabilities.supports_native_raytracing;
  m_raytracing_serialization_identifier =
      RaytracingSerializationIdentifierForProcess();
  Logger::info(str::format(
      "D3D12 host capabilities schema=", host_capabilities.schema_version,
      " metal=", host_capabilities.metal_version,
      " registry=0x", std::hex, host_capabilities.registry_id, std::dec,
      " family7=", HostCapabilityBool(host_capabilities.apple_family7),
      " family8=", HostCapabilityBool(host_capabilities.apple_family8),
      " family9=", HostCapabilityBool(host_capabilities.apple_family9),
      " mtl4=", HostCapabilityBool(host_capabilities.supports_mtl4_command_queue),
      " shared_events=", HostCapabilityBool(host_capabilities.supports_shared_events),
      " raytracing=", HostCapabilityBool(host_capabilities.supports_native_raytracing),
      " sample_mask=0x", std::hex, host_capabilities.texture_sample_counts_mask,
      std::dec));
  const auto native_provider = m_device->selectProvider({});
  Logger::info(str::format("D3D12 default provider=",
                           ProviderKindName(native_provider.kind),
                           " available=", native_provider.available ? 1 : 0,
                           " (feature promotion remains behavior-gated)"));
  if (m_adapter)
    m_adapter->AddRef();
  m_expected_vtable = *(void **)this;
  g_device_this = (void *)this;
  g_device_expected_vtable = m_expected_vtable;
  g_device_expected_m_device = (uint64_t)m_device.get();
  TRACE("M12 feature contract build=full_caps_current_pipeline_20260517");
  TRACE("Device ctor: this=%p vtable=%p m_device=%p sizeof=%zu", (void *)this,
        m_expected_vtable, (void *)m_device.get(), sizeof(MTLD3D12Device));
  extern void *g_d3d12_device_addr;
  extern size_t g_d3d12_device_size;
  g_d3d12_device_addr = (void *)this;
  g_d3d12_device_size = sizeof(MTLD3D12Device);
  TRACE("Device ctor: registered device guard at %p size=%zu",
        g_d3d12_device_addr, g_d3d12_device_size);
  if (device_vtable_watcher_enabled()) {
    g_device_watcher_running.store(true);
    dxmt::thread watcher([]() { device_vtable_watcher(); });
    watcher.detach();
  }
  Logger::info("D3D12 device created via DXMT Metal backend");
}

MTLD3D12Device::~MTLD3D12Device() {
  {
    std::lock_guard lock(m_info_queue_mutex);
    if (m_info_queue) {
      m_info_queue->Release();
      m_info_queue = nullptr;
    }
  }
  if (g_device_this == this) {
    g_device_watcher_running.store(false);
    g_device_this = nullptr;
    g_device_expected_vtable = nullptr;
    g_device_expected_m_device = 0;
  }
  void *current_vt = *(void **)this;
  FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
  if (f) {
    fprintf(f, "Device REAL DTOR this=%p vtable=%p expected=%p m_refCount=%u\n",
            (void *)this, current_vt, m_expected_vtable, m_refCount.load());
    fclose(f);
  }
  Logger::info("D3D12 device destroyed");
}

void MTLD3D12Device::CheckVtable(const char *where) {
  void *current = *(void **)this;
  if (current != m_expected_vtable) {
    TRACE(
        "VTABLE CORRUPTION at %s: expected=%p got=%p this=%p — AUTO-RESTORING",
        where, m_expected_vtable, current, (void *)this);
    *(void **)this = m_expected_vtable;
  }
}

WMT::Device MTLD3D12Device::GetMTLDevice() { return m_device->device(); }

Device &MTLD3D12Device::GetDXMTDevice() { return *m_device; }

namespace {

struct D3D12QueueCompletionPoint {
  WMT::Reference<WMT::SharedEvent> event;
  uint64_t value;
};

struct D3D12DeviceEventWaitContext {
  std::vector<D3D12QueueCompletionPoint> points;
  HANDLE event;
};

DWORD WINAPI D3D12DeviceEventWaitThread(void *argument) {
  auto *context = static_cast<D3D12DeviceEventWaitContext *>(argument);
  for (const auto &point : context->points)
    point.event.waitUntilSignaledValue(point.value, UINT64_MAX);
  SetEvent(context->event);
  CloseHandle(context->event);
  delete context;
  return 0;
}

} // namespace

void MTLD3D12Device::RegisterCommandQueue(MTLD3D12CommandQueue *queue) {
  std::lock_guard lock(m_command_queue_mutex);
  m_command_queues.push_back(queue);
  TRACE("RegisterCommandQueue queue=%p count=%zu", (void *)queue,
        m_command_queues.size());
}

void MTLD3D12Device::UnregisterCommandQueue(MTLD3D12CommandQueue *queue) {
  std::lock_guard lock(m_command_queue_mutex);
  auto entry = std::find(m_command_queues.begin(), m_command_queues.end(),
                         queue);
  if (entry != m_command_queues.end())
    m_command_queues.erase(entry);
  TRACE("UnregisterCommandQueue queue=%p count=%zu", (void *)queue,
        m_command_queues.size());
}

HRESULT MTLD3D12Device::EnqueueSetEvent(HANDLE event) {
  if (!event)
    return E_INVALIDARG;

  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                       &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return HRESULT_FROM_WIN32(GetLastError());

  auto *context = new (std::nothrow) D3D12DeviceEventWaitContext;
  if (!context) {
    CloseHandle(duplicate);
    return E_OUTOFMEMORY;
  }
  context->event = duplicate;

  try {
    std::lock_guard lock(m_command_queue_mutex);
    context->points.reserve(m_command_queues.size());
    for (auto *queue : m_command_queues) {
      D3D12QueueCompletionPoint point = {};
      if (!queue->EnqueueCompletionSignal(point.event, point.value)) {
        CloseHandle(duplicate);
        delete context;
        return E_FAIL;
      }
      context->points.push_back(std::move(point));
    }
  } catch (const std::bad_alloc &) {
    CloseHandle(duplicate);
    delete context;
    return E_OUTOFMEMORY;
  }

  TRACE("EnqueueSetEvent event=%p queue_count=%zu", event,
        context->points.size());
  if (context->points.empty()) {
    SetEvent(duplicate);
    CloseHandle(duplicate);
    delete context;
    return S_OK;
  }

  HANDLE thread =
      CreateThread(nullptr, 0, D3D12DeviceEventWaitThread, context, 0, nullptr);
  if (!thread) {
    CloseHandle(duplicate);
    delete context;
    return HRESULT_FROM_WIN32(GetLastError());
  }
  CloseHandle(thread);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::QueryInterface(REFIID riid,
                                                         void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  TRACE("D3D12Device::QI(%s)", str::format(riid).c_str());

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
      riid == IID_ID3D12Device || riid == IID_ID3D12Device1 ||
      riid == IID_ID3D12Device2 || riid == IID_ID3D12Device3 ||
      riid == IID_ID3D12Device4 || riid == IID_ID3D12Device5 ||
      riid == IID_ID3D12Device6 || riid == IID_ID3D12Device7 ||
      riid == IID_ID3D12Device8 || riid == IID_ID3D12Device9 ||
      riid == IID_ID3D12Device10 || riid == IID_ID3D12Device11_ ||
      riid == IID_ID3D12Device12_) {
    *ppvObject = ref(this);
    TRACE("D3D12Device::QI(%s) -> S_OK (device)", str::format(riid).c_str());
    return S_OK;
  }

  if (riid == IID_ID3D12InfoQueue) {
    std::lock_guard lock(m_info_queue_mutex);
    if (!m_info_queue) {
      m_info_queue = new (std::nothrow) MTLD3D12InfoQueue();
      if (!m_info_queue)
        return E_OUTOFMEMORY;
    }
    *ppvObject = m_info_queue;
    m_info_queue->AddRef();
    TRACE("D3D12Device::QI(%s) -> S_OK (shared info queue)",
          str::format(riid).c_str());
    return S_OK;
  }

  if (riid == __uuidof(IMTLDXGIDevice) && m_dxgi_device) {
    TRACE("D3D12Device::QI(%s) -> delegating to dxgi_device",
          str::format(riid).c_str());
    return m_dxgi_device->QueryInterface(riid, ppvObject);
  }

  if (m_dxgi_device) {
    if (riid == IID_IDXGIDevice || riid == __uuidof(IDXGIDevice1) ||
        riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3)) {
      TRACE("D3D12Device::QI(%s) -> delegating DXGI to dxgi_device",
            str::format(riid).c_str());
      return m_dxgi_device->QueryInterface(riid, ppvObject);
    }
  }

  Logger::warn(str::format("D3D12Device::QueryInterface: unknown IID ", riid));
  TRACE("D3D12Device::QI(%s) -> E_NOINTERFACE", str::format(riid).c_str());
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12Device::AddRef() {
  CheckVtable("AddRef");
  uint32_t rc = m_refCount++;
  if (!rc)
    ++m_refPrivate;
  return rc + 1;
}

ULONG STDMETHODCALLTYPE MTLD3D12Device::Release() {
  CheckVtable("Release");
  uint32_t rc = --m_refCount;
  if (rc <= 1)
    TRACE("Device::Release rc=%u this=%p", rc, (void *)this);
  if (rc == 1 && m_dxgi_device && !m_dxgi_owner_released.exchange(true)) {
    TRACE("Device::Release dropping companion DXGI-device owner ref this=%p "
          "dxgi=%p",
          (void *)this, (void *)m_dxgi_device);
    m_dxgi_device->Release();
    return rc;
  }
  if (!rc) {
    uint32_t rp = --m_refPrivate;
    if (!rp) {
      TRACE("Device::Release DELETING this=%p", (void *)this);
      m_refPrivate += 0x80000000;
      this->~MTLD3D12Device();
      VirtualFree(this, 0, MEM_RELEASE);
    }
  }
  return rc;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::GetPrivateData(REFGUID guid,
                                                         UINT *data_size,
                                                         void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::SetPrivateData(REFGUID guid,
                                                         UINT data_size,
                                                         const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Device::SetPrivateDataInterface(REFGUID guid, const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

UINT STDMETHODCALLTYPE MTLD3D12Device::GetNodeCount() { return 1; }

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommandQueue(
    const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **command_queue) {
  TRACE("CreateCommandQueue type=%u", desc ? desc->Type : 0xFF);
  if (!desc || !command_queue)
    return E_POINTER;
  InitReturnPtr(command_queue);

  auto queue = new MTLD3D12CommandQueue(this, m_device->queue(), *desc);
  HRESULT hr = queue->QueryInterface(riid, command_queue);
  queue->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **command_allocator) {
  TRACE("CreateCommandAllocator type=%u", type);
  if (!command_allocator)
    return E_POINTER;
  InitReturnPtr(command_allocator);

  auto allocator = new MTLD3D12CommandAllocator(this, type);
  HRESULT hr = allocator->QueryInterface(riid, command_allocator);
  allocator->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
    void **pipeline_state) {
  return CreateGraphicsPipelineStateInternal(desc, riid, pipeline_state,
                                             false);
}

HRESULT MTLD3D12Device::CreateGraphicsPipelineStateInternal(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
    void **pipeline_state, bool depth_bounds_test_enable) {
  if (!desc || !pipeline_state)
    return E_POINTER;
  InitReturnPtr(pipeline_state);

  TRACE(
      "CreateGraphicsPSO ENTER: VS=%p(%zu) PS=%p(%zu) NumRT=%u DSV=%u Topo=%u",
      desc->VS.pShaderBytecode, desc->VS.BytecodeLength,
      desc->PS.pShaderBytecode, desc->PS.BytecodeLength, desc->NumRenderTargets,
      (unsigned)desc->DSVFormat, (unsigned)desc->PrimitiveTopologyType);
  TRACE("CreateGraphicsPSO SHADERS: VS_HASH=0x%llx PS_HASH=0x%llx "
        "DS_HASH=0x%llx HS_HASH=0x%llx GS_HASH=0x%llx IL=%u",
        desc->VS.pShaderBytecode
            ? (unsigned long long)DXMTD3D12Hash64(desc->VS.pShaderBytecode,
                                                  desc->VS.BytecodeLength)
            : 0ull,
        desc->PS.pShaderBytecode
            ? (unsigned long long)DXMTD3D12Hash64(desc->PS.pShaderBytecode,
                                                  desc->PS.BytecodeLength)
            : 0ull,
        desc->DS.pShaderBytecode
            ? (unsigned long long)DXMTD3D12Hash64(desc->DS.pShaderBytecode,
                                                  desc->DS.BytecodeLength)
            : 0ull,
        desc->HS.pShaderBytecode
            ? (unsigned long long)DXMTD3D12Hash64(desc->HS.pShaderBytecode,
                                                  desc->HS.BytecodeLength)
            : 0ull,
        desc->GS.pShaderBytecode
            ? (unsigned long long)DXMTD3D12Hash64(desc->GS.pShaderBytecode,
                                                  desc->GS.BytecodeLength)
            : 0ull,
        desc->InputLayout.NumElements);
  DXMTD3D12ScopedTimer create_timer("Device", "CreateGraphicsPSO");
  create_timer.SetDetail("vs=%zu ps=%zu ds=%zu hs=%zu gs=%zu rt=%u il=%u",
                         desc->VS.BytecodeLength, desc->PS.BytecodeLength,
                         desc->DS.BytecodeLength, desc->HS.BytecodeLength,
                         desc->GS.BytecodeLength, desc->NumRenderTargets,
                         desc->InputLayout.NumElements);

  const bool native_tessellation_required =
      D3D12NativeTessellationRequired(*desc);
  if (native_tessellation_required) {
    auto metadata = InspectD3D12NativeTessellationPSO(*desc);
    auto detail = DescribeD3D12NativeTessellationPSO(metadata);
    Logger::warn(str::format("M12 native_tessellation_required: ", detail));
    TRACE("CreateGraphicsPSO native_tessellation_required %s", detail.c_str());
  }

  auto pso = new MTLD3D12PipelineState(this, false);
  pso->SetGraphicsDesc(*desc);
  pso->SetDepthBoundsTestEnable(depth_bounds_test_enable);
  bool compiled = pso->RequestCompile(!native_tessellation_required);
  auto failure_stage = pso->GetCompileFailureStage();
  auto failure_detail = pso->GetCompileFailureDetail();
  TRACE(
      "CreateGraphicsPSO: compile=%d pending=%d VS=%p PS=%p stage=%s detail=%s",
      compiled, pso->IsCompilePending(), desc->VS.pShaderBytecode,
      desc->PS.pShaderBytecode, failure_stage.c_str(), failure_detail.c_str());
  if (!compiled && !pso->IsCompilePending()) {
    Logger::warn(str::format(
        "CreateGraphicsPipelineState: shader compilation deferred/failed at ",
        failure_stage, ": ", failure_detail));
    const bool unsupported_pso_state =
        failure_stage.rfind("pso/unsupported_", 0) == 0 ||
        failure_stage.rfind("pso/metal_", 0) == 0;
    const bool shader_compile_failure = failure_stage.rfind("shader/", 0) == 0;
    char strict_fail[8] = {};
    const bool strict_deferred =
        GetEnvironmentVariableA("DXMT_D3D12_FAIL_DEFERRED_PSO", strict_fail,
                                sizeof(strict_fail)) > 0 &&
        strict_fail[0] && strict_fail[0] != '0';
    if (unsupported_pso_state || shader_compile_failure || strict_deferred) {
      Logger::warn(str::format(
          "CreateGraphicsPipelineState: failing PSO creation for stage ",
          failure_stage, strict_deferred ? " strict=1" : ""));
      pso->Release();
      return E_FAIL;
    }
  }
  HRESULT hr = pso->QueryInterface(riid, pipeline_state);
  pso->Release();
  TRACE("CreateGraphicsPSO EXIT hr=0x%lx pso=%p", hr, *pipeline_state);
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateComputePipelineState(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid,
    void **pipeline_state) {
  if (!desc || !pipeline_state)
    return E_POINTER;
  InitReturnPtr(pipeline_state);

  auto pso = new MTLD3D12PipelineState(this, true);
  pso->SetComputeDesc(*desc);
  DXMTD3D12ScopedTimer create_timer("Device", "CreateComputePSO");
  create_timer.SetDetail("cs=%zu root=%p", desc->CS.BytecodeLength,
                         (void *)desc->pRootSignature);
  TRACE("CreateComputePSO ENTER: CS=%p(%zu) CS_HASH=0x%llx root=%p",
        desc->CS.pShaderBytecode, desc->CS.BytecodeLength,
        desc->CS.pShaderBytecode
            ? (unsigned long long)DXMTD3D12Hash64(desc->CS.pShaderBytecode,
                                                  desc->CS.BytecodeLength)
            : 0ull,
        (void *)desc->pRootSignature);
  bool compiled = pso->RequestCompile(true);
  auto failure_stage = pso->GetCompileFailureStage();
  auto failure_detail = pso->GetCompileFailureDetail();
  TRACE("CreateComputePSO: compile=%d pending=%d CS=%p stage=%s detail=%s",
        compiled, pso->IsCompilePending(), desc->CS.pShaderBytecode,
        failure_stage.c_str(), failure_detail.c_str());
  if (!compiled && !pso->IsCompilePending()) {
    Logger::warn(str::format(
        "CreateComputePipelineState: shader compilation deferred/failed at ",
        failure_stage, ": ", failure_detail));
    const bool shader_compile_failure =
        failure_stage.rfind("shader/", 0) == 0 ||
        failure_stage.rfind("pso/", 0) == 0;
    if (shader_compile_failure) {
      Logger::warn(str::format(
          "CreateComputePipelineState: failing PSO creation for stage ",
          failure_stage));
      pso->Release();
      return E_FAIL;
    }
  }
  HRESULT hr = pso->QueryInterface(riid, pipeline_state);
  pso->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Device::CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                  ID3D12CommandAllocator *command_allocator,
                                  ID3D12PipelineState *initial_pipeline_state,
                                  REFIID riid, void **command_list) {
  TRACE("CreateCommandList type=%u", type);
  if (!command_list)
    return E_POINTER;
  InitReturnPtr(command_list);

  auto allocator = static_cast<MTLD3D12CommandAllocator *>(command_allocator);
  auto list = new MTLD3D12GraphicsCommandList(this, allocator, type,
                                              initial_pipeline_state);
  HRESULT hr = list->QueryInterface(riid, command_list);
  list->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CheckFeatureSupport(
    D3D12_FEATURE feature, void *feature_data, UINT feature_data_size) {
  TRACE("CheckFeatureSupport this=%p feature=%u data=%p size=%u", (void *)this,
        (unsigned)feature, feature_data, feature_data_size);
  if ((UINT_PTR)feature_data > 0 && (UINT_PTR)feature_data < 0x10000) {
    TRACE("!!! SUSPICIOUS CheckFeatureSupport: feature_data=%p looks like "
          "row_pitch (small int), this=%p — probable vtable slot 13 collision "
          "(ReadFromSubresource->CheckFeatureSupport)",
          feature_data, (void *)this);
  }
  if (!feature_data)
    return feature_data_size ? E_POINTER : E_INVALIDARG;
  if (!feature_data_size)
    return E_INVALIDARG;
  switch ((UINT)feature) {
  case D3D12_FEATURE_D3D12_OPTIONS: {
    auto *opts = (D3D12_FEATURE_DATA_D3D12_OPTIONS *)feature_data;
    if (feature_data_size < sizeof(*opts))
      return E_INVALIDARG;
    opts->DoublePrecisionFloatShaderOps = FALSE;
    opts->OutputMergerLogicOp = TRUE;
    opts->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_NONE;
    opts->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_3;
    opts->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
    opts->PSSpecifiedStencilRefSupported = TRUE;
    opts->TypedUAVLoadAdditionalFormats = TRUE;
    // Rasterizer-ordered UAV access is not implemented by the Metal render
    // encoder.  A normal UAV barrier is not an ROV substitute.
    opts->ROVsSupported = FALSE;
    // The bounded reference-model coverage path is used for supported
    // rasterizer descriptions; unsupported combinations still fail during
    // pipeline creation rather than silently falling back.
    opts->ConservativeRasterizationTier =
        D3D12_CONSERVATIVE_RASTERIZATION_TIER_3;
    opts->MaxGPUVirtualAddressBitsPerResource = 40;
    opts->StandardSwizzle64KBSupported = FALSE;
    opts->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
    opts->CrossAdapterRowMajorTextureSupported = FALSE;
    opts->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation =
        TRUE;
    opts->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
    TRACE(
        "  OPTIONS: DoubleFP=%d LogicOp=%d TiledTier=%u BindingTier=%u "
        "PSStencilRef=%d TypedUAV=%d ROV=%d ConsRaster=%u VAbit=%u HeapTier=%u",
        opts->DoublePrecisionFloatShaderOps, opts->OutputMergerLogicOp,
        opts->TiledResourcesTier, opts->ResourceBindingTier,
        opts->PSSpecifiedStencilRefSupported,
        opts->TypedUAVLoadAdditionalFormats, opts->ROVsSupported,
        opts->ConservativeRasterizationTier,
        opts->MaxGPUVirtualAddressBitsPerResource, opts->ResourceHeapTier);
    return S_OK;
  }
  case D3D12_FEATURE_ARCHITECTURE: {
    auto *arch = (D3D12_FEATURE_DATA_ARCHITECTURE *)feature_data;
    if (feature_data_size < sizeof(*arch))
      return E_INVALIDARG;
    arch->NodeIndex = 0;
    arch->TileBasedRenderer = FALSE;
    arch->UMA = TRUE;
    arch->CacheCoherentUMA = TRUE;
    return S_OK;
  }
  case D3D12_FEATURE_FEATURE_LEVELS: {
    auto *fl = (D3D12_FEATURE_DATA_FEATURE_LEVELS *)feature_data;
    if (feature_data_size < sizeof(*fl))
      return E_INVALIDARG;
    if (fl->NumFeatureLevels && !fl->pFeatureLevelsRequested)
      return E_INVALIDARG;
    fl->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_9_1;
    for (UINT i = 0; i < fl->NumFeatureLevels; i++) {
      if (fl->pFeatureLevelsRequested[i] <= D3D12ConfiguredMaximumFeatureLevel() &&
          fl->pFeatureLevelsRequested[i] > fl->MaxSupportedFeatureLevel) {
        fl->MaxSupportedFeatureLevel = fl->pFeatureLevelsRequested[i];
      }
    }
    TRACE("  FEATURE_LEVELS: MaxSupported=%u (from %u requested)",
          (unsigned)fl->MaxSupportedFeatureLevel, fl->NumFeatureLevels);
    return S_OK;
  }
  case D3D12_FEATURE_FORMAT_SUPPORT: {
    auto *fmt = (D3D12_FEATURE_DATA_FORMAT_SUPPORT *)feature_data;
    if (feature_data_size < sizeof(*fmt))
      return E_INVALIDARG;
    TRACE("  FORMAT_SUPPORT: format=%u", (unsigned)fmt->Format);
    fmt->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
    fmt->Support2 = D3D12_FORMAT_SUPPORT2_NONE;

    if (fmt->Format == DXGI_FORMAT_UNKNOWN) {
      fmt->Support1 = D3D12_FORMAT_SUPPORT1_BUFFER;
      fmt->Support2 =
          (D3D12_FORMAT_SUPPORT2)(D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX |
                                  D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
                                  D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
      TRACE("  FORMAT_SUPPORT: format=%u Support1=0x%x Support2=0x%x",
            (unsigned)fmt->Format, (unsigned)fmt->Support1,
            (unsigned)fmt->Support2);
      return S_OK;
    }
    if (fmt->Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ||
        fmt->Format ==
            DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE) {
      fmt->Support1 = (D3D12_FORMAT_SUPPORT1)(
          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_MIP);
      fmt->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
      TRACE("  FORMAT_SUPPORT: sampler-feedback format=%u Support1=0x%x Support2=0x%x",
            (unsigned)fmt->Format, (unsigned)fmt->Support1,
            (unsigned)fmt->Support2);
      return S_OK;
    }

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), fmt->Format, metal_format))) {
      TRACE("  FORMAT_SUPPORT: format=%u unsupported by MTLQueryDXGIFormat",
            (unsigned)fmt->Format);
      return E_INVALIDARG;
    }

    D3D12_FORMAT_SUPPORT1 support1 = D3D12_FORMAT_SUPPORT1_NONE;
    D3D12_FORMAT_SUPPORT2 support2 = D3D12_FORMAT_SUPPORT2_NONE;

    if (metal_format.PixelFormat) {
      support1 =
          (D3D12_FORMAT_SUPPORT1)(support1 | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                                  D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                                  D3D12_FORMAT_SUPPORT1_SHADER_GATHER |
                                  D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD |
                                  D3D12_FORMAT_SUPPORT1_TEXTURE1D |
                                  D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                                  D3D12_FORMAT_SUPPORT1_TEXTURE3D |
                                  D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                                  D3D12_FORMAT_SUPPORT1_MIP |
                                  D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT);

      if (!(metal_format.Flag &
            (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER |
             MTL_DXGI_FORMAT_STENCIL_PLANER))) {
        support1 =
            (D3D12_FORMAT_SUPPORT1)(support1 | D3D12_FORMAT_SUPPORT1_BUFFER);
      }

      if (metal_format.Flag & MTL_DXGI_FORMAT_BACKBUFFER) {
        support1 =
            (D3D12_FORMAT_SUPPORT1)(support1 | D3D12_FORMAT_SUPPORT1_DISPLAY);
      }
    }

    if (metal_format.AttributeFormat) {
      support1 =
          (D3D12_FORMAT_SUPPORT1)(support1 |
                                  D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER);
    }

    if (metal_format.PixelFormat == WMTPixelFormatR32Uint ||
        metal_format.PixelFormat == WMTPixelFormatR16Uint) {
      support1 = (D3D12_FORMAT_SUPPORT1)(support1 |
                                         D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER);
    }

    auto capability =
        query_format_capability(m_format_inspector, metal_format.PixelFormat);

    if (has_format_capability(capability, FormatCapability::Color)) {
      support1 = (D3D12_FORMAT_SUPPORT1)(support1 |
                                         D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    }

    if (has_format_capability(capability, FormatCapability::Blend)) {
      support1 =
          (D3D12_FORMAT_SUPPORT1)(support1 | D3D12_FORMAT_SUPPORT1_BLENDABLE);
    }

    if (has_format_capability(capability, FormatCapability::DepthStencil)) {
      support1 =
          (D3D12_FORMAT_SUPPORT1)(support1 |
                                  D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL |
                                  D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                                  D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON);
    }

    if (has_format_capability(capability, FormatCapability::Resolve)) {
      support1 =
          (D3D12_FORMAT_SUPPORT1)(support1 |
                                  D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);
    }

    if (has_format_capability(capability, FormatCapability::MSAA)) {
      support1 =
          (D3D12_FORMAT_SUPPORT1)(support1 |
                                  D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET);
    }

    if (has_format_capability(capability, FormatCapability::Write)) {
      support1 =
          (D3D12_FORMAT_SUPPORT1)(support1 |
                                  D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
    }

    if (has_format_capability(capability,
                              FormatCapability::TextureBufferRead)) {
      support2 = (D3D12_FORMAT_SUPPORT2)(support2 |
                                         D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD);
    }

    if (has_format_capability(capability,
                              FormatCapability::TextureBufferWrite)) {
      support2 = (D3D12_FORMAT_SUPPORT2)(support2 |
                                         D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }

    if (has_format_capability(capability,
                              FormatCapability::TextureBufferReadWrite)) {
      support2 = (D3D12_FORMAT_SUPPORT2)(support2 |
                                         D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
                                         D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }

    if (has_format_capability(capability, FormatCapability::Atomic)) {
      support2 =
          (D3D12_FORMAT_SUPPORT2)(support2 |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
                                  D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX);
    }

    if (has_format_capability(capability, FormatCapability::Blend)) {
      support2 =
          (D3D12_FORMAT_SUPPORT2)(support2 |
                                  D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP);
    }

    if (has_format_capability(capability, FormatCapability::Sparse)) {
      support2 =
          (D3D12_FORMAT_SUPPORT2)(support2 | D3D12_FORMAT_SUPPORT2_TILED);
    }

    fmt->Support1 = support1;
    fmt->Support2 = support2;
    TRACE("  FORMAT_SUPPORT: format=%u Support1=0x%x Support2=0x%x",
          (unsigned)fmt->Format, (unsigned)fmt->Support1,
          (unsigned)fmt->Support2);
    return S_OK;
  }
  case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
    auto *ms = (D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *)feature_data;
    if (feature_data_size < sizeof(*ms))
      return E_INVALIDARG;
    ms->NumQualityLevels = 1;
    return S_OK;
  }
  case D3D12_FEATURE_FORMAT_INFO: {
    auto *fi = (D3D12_FEATURE_DATA_FORMAT_INFO *)feature_data;
    if (feature_data_size < sizeof(*fi))
      return E_INVALIDARG;
    fi->PlaneCount = FormatPlaneCount(fi->Format);
    return S_OK;
  }
  case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
    auto *va = (D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *)feature_data;
    if (feature_data_size < sizeof(*va))
      return E_INVALIDARG;
    va->MaxGPUVirtualAddressBitsPerResource = 40;
    va->MaxGPUVirtualAddressBitsPerProcess = 40;
    return S_OK;
  }
  case D3D12_FEATURE_SHADER_MODEL: {
    auto *sm = (D3D12_FEATURE_DATA_SHADER_MODEL *)feature_data;
    if (feature_data_size < sizeof(*sm))
      return E_INVALIDARG;
    constexpr D3D_SHADER_MODEL max_shader_model =
        static_cast<D3D_SHADER_MODEL>(0x67);
    if (sm->HighestShaderModel == 0 ||
        sm->HighestShaderModel > max_shader_model)
      sm->HighestShaderModel = max_shader_model;
    TRACE("  SHADER_MODEL: HighestSM=%u behavior-backed maximum=%u",
          (unsigned)sm->HighestShaderModel, (unsigned)max_shader_model);
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS1: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS1 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->WaveOps = TRUE;
    o->WaveLaneCountMin = 32;
    o->WaveLaneCountMax = 32;
    o->TotalLaneCount = 32;
    o->ExpandedComputeResourceStates = TRUE;
    o->Int64ShaderOps = TRUE;
    return S_OK;
  }
  case D3D12_FEATURE_ROOT_SIGNATURE: {
    auto *rs = (D3D12_FEATURE_DATA_ROOT_SIGNATURE *)feature_data;
    if (feature_data_size < sizeof(*rs))
      return E_INVALIDARG;
    rs->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    return S_OK;
  }
  case D3D12_FEATURE_ARCHITECTURE1: {
    auto *a = (D3D12_FEATURE_DATA_ARCHITECTURE1 *)feature_data;
    if (feature_data_size < sizeof(*a))
      return E_INVALIDARG;
    a->NodeIndex = 0;
    a->TileBasedRenderer = FALSE;
    a->UMA = TRUE;
    a->CacheCoherentUMA = TRUE;
    a->IsolatedMMU = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS2: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS2 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->DepthBoundsTestSupported = TRUE;
    // Metal supports a single programmable sample pattern for each
    // multisampled render pass; expose the corresponding D3D12 tier-1 shape.
    o->ProgrammableSamplePositionsTier =
        D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1;
    return S_OK;
  }
  case D3D12_FEATURE_SHADER_CACHE: {
    auto *sc = (D3D12_FEATURE_DATA_SHADER_CACHE *)feature_data;
    if (feature_data_size < sizeof(*sc))
      return E_INVALIDARG;
    sc->SupportFlags =
        (D3D12_SHADER_CACHE_SUPPORT_FLAGS)(D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO |
                                           D3D12_SHADER_CACHE_SUPPORT_LIBRARY);
    TRACE("  SHADER_CACHE: SupportFlags=0x%x", (unsigned)sc->SupportFlags);
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS3: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS3 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->CopyQueueTimestampQueriesSupported = TRUE;
    o->CastingFullyTypedFormatSupported = TRUE;
    o->WriteBufferImmediateSupportFlags =
        (D3D12_COMMAND_LIST_SUPPORT_FLAGS)(
            D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT |
            D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
            D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE);
    o->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
    o->BarycentricsSupported = FALSE;
    TRACE("  OPTIONS3: CopyQueueTS=%d CastFullyTyped=%d WriteBufImm=0x%x "
          "ViewInstTier=%u Bary=%d",
          o->CopyQueueTimestampQueriesSupported,
          o->CastingFullyTypedFormatSupported,
          o->WriteBufferImmediateSupportFlags, o->ViewInstancingTier,
          o->BarycentricsSupported);
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS4: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS4 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->MSAA64KBAlignedTextureSupported = FALSE;
    o->SharedResourceCompatibilityTier =
        D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
    o->Native16BitShaderOpsSupported = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_SERIALIZATION: {
    auto *s = (D3D12_FEATURE_DATA_SERIALIZATION *)feature_data;
    if (feature_data_size < sizeof(*s))
      return E_INVALIDARG;
    s->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS5: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS5 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->SRVOnlyTiledResourceTier3 = FALSE;
    o->RenderPassesTier = D3D12_RENDER_PASS_TIER_1;
    o->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
    TRACE("  OPTIONS5: SRVTiled3=%d RenderPassesTier=%u RayTier=%u",
          o->SRVOnlyTiledResourceTier3, o->RenderPassesTier, o->RaytracingTier);
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS6: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS6 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->AdditionalShadingRatesSupported = TRUE;
    o->PerPrimitiveShadingRateSupportedWithViewportIndexing = TRUE;
    o->VariableShadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_2;
    o->ShadingRateImageTileSize = kD3D12ShadingRateImageTileSize;
    o->BackgroundProcessingSupported = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS7: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS7 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
    o->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_0_9;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS8: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS8 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->UnalignedBlockTexturesSupported = TRUE;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS9: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS9 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    // PIPELINE_STATISTICS1 is software-accounted from successful AS/MS
    // dispatches and has an exact two-group runtime/readback proof. This is
    // independent of the still-conservative MeshShaderTier report.
    o->MeshShaderPipelineStatsSupported = TRUE;
    o->MeshShaderSupportsFullRangeRenderTargetArrayIndex = FALSE;
    o->AtomicInt64OnTypedResourceSupported = TRUE;
    o->AtomicInt64OnGroupSharedSupported = TRUE;
    o->DerivativesInMeshAndAmplificationShadersSupported = FALSE;
    TRACE("  OPTIONS9: MeshStats=%d FullRTArray=%d Atomic64Typed=%d "
          "Atomic64GroupShared=%d",
          o->MeshShaderPipelineStatsSupported,
          o->MeshShaderSupportsFullRangeRenderTargetArrayIndex,
          o->AtomicInt64OnTypedResourceSupported,
          o->AtomicInt64OnGroupSharedSupported);
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS10: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS10 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    // SUM is implemented axis-wise and is covered by the constant-image
    // SUM/SUM readback.  This independent cap does not promote the overall
    // VariableShadingRateTier.
    o->VariableRateShadingSumCombinerSupported = TRUE;
    o->MeshShaderPerPrimitiveShadingRateSupported = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS11: {
    auto *o = (D3D12_FEATURE_DATA_D3D12_OPTIONS11 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->AtomicInt64OnDescriptorHeapResourceSupported = TRUE;
    TRACE("  OPTIONS11: Atomic64DescriptorHeap=%d",
          o->AtomicInt64OnDescriptorHeapResourceSupported);
    return S_OK;
  }
  case 41: { // D3D12_FEATURE_D3D12_OPTIONS12
    auto *o = (D3D12FeatureOptions12 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = 0;
    o->EnhancedBarriersSupported = TRUE;
    o->RelaxedFormatCastingSupported = TRUE;
    TRACE("  OPTIONS12: EnhancedBarriers=%d RelaxedFormatCasting=%d",
          o->EnhancedBarriersSupported, o->RelaxedFormatCastingSupported);
    return S_OK;
  }
  case 42: { // D3D12_FEATURE_D3D12_OPTIONS13
    auto *o = (D3D12FeatureOptions13 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    memset(o, 0, sizeof(*o));
    TRACE("  OPTIONS13: conservative unsupported");
    return S_OK;
  }
  case 43: { // D3D12_FEATURE_D3D12_OPTIONS14
    auto *o = (D3D12FeatureOptions14 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    memset(o, 0, sizeof(*o));
    o->AdvancedTextureOpsSupported = TRUE;
    o->WriteableMSAATexturesSupported = TRUE;
    TRACE("  OPTIONS14: AdvancedTextureOps=%d WriteableMSAA=%d",
          o->AdvancedTextureOpsSupported,
          o->WriteableMSAATexturesSupported);
    return S_OK;
  }
  case 44: { // D3D12_FEATURE_D3D12_OPTIONS15
    auto *o = (D3D12FeatureOptions15 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->TriangleFanSupported = FALSE;
    o->DynamicIndexBufferStripCutSupported = FALSE;
    TRACE("  OPTIONS15: TriangleFan=%d DynamicStripCut=%d",
          o->TriangleFanSupported, o->DynamicIndexBufferStripCutSupported);
    return S_OK;
  }
  case 45: { // D3D12_FEATURE_D3D12_OPTIONS16
    auto *o = (D3D12FeatureOptions16 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->DynamicDepthBiasSupported = FALSE;
    o->GPUUploadHeapSupported = FALSE;
    TRACE("  OPTIONS16: DynamicDepthBias=%d GPUUploadHeap=%d",
          o->DynamicDepthBiasSupported, o->GPUUploadHeapSupported);
    return S_OK;
  }
  case 46: { // D3D12_FEATURE_D3D12_OPTIONS17
    auto *o = (D3D12FeatureOptions17 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    memset(o, 0, sizeof(*o));
    TRACE("  OPTIONS17: conservative unsupported");
    return S_OK;
  }
  case 47: { // D3D12_FEATURE_D3D12_OPTIONS18
    auto *o = (D3D12FeatureOptions18 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->RenderPassesValid = TRUE;
    TRACE("  OPTIONS18: RenderPassesValid=%d", o->RenderPassesValid);
    return S_OK;
  }
  case 48: { // D3D12_FEATURE_D3D12_OPTIONS19
    auto *o = (D3D12FeatureOptions19 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    memset(o, 0, sizeof(*o));
    o->RasterizerDesc2Supported = TRUE;
    o->MaxSamplerDescriptorHeapSize = 2048;
    o->MaxSamplerDescriptorHeapSizeWithStaticSamplers = 2048;
    o->MaxViewDescriptorHeapSize = 1000000;
    TRACE("  OPTIONS19: RasterizerDesc2=%d MaxSamplerHeap=%u MaxViewHeap=%u",
          o->RasterizerDesc2Supported, o->MaxSamplerDescriptorHeapSize,
          o->MaxViewDescriptorHeapSize);
    return S_OK;
  }
  case 49: { // D3D12_FEATURE_D3D12_OPTIONS20
    auto *o = (D3D12FeatureOptions20 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->ComputeOnlyWriteWatchSupported = FALSE;
    o->RecreateAtTier = 0;
    TRACE("  OPTIONS20: RecreateAtTier=%u", o->RecreateAtTier);
    return S_OK;
  }
  case 50: { // D3D12_FEATURE_PREDICATION
    auto *p = (D3D12FeatureBoolSupport *)feature_data;
    if (feature_data_size < sizeof(*p))
      return E_INVALIDARG;
    p->Supported = TRUE;
    TRACE("  PREDICATION: Supported=%d", p->Supported);
    return S_OK;
  }
  case 51: { // D3D12_FEATURE_PLACED_RESOURCE_SUPPORT_INFO
    auto *p = (D3D12FeaturePlacedResourceSupportInfo *)feature_data;
    if (feature_data_size < sizeof(*p))
      return E_INVALIDARG;
    p->Supported = FALSE;
    if (p->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      p->Supported = TRUE;
    } else {
      MTL_DXGI_FORMAT_DESC metal_format;
      p->Supported = SUCCEEDED(
          MTLQueryDXGIFormat(GetMTLDevice(), p->Format, metal_format));
    }
    TRACE("  PLACED_RESOURCE_SUPPORT: format=%u dimension=%u supported=%d",
          (unsigned)p->Format, (unsigned)p->Dimension, p->Supported);
    return S_OK;
  }
  case 52: { // D3D12_FEATURE_HARDWARE_COPY
    auto *p = (D3D12FeatureBoolSupport *)feature_data;
    if (feature_data_size < sizeof(*p))
      return E_INVALIDARG;
    p->Supported = FALSE;
    TRACE("  HARDWARE_COPY: Supported=%d", p->Supported);
    return S_OK;
  }
  case 53: { // D3D12_FEATURE_D3D12_OPTIONS21
    auto *o = (D3D12FeatureOptions21 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->WorkGraphsTier = 0;
    o->ExecuteIndirectTier = 10;
    o->SampleCmpGradientAndBiasSupported = FALSE;
    o->ExtendedCommandInfoSupported = FALSE;
    TRACE("  OPTIONS21: WorkGraphsTier=%u ExecuteIndirectTier=%u",
          o->WorkGraphsTier, o->ExecuteIndirectTier);
    return S_OK;
  }
  case 54: { // D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT
    auto *t = (D3D12FeatureTightAlignment *)feature_data;
    if (feature_data_size < sizeof(*t))
      return E_INVALIDARG;
    t->SupportTier = 1;
    TRACE("  TIGHT_ALIGNMENT: SupportTier=%u", t->SupportTier);
    return S_OK;
  }
  case 56: { // D3D12_FEATURE_APPLICATION_SPECIFIC_DRIVER_STATE
    auto *p = (D3D12FeatureBoolSupport *)feature_data;
    if (feature_data_size < sizeof(*p))
      return E_INVALIDARG;
    p->Supported = FALSE;
    TRACE("  APPLICATION_SPECIFIC_DRIVER_STATE: Supported=%d", p->Supported);
    return S_OK;
  }
  case 57: { // D3D12_FEATURE_BYTECODE_BYPASS_HASH_SUPPORTED
    auto *p = (D3D12FeatureBoolSupport *)feature_data;
    if (feature_data_size < sizeof(*p))
      return E_INVALIDARG;
    p->Supported = FALSE;
    TRACE("  BYTECODE_BYPASS_HASH: Supported=%d", p->Supported);
    return S_OK;
  }
  case 59: { // D3D12_FEATURE_FENCE_BARRIERS
    auto *f = (D3D12FeatureFenceBarriers *)feature_data;
    if (feature_data_size < sizeof(*f))
      return E_INVALIDARG;
    f->FenceBarriersTier = 0;
    TRACE("  FENCE_BARRIERS: type=%u tier=%u", (unsigned)f->CommandListType,
          f->FenceBarriersTier);
    return S_OK;
  }
  case 60: { // D3D12_FEATURE_HARDWARE_SCHEDULING_QUEUE_GROUPINGS
    auto *g = (D3D12FeatureHardwareSchedulingQueueGroupings *)feature_data;
    if (feature_data_size < sizeof(*g))
      return E_INVALIDARG;
    g->ComputeQueuesPer3DQueue = 0;
    TRACE("  HARDWARE_SCHEDULING_QUEUE_GROUPINGS: ComputeQueuesPer3DQueue=%u",
          g->ComputeQueuesPer3DQueue);
    return S_OK;
  }
  case 61: { // D3D12_FEATURE_SHADER_CACHE_ABI_SUPPORT
    auto *s = (D3D12FeatureShaderCacheAbiSupport *)feature_data;
    if (feature_data_size < sizeof(*s))
      return E_INVALIDARG;
    memset(s, 0, sizeof(*s));
    TRACE("  SHADER_CACHE_ABI_SUPPORT: unsupported");
    return S_OK;
  }
  case 63: { // D3D12_FEATURE_ASYNC_COMMANDS
    auto *p = (D3D12FeatureBoolSupport *)feature_data;
    if (feature_data_size < sizeof(*p))
      return E_INVALIDARG;
    p->Supported = FALSE;
    TRACE("  ASYNC_COMMANDS: Supported=%d", p->Supported);
    return S_OK;
  }
  case 64: { // D3D12_FEATURE_BARRIER_LAYOUT
    auto *b = (D3D12FeatureBarrierLayout *)feature_data;
    if (feature_data_size < sizeof(*b))
      return E_INVALIDARG;
    b->Supported = FALSE;
    TRACE("  BARRIER_LAYOUT: type=%u layout=%u supported=%d",
          (unsigned)b->CommandListType, b->Layout, b->Supported);
    return S_OK;
  }
  case 65: { // D3D12_FEATURE_D3D12_OPTIONS22
    auto *o = (D3D12FeatureOptions22 *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->ShaderExecutionReorderingActuallyReorders = FALSE;
    o->CreateByteOffsetViewsSupported = FALSE;
    o->Max1DDispatchSize = 65535;
    o->Max1DDispatchMeshSize = 0;
    TRACE("  OPTIONS22: SER=%d ByteOffsetViews=%d MaxDispatch=%u MaxMesh=%u",
          o->ShaderExecutionReorderingActuallyReorders,
          o->CreateByteOffsetViewsSupported, o->Max1DDispatchSize,
          o->Max1DDispatchMeshSize);
    return S_OK;
  }
  case 68: { // D3D12_FEATURE_D3D12_OPTIONS_MLIR
    auto *m = (D3D12FeatureOptionsMlir *)feature_data;
    if (feature_data_size < sizeof(*m))
      return E_INVALIDARG;
    m->MlirProgramsTier = 0;
    TRACE("  OPTIONS_MLIR: MlirProgramsTier=%u", m->MlirProgramsTier);
    return S_OK;
  }
  case 69: { // D3D12_FEATURE_MLIR_EXCHANGE
    auto *m = (D3D12FeatureMlirExchange *)feature_data;
    if (feature_data_size < sizeof(*m))
      return E_INVALIDARG;
    if (m->OutputDataSizeInBytes)
      *m->OutputDataSizeInBytes = 0;
    TRACE("  MLIR_EXCHANGE: unsupported input=%zu output_size_ptr=%p",
          (size_t)m->InputDataSizeInBytes, (void *)m->OutputDataSizeInBytes);
    return S_OK;
  }
  case 70: { // D3D12_FEATURE_MLIR_INTERFACE_SUPPORT
    auto *m = (D3D12FeatureMlirInterfaceSupport *)feature_data;
    if (feature_data_size < sizeof(*m))
      return E_INVALIDARG;
    if (m->MlirInterfacesSupported) {
      for (UINT i = 0; i < m->NumMlirInterfaces; i++)
        m->MlirInterfacesSupported[i] = FALSE;
    }
    TRACE("  MLIR_INTERFACE_SUPPORT: count=%u", m->NumMlirInterfaces);
    return S_OK;
  }
  case 72: { // D3D12_FEATURE_D3D12_OPTIONS_PREVIEW
    auto *o = (D3D12FeatureOptionsPreview *)feature_data;
    if (feature_data_size < sizeof(*o))
      return E_INVALIDARG;
    o->MaxGroupSharedMemoryPerGroupCS = 32768;
    o->MaxGroupSharedMemoryPerGroupAS = 0;
    o->MaxGroupSharedMemoryPerGroupMS = 0;
    TRACE("  OPTIONS_PREVIEW: MaxGroupSharedCS=%u AS=%u MS=%u",
          o->MaxGroupSharedMemoryPerGroupCS, o->MaxGroupSharedMemoryPerGroupAS,
          o->MaxGroupSharedMemoryPerGroupMS);
    return S_OK;
  }
  case 77: { // D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT
    auto *l = (D3D12FeatureLinearAlgebraSupport *)feature_data;
    if (feature_data_size < sizeof(*l))
      return E_INVALIDARG;
    l->LinearAlgebraTier = 0;
    TRACE("  LINEAR_ALGEBRA_SUPPORT: tier=%u", l->LinearAlgebraTier);
    return S_OK;
  }
  case 78: { // D3D12_FEATURE_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT
    if (!feature_data || feature_data_size < sizeof(UINT))
      return E_INVALIDARG;
    UINT operation_type = *reinterpret_cast<UINT *>(feature_data);
    memset(feature_data, 0, feature_data_size);
    *reinterpret_cast<UINT *>(feature_data) = operation_type;
    TRACE("  LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT: op=%u unsupported",
          operation_type);
    return S_OK;
  }
  case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT: {
    auto *p =
        (D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT *)feature_data;
    if (feature_data_size < sizeof(*p))
      return E_INVALIDARG;
    p->Support = D3D12_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_NONE;
    return S_OK;
  }
  default:
    TRACE("CheckFeatureSupport UNHANDLED feature=%u size=%u -> E_INVALIDARG",
          feature, feature_data_size);
    memset(feature_data, 0, feature_data_size);
    return E_INVALIDARG;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                     REFIID riid, void **descriptor_heap) {
  if (!desc || !descriptor_heap)
    return E_POINTER;
  CheckVtable("CreateDescriptorHeap");
  TRACE("CreateDescriptorHeap type=%u num=%u starting", desc->Type,
        desc->NumDescriptors);
  InitReturnPtr(descriptor_heap);

  TRACE("CreateDescriptorHeap: about to allocate %u bytes for object",
        (unsigned)sizeof(MTLD3D12DescriptorHeap));
  void *raw = HeapAlloc(GetProcessHeap(), 0, sizeof(MTLD3D12DescriptorHeap));
  TRACE("CreateDescriptorHeap: HeapAlloc returned %p", raw);
  if (!raw) {
    TRACE("CreateDescriptorHeap: HeapAlloc for object FAILED");
    return E_OUTOFMEMORY;
  }
  TRACE("CreateDescriptorHeap: about to placement-new, sizeof=%u",
        (unsigned)sizeof(MTLD3D12DescriptorHeap));
  MTLD3D12DescriptorHeap *heap = new (raw) MTLD3D12DescriptorHeap(this, *desc);
  TRACE("CreateDescriptorHeap: heap=%p data=%p", (void *)heap,
        heap->GetDescriptors());
  if (!heap->HasShaderVisibleMirror()) {
    TRACE("CreateDescriptorHeap: shader-visible mirror allocation failed");
    heap->Release();
    return E_OUTOFMEMORY;
  }
  HRESULT hr = heap->QueryInterface(riid, descriptor_heap);
  heap->Release();
  return hr;
}

UINT STDMETHODCALLTYPE MTLD3D12Device::GetDescriptorHandleIncrementSize(
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) {
  TRACE("GetDescriptorHandleIncrementSize type=%u -> %zu", descriptor_heap_type,
        sizeof(D3D12Descriptor));
  return sizeof(D3D12Descriptor);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateRootSignature(
    UINT node_mask, const void *bytecode, SIZE_T bytecode_length, REFIID riid,
    void **root_signature) {
  TRACE("CreateRootSignature len=%llu", (unsigned long long)bytecode_length);
  if (!bytecode || !root_signature)
    return E_POINTER;
  InitReturnPtr(root_signature);

  auto rs = new MTLD3D12RootSignature(this, bytecode, bytecode_length);
  HRESULT hr = rs->QueryInterface(riid, root_signature);
  rs->Release();
  TRACE("CreateRootSignature DONE hr=0x%lx rs=%p out=%p", hr, (void *)rs,
        root_signature ? *root_signature : nullptr);
  return hr;
}

void STDMETHODCALLTYPE MTLD3D12Device::CreateConstantBufferView(
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  TRACE("CreateConstantBufferView");
  CheckVtable("CreateConstantBufferView");
  if (!desc)
    return;
  auto *d = reinterpret_cast<D3D12Descriptor *>(descriptor.ptr);
  if (d) {
    d->resource = nullptr;
    d->resource_uav_counter = nullptr;
    d->is_sampler_feedback = false;
    d->sampler_feedback_target = nullptr;
    d->metal_texture_view = {};
    d->metal_texture_gpu_id = 0;
    d->cbv = *desc;
    d->type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d->range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    UpdateDescriptorTableMirror(this, d);
  }
}

void STDMETHODCALLTYPE MTLD3D12Device::CreateShaderResourceView(
    ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  TRACE("CreateShaderResourceView res=%p handle=0x%llx device=%p",
        (void *)resource, (unsigned long long)descriptor.ptr, (void *)this);
  if ((void *)resource == (void *)this) {
    TRACE("!!! LEAK DETECTED: CreateShaderResourceView called with device "
          "pointer as resource!");
  }
  CheckVtable("CreateShaderResourceView");
  auto *d = reinterpret_cast<D3D12Descriptor *>(descriptor.ptr);
  if (d) {
    d->is_sampler_feedback = false;
    d->sampler_feedback_target = nullptr;
    if (!resource && desc &&
        desc->ViewDimension ==
            D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
      resource = LookupResourceByGPUAddress(
          desc->RaytracingAccelerationStructure.Location);
      TRACE("CreateShaderResourceView resolved acceleration structure "
            "location=0x%llx resource=%p",
            (unsigned long long)
                desc->RaytracingAccelerationStructure.Location,
            (void *)resource);
    }
    d->resource = resource;
    d->metal_texture_view = {};
    d->metal_texture_gpu_id = 0;
    if (desc) {
      d->srv = *desc;
      auto *dxmt_res = static_cast<MTLD3D12Resource *>(resource);
      if (dxmt_res && !dxmt_res->IsBuffer()) {
        D3D12_RESOURCE_DESC resource_desc = {};
        dxmt_res->GetDesc(&resource_desc);
        uint16_t mip_start = 0, mip_count = 1, slice_start = 0, slice_count = 1;
        SrvViewRange(*desc, resource_desc, mip_start, mip_count, slice_start,
                     slice_count);
        if (!dxmt_res->IsViewFormatAllowed(desc->Format)) {
          d->resource = nullptr;
        } else if (resource_desc.Format == DXGI_FORMAT_D32_FLOAT &&
                   desc->Format == DXGI_FORMAT_R32_FLOAT) {
          // Metal comparison sampling requires the underlying Depth32 texture,
          // not an R32Float color view of the same storage.
          d->metal_texture_gpu_id = 0;
        } else {
          CreateDescriptorTextureView(
              d, dxmt_res, desc->Format,
              TextureTypeForSrvView(*desc, resource_desc), mip_start,
              mip_count, slice_start, slice_count);
        }
      }
    }
    d->type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d->range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    UpdateDescriptorTableMirror(this, d);
  }
}

void STDMETHODCALLTYPE MTLD3D12Device::CreateUnorderedAccessView(
    ID3D12Resource *resource, ID3D12Resource *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  TRACE("CreateUnorderedAccessView res=%p counter=%p handle=0x%llx device=%p",
        (void *)resource, (void *)counter_resource,
        (unsigned long long)descriptor.ptr, (void *)this);
  if ((void *)resource == (void *)this ||
      (void *)counter_resource == (void *)this) {
    TRACE("!!! LEAK DETECTED: CreateUnorderedAccessView called with device "
          "pointer as resource!");
  }
  CheckVtable("CreateUnorderedAccessView");
  auto *d = reinterpret_cast<D3D12Descriptor *>(descriptor.ptr);
  if (d) {
    d->is_sampler_feedback = false;
    d->sampler_feedback_target = nullptr;
    d->resource = resource;
    d->resource_uav_counter = counter_resource;
    d->metal_texture_view = {};
    d->metal_texture_gpu_id = 0;
    if (desc) {
      d->uav = *desc;
      auto *dxmt_res = static_cast<MTLD3D12Resource *>(resource);
      if (dxmt_res && !dxmt_res->IsBuffer()) {
        D3D12_RESOURCE_DESC resource_desc = {};
        dxmt_res->GetDesc(&resource_desc);
        uint16_t mip_start = 0, mip_count = 1, slice_start = 0, slice_count = 1;
        UavViewRange(*desc, resource_desc, mip_start, mip_count, slice_start,
                     slice_count);
        if (!dxmt_res->IsViewFormatAllowed(desc->Format)) {
          d->resource = nullptr;
        } else {
          CreateDescriptorTextureView(
              d, dxmt_res, desc->Format,
              TextureTypeForUavView(*desc, resource_desc), mip_start,
              mip_count, slice_start, slice_count);
        }
      }
    }
    d->type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d->range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    UpdateDescriptorTableMirror(this, d);
  }
}

void STDMETHODCALLTYPE MTLD3D12Device::CreateRenderTargetView(
    ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  TRACE("CreateRenderTargetView res=%p device=%p", (void *)resource,
        (void *)this);
  if ((void *)resource == (void *)this) {
    TRACE("!!! LEAK DETECTED: CreateRenderTargetView called with device "
          "pointer as resource!");
  }
  auto *d = reinterpret_cast<D3D12Descriptor *>(descriptor.ptr);
  if (d) {
    d->resource = resource;
    auto *dxmt_res = static_cast<MTLD3D12Resource *>(resource);
    if (desc && dxmt_res &&
        !dxmt_res->IsViewFormatAllowed(desc->Format)) {
      TRACE("CreateRenderTargetView rejected undeclared cast res=%p fmt=%u",
            (void *)resource, (unsigned)desc->Format);
      d->resource = nullptr;
    }
    if (desc)
      d->rtv = *desc;
    d->type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    UpdateDescriptorTableMirror(this, d);
    TRACE("CreateRenderTargetView desc=%p res=%p tex=%llu fmt=%u dim=%u",
          (void *)d, (void *)resource,
          dxmt_res ? (unsigned long long)dxmt_res->GetMTLTexture().handle
                   : 0ull,
          desc ? (unsigned)desc->Format : 0u,
          desc ? (unsigned)desc->ViewDimension : 0u);
  }
}

void STDMETHODCALLTYPE MTLD3D12Device::CreateDepthStencilView(
    ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  TRACE("CreateDepthStencilView res=%p device=%p", (void *)resource,
        (void *)this);
  if ((void *)resource == (void *)this) {
    TRACE("!!! LEAK DETECTED: CreateDepthStencilView called with device "
          "pointer as resource!");
  }
  auto *d = reinterpret_cast<D3D12Descriptor *>(descriptor.ptr);
  if (d) {
    d->resource = resource;
    auto *dxmt_res = static_cast<MTLD3D12Resource *>(resource);
    if (desc && dxmt_res &&
        !dxmt_res->IsViewFormatAllowed(desc->Format)) {
      TRACE("CreateDepthStencilView rejected undeclared cast res=%p fmt=%u",
            (void *)resource, (unsigned)desc->Format);
      d->resource = nullptr;
    }
    if (desc)
      d->dsv = *desc;
    d->type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    UpdateDescriptorTableMirror(this, d);
  }
}

void STDMETHODCALLTYPE MTLD3D12Device::CreateSampler(
    const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  auto *d = reinterpret_cast<D3D12Descriptor *>(descriptor.ptr);
  if (d && desc) {
    d->sampler = *desc;
    d->type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    d->range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;

    WMTSamplerInfo info = {};
    switch (desc->Filter) {
    case D3D12_FILTER_MIN_MAG_MIP_POINT:
      info.min_filter = WMTSamplerMinMagFilterNearest;
      info.mag_filter = WMTSamplerMinMagFilterNearest;
      info.mip_filter = WMTSamplerMipFilterNearest;
      break;
    case D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR:
      info.min_filter = WMTSamplerMinMagFilterNearest;
      info.mag_filter = WMTSamplerMinMagFilterNearest;
      info.mip_filter = WMTSamplerMipFilterLinear;
      break;
    case D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
      info.min_filter = WMTSamplerMinMagFilterNearest;
      info.mag_filter = WMTSamplerMinMagFilterLinear;
      info.mip_filter = WMTSamplerMipFilterNearest;
      break;
    case D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR:
      info.min_filter = WMTSamplerMinMagFilterNearest;
      info.mag_filter = WMTSamplerMinMagFilterLinear;
      info.mip_filter = WMTSamplerMipFilterLinear;
      break;
    case D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT:
      info.min_filter = WMTSamplerMinMagFilterLinear;
      info.mag_filter = WMTSamplerMinMagFilterNearest;
      info.mip_filter = WMTSamplerMipFilterNearest;
      break;
    case D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
      info.min_filter = WMTSamplerMinMagFilterLinear;
      info.mag_filter = WMTSamplerMinMagFilterNearest;
      info.mip_filter = WMTSamplerMipFilterLinear;
      break;
    case D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT:
      info.min_filter = WMTSamplerMinMagFilterLinear;
      info.mag_filter = WMTSamplerMinMagFilterLinear;
      info.mip_filter = WMTSamplerMipFilterNearest;
      break;
    case D3D12_FILTER_MIN_MAG_MIP_LINEAR:
      info.min_filter = WMTSamplerMinMagFilterLinear;
      info.mag_filter = WMTSamplerMinMagFilterLinear;
      info.mip_filter = WMTSamplerMipFilterLinear;
      break;
    case D3D12_FILTER_ANISOTROPIC:
      info.min_filter = WMTSamplerMinMagFilterLinear;
      info.mag_filter = WMTSamplerMinMagFilterLinear;
      info.mip_filter = WMTSamplerMipFilterLinear;
      info.max_anisotroy = desc->MaxAnisotropy;
      break;
    default:
      info.min_filter = WMTSamplerMinMagFilterLinear;
      info.mag_filter = WMTSamplerMinMagFilterLinear;
      info.mip_filter = WMTSamplerMipFilterLinear;
      break;
    }

    auto map_addr =
        [](D3D12_TEXTURE_ADDRESS_MODE mode) -> WMTSamplerAddressMode {
      switch (mode) {
      case D3D12_TEXTURE_ADDRESS_MODE_WRAP:
        return WMTSamplerAddressModeRepeat;
      case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
        return WMTSamplerAddressModeMirrorRepeat;
      case D3D12_TEXTURE_ADDRESS_MODE_CLAMP:
        return WMTSamplerAddressModeClampToEdge;
      case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
        return WMTSamplerAddressModeClampToBorderColor;
      case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE:
        return WMTSamplerAddressModeMirrorClampToEdge;
      default:
        return WMTSamplerAddressModeClampToEdge;
      }
    };
    info.s_address_mode = map_addr(desc->AddressU);
    info.t_address_mode = map_addr(desc->AddressV);
    info.r_address_mode = map_addr(desc->AddressW);
    info.lod_min_clamp = desc->MinLOD;
    info.lod_max_clamp = desc->MaxLOD;
    info.normalized_coords = true;
    info.support_argument_buffers = true;
    if (D3D12_DECODE_IS_COMPARISON_FILTER(desc->Filter)) {
      if (desc->ComparisonFunc >= D3D12_COMPARISON_FUNC_LESS &&
          desc->ComparisonFunc <= D3D12_COMPARISON_FUNC_ALWAYS) {
        info.compare_function = (WMTCompareFunction)(desc->ComparisonFunc - 1);
      }
    }

    d->metal_sampler = GetMTLDevice().newSamplerState(info);
    d->metal_sampler_gpu_id = info.gpu_resource_id;

    WMTSamplerInfo cube_info = info;
    if (cube_info.min_filter == WMTSamplerMinMagFilterLinear &&
        cube_info.mag_filter == WMTSamplerMinMagFilterLinear) {
      cube_info.s_address_mode = WMTSamplerAddressModeClampToBorderColor;
      cube_info.t_address_mode = WMTSamplerAddressModeClampToBorderColor;
      cube_info.r_address_mode = WMTSamplerAddressModeClampToBorderColor;
    } else {
      cube_info.s_address_mode = WMTSamplerAddressModeClampToEdge;
      cube_info.t_address_mode = WMTSamplerAddressModeClampToEdge;
      cube_info.r_address_mode = WMTSamplerAddressModeClampToEdge;
    }
    d->metal_sampler_cube = GetMTLDevice().newSamplerState(cube_info);
    d->metal_sampler_cube_gpu_id = cube_info.gpu_resource_id;
    UpdateDescriptorTableMirror(this, d);
  }
}

void STDMETHODCALLTYPE MTLD3D12Device::CopyDescriptors(
    UINT dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
    const UINT *dst_descriptor_range_sizes, UINT src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
    const UINT *src_descriptor_range_sizes,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) {
  UINT descriptor_stride =
      GetDescriptorHandleIncrementSize(descriptor_heap_type) /
      sizeof(D3D12Descriptor);
  if (descriptor_stride == 0)
    descriptor_stride = 1;

  auto descriptor_at = [descriptor_stride](D3D12_CPU_DESCRIPTOR_HANDLE base,
                                           UINT index) {
    return reinterpret_cast<D3D12Descriptor *>(base.ptr) +
           (index * descriptor_stride);
  };

  UINT src_range = 0;
  UINT src_offset = 0;
  for (UINT dst_range = 0; dst_range < dst_descriptor_range_count;
       dst_range++) {
    UINT dst_count =
        dst_descriptor_range_sizes ? dst_descriptor_range_sizes[dst_range] : 1;
    for (UINT dst_offset = 0; dst_offset < dst_count; dst_offset++) {
      while (src_range < src_descriptor_range_count) {
        UINT src_count = src_descriptor_range_sizes
                             ? src_descriptor_range_sizes[src_range]
                             : 1;
        if (src_offset < src_count)
          break;
        src_range++;
        src_offset = 0;
      }

      if (src_range >= src_descriptor_range_count) {
        TRACE("CopyDescriptors: source ranges exhausted at dst_range=%u "
              "dst_offset=%u",
              dst_range, dst_offset);
        return;
      }

      auto *dst =
          descriptor_at(dst_descriptor_range_offsets[dst_range], dst_offset);
      auto *src =
          descriptor_at(src_descriptor_range_offsets[src_range], src_offset);
      if (src->resource && (void *)src->resource == (void *)this) {
        TRACE("!!! CopyDescriptors: src descriptor at %p has device pointer as "
              "resource! copying to dst %p",
              (void *)src, (void *)dst);
      }
      auto *dst_owner = dst->owner;
      *dst = *src;
      dst->owner = dst_owner;
      UpdateDescriptorTableMirror(this, dst);
      src_offset++;
    }
  }
}

void STDMETHODCALLTYPE MTLD3D12Device::CopyDescriptorsSimple(
    UINT descriptor_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
    const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) {
  TRACE("CopyDescriptorsSimple count=%u dst=%p src=%p type=%u",
        descriptor_count, (void *)dst_descriptor_range_offset.ptr,
        (void *)src_descriptor_range_offset.ptr, descriptor_heap_type);
  CopyDescriptors(1, &dst_descriptor_range_offset, &descriptor_count, 1,
                  &src_descriptor_range_offset, &descriptor_count,
                  descriptor_heap_type);
}

D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
MTLD3D12Device::GetResourceAllocationInfo(
    D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
    UINT resource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) {
  TRACE("GetResourceAllocationInfo visible=0x%x count=%u descs=%p ret=%p",
        visible_mask, resource_desc_count, (void *)resource_descs,
        (void *)__ret);
  if (!__ret)
    return nullptr;

  __ret->SizeInBytes = 0;
  if (!resource_desc_count) {
    __ret->Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return __ret;
  }
  if (!resource_descs) {
    __ret->Alignment = 0;
    return __ret;
  }

  __ret->Alignment = 0;
  UINT64 cursor = 0;
  for (UINT i = 0; i < resource_desc_count; i++) {
    const D3D12_RESOURCE_DESC normalized =
        NormalizeResourceDesc(resource_descs[i]);
    if (!IsValidResourceDesc(normalized)) {
      __ret->Alignment = 0;
      return __ret;
    }
    UINT64 alignment = ResourcePlacementAlignment(normalized);
    UINT64 size = EstimateResourceAllocationSize(normalized);
    __ret->Alignment = std::max<UINT64>(__ret->Alignment, alignment);
    cursor = AlignTo(cursor, alignment);
    TRACE("GetResourceAllocationInfo[%u] dim=%u fmt=%u %llux%u size=%llu "
          "align=%llu offset=%llu",
          i, normalized.Dimension, normalized.Format,
          (unsigned long long)normalized.Width,
          normalized.Height, (unsigned long long)size,
          (unsigned long long)alignment, (unsigned long long)cursor);
    cursor += size;
  }
  __ret->SizeInBytes = AlignTo(cursor, __ret->Alignment);
  TRACE("GetResourceAllocationInfo -> size=%llu align=%llu",
        (unsigned long long)__ret->SizeInBytes,
        (unsigned long long)__ret->Alignment);
  return __ret;
}

static D3D12_RESOURCE_ALLOCATION_INFO *FillResourceAllocationInfoWithSideband(
    D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
    UINT resource_desc_count, const D3D12_RESOURCE_DESC *resource_descs,
    D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) {
  if (!__ret)
    return nullptr;

  __ret->SizeInBytes = 0;
  if (!resource_desc_count) {
    __ret->Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return __ret;
  }
  if (!resource_descs) {
    __ret->Alignment = 0;
    return __ret;
  }

  __ret->Alignment = 0;
  UINT64 cursor = 0;
  for (UINT i = 0; i < resource_desc_count; i++) {
    const D3D12_RESOURCE_DESC normalized =
        NormalizeResourceDesc(resource_descs[i]);
    if (!IsValidResourceDesc(normalized)) {
      __ret->Alignment = 0;
      if (resource_allocation_info1)
        resource_allocation_info1[i] = {};
      return __ret;
    }
    UINT64 alignment = ResourcePlacementAlignment(normalized);
    UINT64 size = EstimateResourceAllocationSize(normalized);
    cursor = AlignTo(cursor, alignment);
    if (resource_allocation_info1) {
      resource_allocation_info1[i].Offset = cursor;
      resource_allocation_info1[i].Alignment = alignment;
      resource_allocation_info1[i].SizeInBytes = size;
    }
    __ret->Alignment = std::max<UINT64>(__ret->Alignment, alignment);
    cursor += size;
  }

  __ret->SizeInBytes = AlignTo(cursor, __ret->Alignment);
  TRACE("GetResourceAllocationInfo sideband visible=0x%x count=%u -> size=%llu "
        "align=%llu",
        visible_mask, resource_desc_count,
        (unsigned long long)__ret->SizeInBytes,
        (unsigned long long)__ret->Alignment);
  return __ret;
}

D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
MTLD3D12Device::GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret,
                                        UINT node_mask,
                                        D3D12_HEAP_TYPE heap_type) {
  TRACE("GetCustomHeapProperties node=0x%x heap_type=%u ret=%p", node_mask,
        heap_type, (void *)__ret);
  if (!__ret)
    return nullptr;
  __ret->Type = heap_type;
  __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  __ret->CreationNodeMask = 1;
  __ret->VisibleNodeMask = 1;
  return __ret;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
    void **resource) {
  TRACE("CreateCommittedResource dim=%u fmt=%u width=%llu state=0x%x "
        "heap_type=%u",
        desc ? desc->Dimension : 0xFF, desc ? desc->Format : 0,
        desc ? desc->Width : 0, initial_state,
        heap_properties ? heap_properties->Type : 0xFF);
  CheckVtable("CreateCommittedResource");
  if (!desc || !resource)
    return E_POINTER;
  InitReturnPtr(resource);
  if (!heap_properties || !IsValidHeapProperties(*heap_properties))
    return E_INVALIDARG;
  D3D12_RESOURCE_DESC normalized_desc = NormalizeResourceDesc(*desc);
  if (!IsValidResourceDesc(normalized_desc) ||
      !IsResourceAllowedByHeapFlags(normalized_desc, heap_flags) ||
      !IsValidInitialResourceState(heap_properties->Type, initial_state) ||
      !IsValidOptimizedClearValue(normalized_desc, optimized_clear_value))
    return E_INVALIDARG;
  desc = &normalized_desc;

  auto res = new MTLD3D12Resource(
      this, *desc, initial_state,
      heap_properties ? *heap_properties : D3D12_HEAP_PROPERTIES{}, heap_flags);
  if (!res->IsValid()) {
    TRACE("CreateCommittedResource unsupported resource backing dim=%u fmt=%u",
          (unsigned)desc->Dimension, (unsigned)desc->Format);
    res->Release();
    return E_INVALIDARG;
  }
  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
      desc->Width >= (64ull << 20)) {
    Logger::info(
        str::format("M12 large committed buffer width=", desc->Width,
                    " heap_type=", heap_properties ? heap_properties->Type : 0,
                    " flags=0x", (unsigned)heap_flags));
  }
  HRESULT hr = res->QueryInterface(riid, resource);
  TRACE("CreateCommittedResource res_obj=%p out=%p hr=0x%lx", (void *)res,
        resource ? *resource : nullptr, hr);
  if (resource && *resource == (void *)this) {
    TRACE("!!! LEAK DETECTED: CreateCommittedResource returned device pointer "
          "%p as resource!",
          (void *)this);
  }
  res->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateHeap(
    const D3D12_HEAP_DESC *desc, REFIID riid, void **heap) {
  TRACE("CreateHeap size=%llu type=%u flags=0x%x",
        desc ? (unsigned long long)desc->SizeInBytes : 0,
        desc ? desc->Properties.Type : 0xFF, desc ? desc->Flags : 0);
  if (!desc || !heap)
    return E_POINTER;
  InitReturnPtr(heap);
  if (!desc->SizeInBytes || !IsValidHeapProperties(desc->Properties) ||
      (desc->Alignment &&
       desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
       desc->Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT) ||
      ((desc->Flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) &&
       (desc->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)))
    return E_INVALIDARG;

  D3D12_HEAP_DESC normalized = *desc;
  if (!normalized.Alignment)
    normalized.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  if (normalized.SizeInBytes >
      UINT64_MAX - (normalized.Alignment - 1))
    return E_INVALIDARG;
  normalized.SizeInBytes = AlignTo(std::max<UINT64>(normalized.SizeInBytes, 1),
                                   normalized.Alignment);

  auto h = new MTLD3D12Heap(this, normalized);
  if (normalized.SizeInBytes >= (64ull << 20)) {
    Logger::info(str::format("M12 large heap size=", normalized.SizeInBytes,
                             " alignment=", normalized.Alignment,
                             " heap_type=", normalized.Properties.Type,
                             " flags=0x", (unsigned)normalized.Flags));
  }
  HRESULT hr = h->QueryInterface(riid, heap);
  TRACE("CreateHeap normalized size=%llu alignment=%llu out=%p hr=0x%lx",
        (unsigned long long)normalized.SizeInBytes,
        (unsigned long long)normalized.Alignment, heap ? *heap : nullptr, hr);
  h->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreatePlacedResource(
    ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
    void **resource) {
  TRACE("CreatePlacedResource heap=%p offset=%llu dim=%u fmt=%u w=%llu",
        (void *)heap, (unsigned long long)heap_offset,
        desc ? desc->Dimension : 0, desc ? desc->Format : 0,
        desc ? desc->Width : 0);
  if (!desc || !resource || !heap)
    return E_POINTER;
  InitReturnPtr(resource);
  D3D12_RESOURCE_DESC normalized_desc = NormalizeResourceDesc(*desc);
  if (!IsValidResourceDesc(normalized_desc) ||
      !IsValidOptimizedClearValue(normalized_desc, optimized_clear_value))
    return E_INVALIDARG;
  desc = &normalized_desc;

  D3D12_HEAP_PROPERTIES heap_props = {};
  D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
  heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
  auto mt_heap = static_cast<MTLD3D12Heap *>(heap);
  if (mt_heap) {
    const auto &heap_desc = mt_heap->GetHeapDesc();
    heap_props = heap_desc.Properties;
    heap_flags = heap_desc.Flags;
    if (!IsResourceAllowedByHeapFlags(*desc, heap_flags))
      return E_INVALIDARG;
    D3D12_RESOURCE_ALLOCATION_INFO info = {};
    GetResourceAllocationInfo(&info, 0, 1, desc);
    if ((desc->Flags & kD3D12ResourceFlagUseTightAlignment) &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        info.Alignment > 256)
      return E_INVALIDARG;
    if (info.Alignment && heap_offset % info.Alignment) {
      TRACE("CreatePlacedResource misaligned offset=%llu align=%llu",
            (unsigned long long)heap_offset,
            (unsigned long long)info.Alignment);
      return E_INVALIDARG;
    }
    if (heap_offset > heap_desc.SizeInBytes ||
        info.SizeInBytes > heap_desc.SizeInBytes - heap_offset) {
      Logger::warn(str::format(
          "CreatePlacedResource out of heap bounds offset=", heap_offset,
          " size=", info.SizeInBytes, " heap_size=", heap_desc.SizeInBytes));
      return E_INVALIDARG;
    }
  }

  if (!IsValidInitialResourceState(heap_props.Type, initial_state))
    return E_INVALIDARG;

  auto heap_buffer =
      mt_heap ? mt_heap->GetMTLBuffer() : WMT::Reference<WMT::Buffer>{};
  WMT::Reference<WMT::Buffer> placed_buffer;
  uint64_t placed_buffer_gpu = 0;
  if (mt_heap && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
      heap_props.Type == D3D12_HEAP_TYPE_DEFAULT) {
    auto placement_heap = mt_heap->GetMTLHeap();
    if (placement_heap.handle) {
      WMTBufferInfo buffer_info = {};
      buffer_info.length = desc->Width;
      buffer_info.options = WMTResourceStorageModePrivate;
      placed_buffer = placement_heap.newBuffer(buffer_info, heap_offset);
      placed_buffer_gpu = buffer_info.gpu_address;
      if (!placed_buffer.handle)
        TRACE("CreatePlacedResource native heap buffer allocation failed");
    }
  }
  bool use_heap_backing =
      placed_buffer.handle ||
      (mt_heap && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
       heap_buffer.handle != NULL_OBJECT_HANDLE);
  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
      desc->Width >= (64ull << 20)) {
    Logger::info(str::format(
        "M12 large placed buffer width=", desc->Width, " heap_offset=",
        heap_offset, " heap_backing=", use_heap_backing ? 1 : 0, " heap_gpu=0x",
        (unsigned long long)(mt_heap ? mt_heap->GetGPUAddress() : 0)));
  }

  WMT::Reference<WMT::Texture> placed_texture;
  uint64_t placed_texture_gpu_id = 0;
  if (mt_heap && desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    auto placement_heap = mt_heap->GetMTLHeap();
    if (!placement_heap.handle)
      return E_NOTIMPL;
    WMTTextureInfo texture_info = {};
    texture_info.width = static_cast<uint32_t>(desc->Width);
    texture_info.height = desc->Height ? desc->Height : 1;
    texture_info.depth =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? desc->DepthOrArraySize
            : 1;
    texture_info.array_length =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
            ? desc->DepthOrArraySize
            : 1;
    texture_info.mipmap_level_count = desc->MipLevels ? desc->MipLevels : 1;
    const UINT samples = desc->SampleDesc.Count ? desc->SampleDesc.Count : 1;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
      texture_info.type = desc->DepthOrArraySize > 1
                              ? WMTTextureType1DArray
                              : WMTTextureType1D;
    else if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      texture_info.type = WMTTextureType3D;
    else if (samples > 1)
      texture_info.type = desc->DepthOrArraySize > 1
                              ? WMTTextureType2DMultisampleArray
                              : WMTTextureType2DMultisample;
    else
      texture_info.type = desc->DepthOrArraySize > 1
                              ? WMTTextureType2DArray
                              : WMTTextureType2D;
    texture_info.sample_count = samples;
    if (samples > 1)
      texture_info.mipmap_level_count = 1;
    texture_info.usage =
        (WMTTextureUsage)(WMTTextureUsageRenderTarget |
                          WMTTextureUsageShaderRead |
                          WMTTextureUsageShaderWrite |
                          WMTTextureUsagePixelFormatView);
    texture_info.options = WMTResourceStorageModePrivate;
    texture_info.pixel_format = MTLD3D12PipelineState::DXGIToMTLPixelFormat(
        static_cast<DXGI_FORMAT>(desc->Format));
    if (texture_info.pixel_format == WMTPixelFormatInvalid)
      return E_INVALIDARG;
    placed_texture = placement_heap.newTexture(texture_info, heap_offset);
    placed_texture_gpu_id = texture_info.gpu_resource_id;
    if (!placed_texture.handle)
      return E_NOTIMPL;
  }
  auto res = placed_buffer.handle
                 ? new MTLD3D12Resource(this, *desc, initial_state, heap_props,
                                        heap_flags, std::move(placed_buffer),
                                        nullptr, placed_buffer_gpu, 0)
                 : use_heap_backing
                       ? new MTLD3D12Resource(
                             this, *desc, initial_state, heap_props, heap_flags,
                             heap_buffer, mt_heap->GetCPUAddress(),
                             mt_heap->GetGPUAddress(), heap_offset)
                       : placed_texture.handle
                       ? new MTLD3D12Resource(
                             this, *desc, initial_state, heap_props, heap_flags,
                             std::move(placed_texture), placed_texture_gpu_id,
                             heap_offset)
                 : new MTLD3D12Resource(this, *desc, initial_state, heap_props,
                                        heap_flags);
  if (mt_heap)
    res->SetParentHeap(mt_heap);
  if (!res->IsValid()) {
    TRACE("CreatePlacedResource unsupported resource backing dim=%u fmt=%u",
          (unsigned)desc->Dimension, (unsigned)desc->Format);
    res->Release();
    return E_INVALIDARG;
  }
  HRESULT hr = res->QueryInterface(riid, resource);
  TRACE("CreatePlacedResource out=%p hr=0x%lx", resource ? *resource : nullptr,
        hr);
  res->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateReservedResource(
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
    void **resource) {
  TRACE("CreateReservedResource dim=%u fmt=%u w=%llu",
        desc ? static_cast<unsigned>(desc->Dimension) : 0,
        desc ? static_cast<unsigned>(desc->Format) : 0, desc ? desc->Width : 0);
  CheckVtable("CreateReservedResource");
  if (!resource)
    return E_POINTER;
  InitReturnPtr(resource);
  if (!desc)
    return E_INVALIDARG;
  D3D12_RESOURCE_DESC normalized_desc = NormalizeResourceDesc(*desc);
  if (!IsValidResourceDesc(normalized_desc))
    return E_INVALIDARG;
  desc = &normalized_desc;
  if (desc->Flags & kD3D12ResourceFlagUseTightAlignment)
    return E_INVALIDARG;
  if (!IsValidOptimizedClearValue(*desc, optimized_clear_value))
    return E_INVALIDARG;
  const bool reserved_buffer =
      desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
      desc->Format == DXGI_FORMAT_UNKNOWN && desc->Width &&
      desc->Width % D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == 0 &&
      desc->Height == 1 && desc->DepthOrArraySize == 1 &&
      desc->MipLevels == 1 && desc->SampleDesc.Count <= 1 &&
      desc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const bool reserved_texture =
      (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
       desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) &&
      desc->MipLevels && desc->MipLevels <= 16 &&
      desc->SampleDesc.Count <= 1 && desc->Width && desc->Height &&
      desc->DepthOrArraySize &&
      MTLD3D12PipelineState::DXGIToMTLPixelFormat(desc->Format) !=
          WMTPixelFormatInvalid;
  if (!reserved_buffer && !reserved_texture) {
    TRACE("CreateReservedResource rejected unsupported sparse shape dim=%u "
          "samples=%u format=%u",
          (unsigned)desc->Dimension, desc->SampleDesc.Count,
          (unsigned)desc->Format);
    return E_NOTIMPL;
  }

  D3D12_HEAP_PROPERTIES heap_properties = {};
  heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  auto res = new MTLD3D12Resource(this, *desc, initial_state,
                                  heap_properties, D3D12_HEAP_FLAG_NONE, true);
  if (!res->IsSparseBacked()) {
    TRACE("CreateReservedResource native sparse texture allocation failed");
    res->Release();
    return E_NOTIMPL;
  }
  HRESULT hr = res->QueryInterface(riid, resource);
  res->Release();
  TRACE("CreateReservedResource sparse texture out=%p hr=0x%lx",
        resource ? *resource : nullptr, hr);
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateSharedHandle(
    ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes,
    DWORD access, const WCHAR *name, HANDLE *handle) {
  (void)attributes;
  (void)access;
  if (!handle)
    return E_POINTER;
  *handle = nullptr;
  if (!object)
    return E_INVALIDARG;
  if (!IsValidSharedHandleAccess(access))
    return E_INVALIDARG;

  std::lock_guard lock(g_shared_handle_mutex);
  if (name && g_named_shared_handles.contains(std::wstring(name)))
    return DXGI_ERROR_NAME_ALREADY_EXISTS;

  // Named buffers use a file mapping with a fixed, pointer-free metadata
  // header so another Wine process can recreate the resource and share its
  // CPU/GPU-visible backing. Keep the legacy event registry for other object
  // kinds until their platform providers are implemented.
  if (name) {
    ID3D12Resource *resource = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
      auto *resource_impl = static_cast<MTLD3D12Resource *>(resource);
      const bool is_buffer = resource_impl->IsBuffer();
      if (is_buffer) {
        HANDLE public_mapping = nullptr;
        HRESULT hr = CreateSharedBufferMapping(resource_impl, name,
                                               &public_mapping);
        resource->Release();
        if (FAILED(hr))
          return hr;
        *handle = public_mapping;
        TRACE("CreateSharedHandle named buffer object=%p name=%ls handle=%p",
              (void *)object, name, public_mapping);
        return S_OK;
      }
      resource->Release();
    }
    ID3D12Heap *heap = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&heap)))) {
      auto *heap_impl = static_cast<MTLD3D12Heap *>(heap);
      const bool shareable_heap = heap_impl->GetCPUAddress() != nullptr;
      if (shareable_heap) {
        HANDLE public_mapping = nullptr;
        HRESULT hr = CreateSharedHeapMapping(heap_impl, name,
                                             &public_mapping);
        heap->Release();
        if (FAILED(hr))
          return hr;
        *handle = public_mapping;
        TRACE("CreateSharedHandle named heap object=%p name=%ls handle=%p",
              (void *)object, name, public_mapping);
        return S_OK;
      }
      heap->Release();
    }
    ID3D12Fence *fence = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&fence)))) {
      HANDLE public_mapping = nullptr;
      HRESULT hr = CreateSharedFenceMapping(
          static_cast<MTLD3D12Fence *>(fence), name, &public_mapping);
      fence->Release();
      if (SUCCEEDED(hr)) {
        *handle = public_mapping;
        TRACE("CreateSharedHandle named fence object=%p name=%ls handle=%p",
              (void *)object, name, public_mapping);
        return S_OK;
      }
      if (hr != E_INVALIDARG)
        return hr;
    }
  }

  HANDLE public_handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!public_handle)
    return HRESULT_FROM_WIN32(GetLastError());
  HANDLE retained_handle = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), public_handle,
                       GetCurrentProcess(), &retained_handle, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(public_handle);
    return hr;
  }

  auto existing = g_shared_handles.find(public_handle);
  if (existing != g_shared_handles.end()) {
    ReleaseSharedHandleEntry(existing->second);
    g_shared_handles.erase(existing);
  }
  object->AddRef();
  g_shared_handles.emplace(
      public_handle, D3D12SharedHandleEntry{object, retained_handle});
  if (name)
    g_named_shared_handles.emplace(std::wstring(name), public_handle);
  *handle = public_handle;
  TRACE("CreateSharedHandle object=%p name=%ls handle=%p", (void *)object,
        name ? name : L"(null)", public_handle);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::OpenSharedHandle(HANDLE handle,
                                                           REFIID riid,
                                                           void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (!handle)
    return E_INVALIDARG;

  // Named resource/heap/fence handles carry self-describing, pointer-free
  // metadata. Reconstruct from the mapping first even in the creating
  // process, so OpenSharedHandle has normal independent-object lifetime
  // rather than returning a stale registry-owned pointer.
  ID3D12Resource *shared_resource = nullptr;
  HRESULT shared_resource_hr =
      OpenSharedBufferFromMapping(this, handle, &shared_resource);
  if (SUCCEEDED(shared_resource_hr) && shared_resource) {
    HRESULT hr = shared_resource->QueryInterface(riid, object);
    shared_resource->Release();
    return hr;
  }
  ID3D12Heap *shared_heap = nullptr;
  HRESULT shared_heap_hr = OpenSharedHeapFromMapping(this, handle, &shared_heap);
  if (SUCCEEDED(shared_heap_hr) && shared_heap) {
    HRESULT hr = shared_heap->QueryInterface(riid, object);
    shared_heap->Release();
    return hr;
  }
  ID3D12Fence *shared_fence = nullptr;
  HRESULT shared_fence_hr =
      OpenSharedFenceFromMapping(this, handle, &shared_fence);
  if (SUCCEEDED(shared_fence_hr) && shared_fence) {
    HRESULT hr = shared_fence->QueryInterface(riid, object);
    shared_fence->Release();
    return hr;
  }

  // Unsupported object kinds retain the legacy in-process registry until a
  // platform-backed provider exists; do not pretend a closed mapping is one.
  std::lock_guard lock(g_shared_handle_mutex);
  auto entry = g_shared_handles.find(handle);
  if (entry == g_shared_handles.end() || !entry->second.object)
    return DXGI_ERROR_INVALID_CALL;
  HRESULT hr = entry->second.object->QueryInterface(riid, object);
  TRACE("OpenSharedHandle handle=%p riid=%s out=%p hr=0x%lx", handle,
        str::format(riid).c_str(), *object, hr);
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::OpenSharedHandleByName(
    const WCHAR *name, DWORD access, HANDLE *handle) {
  if (!handle)
    return E_POINTER;
  *handle = nullptr;
  if (!name || !IsValidSharedHandleAccess(access))
    return E_INVALIDARG;
  const DWORD mapping_access = FileMappingAccessForSharedHandle(access);
  HANDLE opened_mapping =
      OpenFileMappingW(mapping_access, FALSE, name);
  if (opened_mapping) {
    // Validate the mapping without retaining a process-global COM object.
    // The returned handle can subsequently be passed to OpenSharedHandle,
    // which reconstructs a fresh object from the same metadata.
    ID3D12Resource *shared_resource = nullptr;
    HRESULT hr = OpenSharedBufferFromMapping(this, opened_mapping,
                                             &shared_resource);
    ID3D12Heap *shared_heap = nullptr;
    ID3D12Fence *shared_fence = nullptr;
    if (FAILED(hr) || !shared_resource)
      hr = OpenSharedHeapFromMapping(this, opened_mapping, &shared_heap);
    if (FAILED(hr) || (!shared_resource && !shared_heap))
      hr = OpenSharedFenceFromMapping(this, opened_mapping, &shared_fence);
    if (shared_resource)
      shared_resource->Release();
    if (shared_heap)
      shared_heap->Release();
    if (shared_fence)
      shared_fence->Release();
    if (SUCCEEDED(hr)) {
      *handle = opened_mapping;
      TRACE("OpenSharedHandleByName name=%ls handle=%p", name,
            opened_mapping);
      return S_OK;
    }
    CloseHandle(opened_mapping);
  }

  // Unsupported object kinds retain the legacy registry path until their
  // platform-backed provider exists.
  std::lock_guard lock(g_shared_handle_mutex);
  auto named = g_named_shared_handles.find(std::wstring(name));
  if (named == g_named_shared_handles.end())
    return DXGI_ERROR_NOT_FOUND;
  auto entry = g_shared_handles.find(named->second);
  if (entry == g_shared_handles.end() || !entry->second.retained_handle)
    return DXGI_ERROR_INVALID_CALL;
  HANDLE opened = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), entry->second.retained_handle,
                       GetCurrentProcess(), &opened, 0, FALSE,
                       DUPLICATE_SAME_ACCESS))
    return HRESULT_FROM_WIN32(GetLastError());
  HANDLE retained_opened = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), opened, GetCurrentProcess(),
                       &retained_opened, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(opened);
    return hr;
  }
  entry->second.object->AddRef();
  g_shared_handles.emplace(
      opened, D3D12SharedHandleEntry{entry->second.object, retained_opened});
  *handle = opened;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::MakeResident(
    UINT object_count, ID3D12Pageable *const *objects) {
  TRACE("MakeResident count=%u objects=%p", object_count, (void *)objects);
  if (object_count && !objects)
    return E_INVALIDARG;
  std::vector<D3D12ResidencyObjectKind> kinds(object_count,
                                               D3D12ResidencyObjectKind::Invalid);
  for (UINT i = 0; i < object_count; i++) {
    kinds[i] = ClassifyResidencyObject(objects[i]);
    if (kinds[i] == D3D12ResidencyObjectKind::Invalid)
      return E_INVALIDARG;
  }
  for (UINT i = 0; i < object_count; i++) {
    switch (kinds[i]) {
    case D3D12ResidencyObjectKind::Resource:
      static_cast<MTLD3D12Resource *>(objects[i])->MakeResident();
      break;
    case D3D12ResidencyObjectKind::Heap:
      static_cast<MTLD3D12Heap *>(objects[i])->MakeResident();
      break;
    case D3D12ResidencyObjectKind::DescriptorHeap:
      static_cast<MTLD3D12DescriptorHeap *>(objects[i])->MakeResident();
      break;
    case D3D12ResidencyObjectKind::QueryHeap:
      static_cast<MTLD3D12QueryHeap *>(objects[i])->MakeResident();
      break;
    default:
      break;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Device::Evict(UINT object_count, ID3D12Pageable *const *objects) {
  TRACE("Evict count=%u objects=%p", object_count, (void *)objects);
  if (object_count && !objects)
    return E_INVALIDARG;
  std::vector<D3D12ResidencyObjectKind> kinds(object_count,
                                               D3D12ResidencyObjectKind::Invalid);
  for (UINT i = 0; i < object_count; i++) {
    kinds[i] = ClassifyResidencyObject(objects[i]);
    if (kinds[i] == D3D12ResidencyObjectKind::Invalid)
      return E_INVALIDARG;
  }
  for (UINT i = 0; i < object_count; i++) {
    switch (kinds[i]) {
    case D3D12ResidencyObjectKind::Resource:
      static_cast<MTLD3D12Resource *>(objects[i])->Evict();
      break;
    case D3D12ResidencyObjectKind::Heap:
      static_cast<MTLD3D12Heap *>(objects[i])->Evict();
      break;
    case D3D12ResidencyObjectKind::DescriptorHeap:
      static_cast<MTLD3D12DescriptorHeap *>(objects[i])->Evict();
      break;
    case D3D12ResidencyObjectKind::QueryHeap:
      static_cast<MTLD3D12QueryHeap *>(objects[i])->Evict();
      break;
    default:
      break;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateFence(UINT64 initial_value,
                                                      D3D12_FENCE_FLAGS flags,
                                                      REFIID riid,
                                                      void **fence) {
  if (!fence)
    return E_POINTER;
  InitReturnPtr(fence);

  auto f = new MTLD3D12Fence(this, initial_value, flags);
  TRACE("CreateFence init=%llu fence=%p", (unsigned long long)initial_value,
        (void *)f);
  HRESULT hr = f->QueryInterface(riid, fence);
  f->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::GetDeviceRemovedReason() {
  TRACE("GetDeviceRemovedReason -> S_OK");
  return S_OK;
}

void STDMETHODCALLTYPE MTLD3D12Device::GetCopyableFootprints(
    const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource,
    UINT sub_resource_count, UINT64 base_offset,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count,
    UINT64 *row_size, UINT64 *total_bytes) {
  TRACE("GetCopyableFootprints desc=%p first=%u count=%u base=%llu layouts=%p "
        "rows=%p row_size=%p total=%p",
        (void *)desc, first_sub_resource, sub_resource_count,
        (unsigned long long)base_offset, (void *)layouts, (void *)row_count,
        (void *)row_size, (void *)total_bytes);

  D3D12_RESOURCE_DESC normalized_desc = {};
  if (desc) {
    normalized_desc = NormalizeResourceDesc(*desc);
    desc = &normalized_desc;
  }
  if (!desc || !IsValidResourceDesc(*desc)) {
    if (total_bytes)
      *total_bytes = UINT64_MAX;
    return;
  }
  const UINT plane_count = FormatPlaneCount(desc->Format);
  const UINT mip_levels = std::max<UINT>(desc->MipLevels, 1);
  const UINT array_size =
      desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(desc->DepthOrArraySize, 1);
  const uint64_t subresource_count =
      desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
          ? 1
          : uint64_t(mip_levels) * array_size * plane_count;
  if (uint64_t(first_sub_resource) > D3D12_REQ_SUBRESOURCES ||
      uint64_t(sub_resource_count) >
          uint64_t(D3D12_REQ_SUBRESOURCES) - first_sub_resource) {
    if (total_bytes)
      *total_bytes = UINT64_MAX;
    return;
  }
  UINT64 cursor = base_offset;
  UINT64 last_end = base_offset;

  for (UINT i = 0; i < sub_resource_count; i++) {
    UINT subresource = first_sub_resource + i;
    UINT mip = subresource % mip_levels;
    UINT plane_slice =
        (subresource / mip_levels) / std::max<UINT>(array_size, 1);

    UINT64 width = desc->Width;
    UINT64 height = desc->Height;
    UINT depth = desc->DepthOrArraySize;
    DXGI_FORMAT format =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            ? DXGI_FORMAT_UNKNOWN
            : CopyFootprintPlaneFormat(desc->Format, plane_slice);

    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      height = 1;
      depth = 1;
      format = DXGI_FORMAT_UNKNOWN;
    } else {
      width = std::max<UINT64>(1, width >> mip);
      height = std::max<UINT64>(1, height >> mip);
      AdjustCopyFootprintPlaneDimensions(desc->Format, plane_slice, width,
                                          height);
      if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        depth = std::max<UINT>(1, depth >> mip);
      else
        depth = 1;
    }

    UINT bytes_per_texel =
        desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            ? 1
            : FormatBytesPerTexel(format);
    UINT block_size = desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                          ? 1
                          : FormatBlockSize(format);
    UINT64 width_blocks =
        desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            ? width
            : std::max<UINT64>(1, AlignTo(width, block_size) / block_size);
    UINT64 rows =
        desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            ? 1
            : std::max<UINT64>(1, AlignTo(height, block_size) / block_size);
    UINT64 unaligned_row_size = width_blocks * bytes_per_texel;
    UINT64 aligned_row_pitch =
        AlignTo(unaligned_row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    UINT64 subresource_bytes = aligned_row_pitch * rows * depth;
    UINT64 offset = AlignTo(cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    if (layouts) {
      layouts[i].Offset = offset;
      layouts[i].Footprint.Format = format;
      // Footprint dimensions are expressed in texels. Block-compressed
      // formats use block-rounded rows and pitches, but do not expose the
      // padded block dimensions to the caller.
      layouts[i].Footprint.Width = static_cast<UINT>(
          std::min<UINT64>(width, UINT32_MAX));
      layouts[i].Footprint.Height = static_cast<UINT>(
          std::min<UINT64>(height, UINT32_MAX));
      layouts[i].Footprint.Depth = depth;
      layouts[i].Footprint.RowPitch = static_cast<UINT>(aligned_row_pitch);
    }
    if (row_count)
      row_count[i] = static_cast<UINT>(rows);
    if (row_size)
      row_size[i] = unaligned_row_size;

    last_end = offset + subresource_bytes;
    cursor = last_end;
  }

  if (total_bytes)
    *total_bytes = last_end - base_offset;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateQueryHeap(
    const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **heap) {
  TRACE("CreateQueryHeap desc=%p type=%u count=%u node=0x%x heap_out=%p",
        (void *)desc, desc ? desc->Type : 0xFFFFFFFFu, desc ? desc->Count : 0,
        desc ? desc->NodeMask : 0, (void *)heap);
  if (!desc || !heap)
    return E_POINTER;
  InitReturnPtr(heap);

  auto qh = new MTLD3D12QueryHeap(this, *desc);
  HRESULT hr = qh->QueryInterface(riid, heap);
  TRACE("CreateQueryHeap DONE qh=%p out=%p hr=0x%lx", (void *)qh,
        heap ? *heap : nullptr, hr);
  qh->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::SetStablePowerState(WINBOOL enable) {
  TRACE("SetStablePowerState enable=%d", enable);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Device::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                       ID3D12RootSignature *root_signature,
                                       REFIID riid, void **command_signature) {
  if (!command_signature)
    return E_POINTER;
  InitReturnPtr(command_signature);
  TRACE("CreateCommandSignature stride=%u num_args=%u",
        desc ? desc->ByteStride : 0, desc ? desc->NumArgumentDescs : 0);
  if (!desc)
    return E_INVALIDARG;
  auto *obj = new MTLD3D12CommandSignature(this, *desc);
  HRESULT hr = obj->QueryInterface(riid, command_signature);
  if (FAILED(hr))
    delete obj;
  return hr;
}

void STDMETHODCALLTYPE MTLD3D12Device::GetResourceTiling(
    ID3D12Resource *resource, UINT *total_tile_count,
    D3D12_PACKED_MIP_INFO *packed_mip_info,
    D3D12_TILE_SHAPE *standard_tile_shape, UINT *sub_resource_tiling_count,
    UINT first_sub_resource_tiling,
    D3D12_SUBRESOURCE_TILING *sub_resource_tilings) {
  const UINT requested_tiling_count =
      sub_resource_tiling_count ? *sub_resource_tiling_count : 0;
  TRACE("GetResourceTiling res=%p total=%p packed=%p shape=%p count=%p "
        "first=%u tilings=%p",
        (void *)resource, (void *)total_tile_count, (void *)packed_mip_info,
        (void *)standard_tile_shape, (void *)sub_resource_tiling_count,
        first_sub_resource_tiling, (void *)sub_resource_tilings);
  if (total_tile_count)
    *total_tile_count = 0;
  if (packed_mip_info)
    *packed_mip_info = {};
  if (standard_tile_shape)
    *standard_tile_shape = {};
  if (sub_resource_tiling_count)
    *sub_resource_tiling_count = 0;

  auto *reserved = resource ? static_cast<MTLD3D12Resource *>(resource)
                            : nullptr;
  if (!reserved || !reserved->IsReservedResource() ||
      !reserved->IsSparseBacked())
    return;
  D3D12_RESOURCE_DESC desc = {};
  reserved->GetDesc(&desc);
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    const UINT total_tiles = static_cast<UINT>(
        (desc.Width + D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES - 1) /
        D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
    if (total_tile_count)
      *total_tile_count = total_tiles;
    if (packed_mip_info)
      *packed_mip_info = {};
    if (standard_tile_shape)
      *standard_tile_shape = {D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, 1, 1};
    if (sub_resource_tiling_count) {
      const UINT requested = requested_tiling_count;
      if (first_sub_resource_tiling != 0) {
        *sub_resource_tiling_count = 0;
      } else {
        *sub_resource_tiling_count = requested ? std::min(requested, 1u) : 1;
        if (sub_resource_tilings && requested)
          sub_resource_tilings[0] = {total_tiles, 1, 1, 0};
      }
    }
    return;
  }
  const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const bool array_texture =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  if ((!array_texture && !volume) || desc.SampleDesc.Count > 1 ||
      !desc.MipLevels || !desc.DepthOrArraySize)
    return;

  const D3D12_TILE_SHAPE shape = reserved->GetTiledResourceTileShape();
  const UINT mip_levels = desc.MipLevels;
  // A volume's DepthOrArraySize is its texel depth, not an array-slice count.
  const UINT array_size = volume ? 1 : desc.DepthOrArraySize;
  const UINT tiling_count = mip_levels * array_size;
  UINT standard_mip_count = mip_levels;
  // Once any dimension falls below the standard tile shape, the remaining
  // mip chain is represented by D3D12's packed tail. This applies equally to
  // color, integer, depth, compressed, and volume resources.
  if (mip_levels > 1) {
    for (UINT mip = 0; mip < mip_levels; mip++) {
      const UINT width = std::max<UINT>(1, desc.Width >> mip);
      const UINT height = std::max<UINT>(1, desc.Height >> mip);
      const UINT depth = volume
                             ? std::max<UINT>(1, desc.DepthOrArraySize >> mip)
                             : 1;
      if (width < shape.WidthInTexels || height < shape.HeightInTexels ||
          depth < shape.DepthInTexels) {
        standard_mip_count = mip;
        break;
      }
    }
  }
  const UINT packed_mip_count = mip_levels - standard_mip_count;
  const UINT packed_tile_count = packed_mip_count ? 1u : 0u;
  UINT standard_tiles_per_slice = 0;
  for (UINT mip = 0; mip < standard_mip_count; mip++) {
    const UINT width = std::max<UINT>(1, desc.Width >> mip);
    const UINT height = std::max<UINT>(1, desc.Height >> mip);
    const UINT width_tiles =
        (width + shape.WidthInTexels - 1) / shape.WidthInTexels;
    const UINT height_tiles =
        (height + shape.HeightInTexels - 1) / shape.HeightInTexels;
    const UINT depth =
        volume ? std::max<UINT>(1, desc.DepthOrArraySize >> mip) : 1;
    const UINT depth_tiles =
        (depth + shape.DepthInTexels - 1) / shape.DepthInTexels;
    standard_tiles_per_slice += width_tiles * height_tiles * depth_tiles;
  }
  UINT total_tiles = 0;
  for (UINT array_slice = 0; array_slice < array_size; array_slice++) {
    const UINT slice_start =
        array_slice * (standard_tiles_per_slice + packed_tile_count);
    for (UINT mip = 0; mip < mip_levels; mip++) {
      const UINT subresource = array_slice * mip_levels + mip;
      if (sub_resource_tilings && sub_resource_tiling_count &&
          subresource >= first_sub_resource_tiling &&
          subresource < first_sub_resource_tiling +
                            requested_tiling_count) {
        const UINT output_index = subresource - first_sub_resource_tiling;
        if (mip < standard_mip_count) {
          const UINT width = std::max<UINT>(1, desc.Width >> mip);
          const UINT height = std::max<UINT>(1, desc.Height >> mip);
          const UINT width_tiles =
              (width + shape.WidthInTexels - 1) / shape.WidthInTexels;
          const UINT height_tiles =
              (height + shape.HeightInTexels - 1) / shape.HeightInTexels;
          const UINT depth =
              volume ? std::max<UINT>(1, desc.DepthOrArraySize >> mip) : 1;
          const UINT depth_tiles =
              (depth + shape.DepthInTexels - 1) / shape.DepthInTexels;
          UINT prior_tiles = 0;
          for (UINT prior_mip = 0; prior_mip < mip; prior_mip++) {
            const UINT prior_width =
                std::max<UINT>(1, desc.Width >> prior_mip);
            const UINT prior_height =
                std::max<UINT>(1, desc.Height >> prior_mip);
            const UINT prior_depth = volume
                                          ? std::max<UINT>(
                                                1, desc.DepthOrArraySize >>
                                                       prior_mip)
                                          : 1;
            prior_tiles +=
                ((prior_width + shape.WidthInTexels - 1) /
                 shape.WidthInTexels) *
                ((prior_height + shape.HeightInTexels - 1) /
                 shape.HeightInTexels) *
                ((prior_depth + shape.DepthInTexels - 1) /
                 shape.DepthInTexels);
          }
          sub_resource_tilings[output_index] = {
              width_tiles, static_cast<UINT16>(height_tiles),
              static_cast<UINT16>(depth_tiles), slice_start + prior_tiles};
        } else {
          sub_resource_tilings[output_index] = {0, 0, 0,
                                                 D3D12_PACKED_TILE};
        }
      }
    }
    total_tiles += standard_tiles_per_slice + packed_tile_count;
  }
  if (total_tile_count)
    *total_tile_count = total_tiles;
  if (packed_mip_info) {
    packed_mip_info->NumStandardMips =
        static_cast<UINT8>(std::min<UINT>(standard_mip_count, 255));
    packed_mip_info->NumPackedMips =
        static_cast<UINT8>(std::min<UINT>(packed_mip_count, 255));
    packed_mip_info->NumTilesForPackedMips = packed_tile_count;
    packed_mip_info->StartTileIndexInOverallResource =
        packed_mip_count ? standard_tiles_per_slice : 0;
  }
  if (standard_tile_shape)
    *standard_tile_shape = shape;
  if (sub_resource_tiling_count) {
    const UINT available = first_sub_resource_tiling < tiling_count
                               ? tiling_count - first_sub_resource_tiling
                               : 0;
    *sub_resource_tiling_count =
        requested_tiling_count
            ? std::min(requested_tiling_count, available)
            : available;
  }
}

LUID *STDMETHODCALLTYPE MTLD3D12Device::GetAdapterLuid(LUID *__ret) {
  TRACE("GetAdapterLuid ret=%p", (void *)__ret);
  if (!__ret)
    return nullptr;
  *__ret = std::bit_cast<LUID>(__builtin_bswap64(GetMTLDevice().registryID()));
  TRACE("GetAdapterLuid -> %08lx:%08lx", __ret->HighPart, __ret->LowPart);
  return __ret;
}

void MTLD3D12Device::RegisterResource(MTLD3D12Resource *res) {
  if (!res)
    return;
  D3D12_GPU_VIRTUAL_ADDRESS addr = res->GetGPUVirtualAddress();
  if (addr) {
    std::lock_guard<std::mutex> lock(m_resource_mutex);
    m_resources_by_gpu_addr[addr] = res;
  }
}

void MTLD3D12Device::UnregisterResource(MTLD3D12Resource *res) {
  if (!res)
    return;
  D3D12_GPU_VIRTUAL_ADDRESS addr = res->GetGPUVirtualAddress();
  if (addr) {
    std::lock_guard<std::mutex> lock(m_resource_mutex);
    m_resources_by_gpu_addr.erase(addr);
  }
}

MTLD3D12Resource *
MTLD3D12Device::LookupResourceByGPUAddress(D3D12_GPU_VIRTUAL_ADDRESS addr) {
  if (!addr)
    return nullptr;
  std::lock_guard<std::mutex> lock(m_resource_mutex);
  auto it = m_resources_by_gpu_addr.find(addr);
  if (it != m_resources_by_gpu_addr.end())
    return it->second;
  for (auto &[gpu_addr, res] : m_resources_by_gpu_addr) {
    D3D12_RESOURCE_DESC desc = {};
    res->GetDesc(&desc);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (addr >= gpu_addr && addr < gpu_addr + desc.Width)
        return res;
    }
  }
  return nullptr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreatePipelineLibrary(
    const void *blob, SIZE_T blob_size, REFIID riid, void **lib) {
  TRACE("CreatePipelineLibrary blob=%p size=%zu riid=%s", blob, blob_size,
        str::format(riid).c_str());
  if (!lib)
    return E_POINTER;
  *lib = nullptr;

  auto pipeline_library = new MTLD3D12PipelineLibrary(this, blob, blob_size);
  HRESULT hr = pipeline_library->QueryInterface(riid, lib);
  if (FAILED(hr))
    delete pipeline_library;
  TRACE("CreatePipelineLibrary -> 0x%lx lib=%p", hr, lib ? *lib : nullptr);
  return hr;
}

namespace {

struct MultiFenceWait {
  ID3D12Fence *fence;
  UINT64 value;
};

struct MultiFenceWaitCtx {
  std::vector<MultiFenceWait> waits;
  HANDLE event;
};

DWORD WINAPI MultiFenceWaitThread(void *arg) {
  auto *ctx = static_cast<MultiFenceWaitCtx *>(arg);
  for (auto &wait : ctx->waits) {
    wait.fence->SetEventOnCompletion(wait.value, nullptr);
  }
  for (auto &wait : ctx->waits) {
    wait.fence->Release();
  }
  SetEvent(ctx->event);
  delete ctx;
  return 0;
}

} // namespace

HRESULT STDMETHODCALLTYPE MTLD3D12Device::SetEventOnMultipleFenceCompletion(
    ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count,
    D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) {
  TRACE("SetEventOnMultipleFenceCompletion count=%u flags=0x%x event=%p",
        fence_count, flags, (void *)(uintptr_t)event);
  if (!fences || !values || !event)
    return E_POINTER;
  if (!fence_count) {
    SetEvent(event);
    return S_OK;
  }

  bool all_signaled = true;
  for (UINT i = 0; i < fence_count; i++) {
    if (fences[i]->GetCompletedValue() < values[i]) {
      all_signaled = false;
      break;
    }
  }

  if (all_signaled) {
    SetEvent(event);
    return S_OK;
  }

  if (flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL) {
    auto *ctx = new MultiFenceWaitCtx{};
    ctx->event = event;
    ctx->waits.reserve(fence_count);
    for (UINT i = 0; i < fence_count; i++) {
      if (!fences[i]) {
        delete ctx;
        return E_POINTER;
      }
      fences[i]->AddRef();
      ctx->waits.push_back({fences[i], values[i]});
    }
    HANDLE thread =
        CreateThread(nullptr, 0, MultiFenceWaitThread, ctx, 0, nullptr);
    if (!thread) {
      for (auto &wait : ctx->waits)
        wait.fence->Release();
      delete ctx;
      return E_FAIL;
    }
    CloseHandle(thread);
  } else {
    for (UINT i = 0; i < fence_count; i++) {
      if (fences[i]->GetCompletedValue() < values[i]) {
        return fences[i]->SetEventOnCompletion(values[i], event);
      }
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::SetResidencyPriority(
    UINT object_count, ID3D12Pageable *const *objects,
    const D3D12_RESIDENCY_PRIORITY *priorities) {
  TRACE("SetResidencyPriority count=%u objects=%p priorities=%p", object_count,
        (void *)objects, (void *)priorities);
  if (object_count && (!objects || !priorities))
    return E_INVALIDARG;
  std::vector<D3D12ResidencyObjectKind> kinds(object_count,
                                               D3D12ResidencyObjectKind::Invalid);
  for (UINT i = 0; i < object_count; i++) {
    if (!IsValidResidencyPriority(priorities[i]))
      return E_INVALIDARG;
    kinds[i] = ClassifyResidencyObject(objects[i]);
    if (kinds[i] == D3D12ResidencyObjectKind::Invalid)
      return E_INVALIDARG;
  }
  for (UINT i = 0; i < object_count; i++) {
    switch (kinds[i]) {
    case D3D12ResidencyObjectKind::Resource:
      static_cast<MTLD3D12Resource *>(objects[i])->SetResidencyPriority(
          priorities[i]);
      break;
    case D3D12ResidencyObjectKind::Heap:
      static_cast<MTLD3D12Heap *>(objects[i])->SetResidencyPriority(
          priorities[i]);
      break;
    case D3D12ResidencyObjectKind::DescriptorHeap:
      static_cast<MTLD3D12DescriptorHeap *>(objects[i])->SetResidencyPriority(
          priorities[i]);
      break;
    case D3D12ResidencyObjectKind::QueryHeap:
      static_cast<MTLD3D12QueryHeap *>(objects[i])->SetResidencyPriority(
          priorities[i]);
      break;
    default:
      break;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreatePipelineState(
    const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid,
    void **ppPipelineState) {
  TRACE("ID3D12Device2::CreatePipelineState ENTER: size=%zu",
        desc ? desc->SizeInBytes : 0);

  if (!desc || !desc->pPipelineStateSubobjectStream || !ppPipelineState)
    return E_INVALIDARG;

  *ppPipelineState = nullptr;

  auto *stream = (uint8_t *)desc->pPipelineStateSubobjectStream;
  auto *end = stream + desc->SizeInBytes;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc = {};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
  D3D12_SHADER_BYTECODE amplification_shader = {};
  D3D12_SHADER_BYTECODE mesh_shader = {};
  bool has_cs = false;
  bool is_compute = true;
  bool depth_bounds_test_enable = false;
  ID3D12RootSignature *created_stream_root_signature = nullptr;
  struct CreatedRootSignatureGuard {
    ID3D12RootSignature *&root_signature;
    ~CreatedRootSignatureGuard() {
      if (root_signature)
        root_signature->Release();
    }
  } root_signature_guard{created_stream_root_signature};

  graphics_desc.SampleMask = UINT_MAX;
  graphics_desc.SampleDesc.Count = 1;

  while (stream + sizeof(UINT) <= end) {
    uint8_t *subobject = stream;
    UINT type = *reinterpret_cast<UINT *>(subobject);
    bool advanced = false;

    switch (type) {
    case 0: { // D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE
      ID3D12RootSignature *root_signature = nullptr;
      if (!read_pipeline_stream_subobject(subobject, end, &root_signature))
        return E_INVALIDARG;
      graphics_desc.pRootSignature = root_signature;
      compute_desc.pRootSignature = root_signature;
      advanced = advance_pipeline_stream<ID3D12RootSignature *>(&stream, end);
      break;
    }
    case 1: { // VS
      if (!read_pipeline_stream_subobject(subobject, end, &graphics_desc.VS))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 2: { // PS
      if (!read_pipeline_stream_subobject(subobject, end, &graphics_desc.PS))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 3: { // DS
      if (!read_pipeline_stream_subobject(subobject, end, &graphics_desc.DS))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 4: { // HS
      if (!read_pipeline_stream_subobject(subobject, end, &graphics_desc.HS))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 5: { // GS
      if (!read_pipeline_stream_subobject(subobject, end, &graphics_desc.GS))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 6: { // CS
      if (!read_pipeline_stream_subobject(subobject, end, &compute_desc.CS))
        return E_INVALIDARG;
      has_cs = true;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 7: { // STREAM_OUTPUT
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.StreamOutput))
        return E_INVALIDARG;
      is_compute = false;
      advanced =
          advance_pipeline_stream<D3D12_STREAM_OUTPUT_DESC>(&stream, end);
      break;
    }
    case 8: { // BLEND
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.BlendState))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_BLEND_DESC>(&stream, end);
      break;
    }
    case 9: { // SAMPLE_MASK
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.SampleMask))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<UINT>(&stream, end);
      break;
    }
    case 10: { // RASTERIZER
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.RasterizerState))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_RASTERIZER_DESC>(&stream, end);
      break;
    }
    case 11: { // DEPTH_STENCIL
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.DepthStencilState))
        return E_INVALIDARG;
      depth_bounds_test_enable = false;
      is_compute = false;
      advanced =
          advance_pipeline_stream<D3D12_DEPTH_STENCIL_DESC>(&stream, end);
      break;
    }
    case 12: { // INPUT_LAYOUT
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.InputLayout))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_INPUT_LAYOUT_DESC>(&stream, end);
      break;
    }
    case 13: { // IB_STRIP_CUT_VALUE
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.IBStripCutValue))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(
          &stream, end);
      break;
    }
    case 14: { // PRIMITIVE_TOPOLOGY
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.PrimitiveTopologyType))
        return E_INVALIDARG;
      is_compute = false;
      advanced =
          advance_pipeline_stream<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(&stream, end);
      break;
    }
    case 15: { // RENDER_TARGET_FORMATS
      D3D12RTFormatArray fmt = {};
      if (!read_pipeline_stream_subobject(subobject, end, &fmt))
        return E_INVALIDARG;
      graphics_desc.NumRenderTargets = fmt.NumRenderTargets;
      for (UINT i = 0; i < 8 && i < fmt.NumRenderTargets; i++)
        graphics_desc.RTVFormats[i] = fmt.RTFormats[i];
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12RTFormatArray>(&stream, end);
      break;
    }
    case 16: { // DEPTH_STENCIL_FORMAT
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.DSVFormat))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<DXGI_FORMAT>(&stream, end);
      break;
    }
    case 17: { // SAMPLE_DESC
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &graphics_desc.SampleDesc))
        return E_INVALIDARG;
      is_compute = false;
      advanced = advance_pipeline_stream<DXGI_SAMPLE_DESC>(&stream, end);
      break;
    }
    case 18: { // NODE_MASK
      UINT node_mask = 0;
      if (!read_pipeline_stream_subobject(subobject, end, &node_mask))
        return E_INVALIDARG;
      graphics_desc.NodeMask = node_mask;
      compute_desc.NodeMask = node_mask;
      advanced = advance_pipeline_stream<UINT>(&stream, end);
      break;
    }
    case 19: { // CACHED_PSO
      D3D12_CACHED_PIPELINE_STATE cached_pso = {};
      if (!read_pipeline_stream_subobject(subobject, end, &cached_pso))
        return E_INVALIDARG;
      graphics_desc.CachedPSO = cached_pso;
      compute_desc.CachedPSO = cached_pso;
      advanced =
          advance_pipeline_stream<D3D12_CACHED_PIPELINE_STATE>(&stream, end);
      break;
    }
    case 20: { // FLAGS
      D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
      if (!read_pipeline_stream_subobject(subobject, end, &flags))
        return E_INVALIDARG;
      graphics_desc.Flags = flags;
      compute_desc.Flags = flags;
      advanced =
          advance_pipeline_stream<D3D12_PIPELINE_STATE_FLAGS>(&stream, end);
      break;
    }
    case 21: { // DEPTH_STENCIL1
      D3D12DepthStencilDesc1 depth_stencil = {};
      if (!read_pipeline_stream_subobject(subobject, end, &depth_stencil))
        return E_INVALIDARG;
      graphics_desc.DepthStencilState =
          convert_depth_stencil_desc1(depth_stencil);
      depth_bounds_test_enable = depth_stencil.DepthBoundsTestEnable;
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12DepthStencilDesc1>(&stream, end);
      break;
    }
    case 22: { // VIEW_INSTANCING
      D3D12ViewInstancingDesc view_instancing = {};
      if (!read_pipeline_stream_subobject(subobject, end, &view_instancing))
        return E_INVALIDARG;
      TRACE("CreatePipelineState: view instancing count=%u flags=0x%x ignored",
            view_instancing.ViewInstanceCount, view_instancing.Flags);
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12ViewInstancingDesc>(&stream, end);
      break;
    }
    case 26: { // DEPTH_STENCIL2
      D3D12DepthStencilDesc2 depth_stencil = {};
      if (!read_pipeline_stream_subobject(subobject, end, &depth_stencil))
        return E_INVALIDARG;
      graphics_desc.DepthStencilState =
          convert_depth_stencil_desc2(depth_stencil);
      depth_bounds_test_enable = depth_stencil.DepthBoundsTestEnable;
      TRACE("CreatePipelineState: depth-stencil2 depth=%d stencil=%d "
            "depth_bounds=%d",
            depth_stencil.DepthEnable, depth_stencil.StencilEnable,
            depth_stencil.DepthBoundsTestEnable);
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12DepthStencilDesc2>(&stream, end);
      break;
    }
    case 27: { // RASTERIZER1
      D3D12RasterizerDesc1 rasterizer = {};
      if (!read_pipeline_stream_subobject(subobject, end, &rasterizer))
        return E_INVALIDARG;
      graphics_desc.RasterizerState = convert_rasterizer_desc1(rasterizer);
      TRACE("CreatePipelineState: rasterizer1 fill=%u cull=%u depth_bias=%g "
            "forced_samples=%u conservative=%u",
            rasterizer.FillMode, rasterizer.CullMode, rasterizer.DepthBias,
            rasterizer.ForcedSampleCount, rasterizer.ConservativeRaster);
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12RasterizerDesc1>(&stream, end);
      break;
    }
    case 28: { // RASTERIZER2
      D3D12RasterizerDesc2 rasterizer = {};
      if (!read_pipeline_stream_subobject(subobject, end, &rasterizer))
        return E_INVALIDARG;
      graphics_desc.RasterizerState = convert_rasterizer_desc2(rasterizer);
      TRACE("CreatePipelineState: rasterizer2 fill=%u cull=%u depth_bias=%g "
            "line_mode=%u forced_samples=%u conservative=%u",
            rasterizer.FillMode, rasterizer.CullMode, rasterizer.DepthBias,
            rasterizer.LineRasterizationMode, rasterizer.ForcedSampleCount,
            rasterizer.ConservativeRaster);
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12RasterizerDesc2>(&stream, end);
      break;
    }
    case 29: { // SERIALIZED_ROOT_SIGNATURE
      D3D12_SHADER_BYTECODE root_signature = {};
      if (!read_pipeline_stream_subobject(subobject, end, &root_signature))
        return E_INVALIDARG;
      if (root_signature.pShaderBytecode && root_signature.BytecodeLength) {
        if (created_stream_root_signature)
          created_stream_root_signature->Release();
        created_stream_root_signature =
            new MTLD3D12RootSignature(this, root_signature.pShaderBytecode,
                                      root_signature.BytecodeLength);
        graphics_desc.pRootSignature = created_stream_root_signature;
        compute_desc.pRootSignature = created_stream_root_signature;
        TRACE("CreatePipelineState: serialized root signature bytes=%zu -> %p",
              root_signature.BytecodeLength, created_stream_root_signature);
      } else {
        TRACE("CreatePipelineState: serialized root signature empty");
      }
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 24: { // AS
      if (!read_pipeline_stream_subobject(subobject, end,
                                          &amplification_shader))
        return E_INVALIDARG;
      TRACE("CreatePipelineState: amplification shader bytes=%zu",
            amplification_shader.BytecodeLength);
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    case 25: { // MS
      if (!read_pipeline_stream_subobject(subobject, end, &mesh_shader))
        return E_INVALIDARG;
      TRACE("CreatePipelineState: mesh shader bytes=%zu",
            mesh_shader.BytecodeLength);
      is_compute = false;
      advanced = advance_pipeline_stream<D3D12_SHADER_BYTECODE>(&stream, end);
      break;
    }
    default:
      TRACE("CreatePipelineState: unknown subobject type %u at offset=%zu",
            type,
            static_cast<size_t>(
                subobject - (uint8_t *)desc->pPipelineStateSubobjectStream));
      return E_INVALIDARG;
    }

    if (!advanced)
      return E_INVALIDARG;
  }

  if (has_cs && is_compute) {
    compute_desc.pRootSignature = graphics_desc.pRootSignature;
    TRACE("ID3D12Device2::CreatePipelineState -> delegating to "
          "CreateComputePSO CS=%p",
          compute_desc.CS.pShaderBytecode);
    return CreateComputePipelineState(&compute_desc, riid, ppPipelineState);
  }

  if (mesh_shader.pShaderBytecode && mesh_shader.BytecodeLength) {
    if (has_cs) {
      TRACE("CreatePipelineState: mesh and compute shaders are mutually exclusive");
      return E_INVALIDARG;
    }
    auto *pso = new MTLD3D12PipelineState(this, false);
    pso->SetGraphicsDesc(graphics_desc);
    pso->SetDepthBoundsTestEnable(depth_bounds_test_enable);
    pso->SetMeshShaders(amplification_shader, mesh_shader);
    bool compiled = pso->RequestCompile(false);
    TRACE("ID3D12Device2::CreatePipelineState mesh compile=%d stage=%s "
          "detail=%s",
          compiled, pso->GetCompileFailureStage().c_str(),
          pso->GetCompileFailureDetail().c_str());
    if (!compiled) {
      pso->Release();
      return E_FAIL;
    }
    HRESULT hr = pso->QueryInterface(riid, ppPipelineState);
    pso->Release();
    return hr;
  }

  if (amplification_shader.pShaderBytecode &&
      amplification_shader.BytecodeLength) {
    TRACE("CreatePipelineState: amplification shader without mesh shader");
    return E_INVALIDARG;
  }

  TRACE("ID3D12Device2::CreatePipelineState -> delegating to CreateGraphicsPSO "
        "VS=%p PS=%p NumRT=%u",
        graphics_desc.VS.pShaderBytecode, graphics_desc.PS.pShaderBytecode,
        graphics_desc.NumRenderTargets);
  return CreateGraphicsPipelineStateInternal(
      &graphics_desc, riid, ppPipelineState, depth_bounds_test_enable);
}

/*** ID3D12Device3 ***/
HRESULT STDMETHODCALLTYPE MTLD3D12Device::OpenExistingHeapFromAddress(
    const void *address, REFIID riid, void **heap) {
  TRACE("ID3D12Device3::OpenExistingHeapFromAddress address=%p", address);
  if (!heap)
    return E_POINTER;
  InitReturnPtr(heap);
  if (!address)
    return E_INVALIDARG;
  auto existing = FindHeapContainingAddress(address, this);
  if (!existing)
    return DXGI_ERROR_NOT_FOUND;
  HRESULT hr = existing->QueryInterface(riid, heap);
  existing->Release();
  TRACE("ID3D12Device3::OpenExistingHeapFromAddress out=%p hr=0x%lx",
        *heap, hr);
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::OpenExistingHeapFromFileMapping(
    HANDLE file_mapping, REFIID riid, void **heap) {
  TRACE("ID3D12Device3::OpenExistingHeapFromFileMapping mapping=%p",
        file_mapping);
  if (!heap)
    return E_POINTER;
  InitReturnPtr(heap);
  if (!file_mapping)
    return E_INVALIDARG;
  ID3D12Heap *opened_heap = nullptr;
  HRESULT hr = OpenSharedHeapFromMapping(this, file_mapping, &opened_heap);
  if (FAILED(hr) || !opened_heap)
    return FAILED(hr) ? hr : DXGI_ERROR_INVALID_CALL;
  hr = opened_heap->QueryInterface(riid, heap);
  opened_heap->Release();
  TRACE("ID3D12Device3::OpenExistingHeapFromFileMapping out=%p hr=0x%lx",
        *heap, hr);
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::EnqueueMakeResident(
    D3D12_RESIDENCY_FLAGS flags, UINT num_objects,
    ID3D12Pageable *const *objects, ID3D12Fence *fence, UINT64 fence_value) {
  TRACE("ID3D12Device3::EnqueueMakeResident flags=0x%x objects=%u fence=%p",
        (unsigned)flags, num_objects, (void *)fence);
  if (static_cast<UINT>(flags) & ~static_cast<UINT>(
                                      D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET))
    return E_INVALIDARG;
  HRESULT hr = MakeResident(num_objects, objects);
  if (FAILED(hr) || !fence)
    return hr;
  HRESULT signal_hr = fence->Signal(fence_value);
  return FAILED(signal_hr) ? signal_hr : hr;
}

/*** ID3D12Device4 ***/
HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommandList1(
    UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **command_list) {
  TRACE("ID3D12Device4::CreateCommandList1 -> delegating to CreateCommandList");
  return CreateCommandList(node_mask, type, nullptr, nullptr, riid,
                           command_list);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateProtectedResourceSession(
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc, REFIID riid,
    void **session) {
  TRACE("ID3D12Device4::CreateProtectedResourceSession -> E_NOTIMPL");
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommittedResource1(
    const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_resource_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource,
    void **resource) {
  if (protected_session) {
    TRACE("ID3D12Device4::CreateCommittedResource1 -> E_NOTIMPL (protected "
          "session)");
    return E_NOTIMPL;
  }
  return CreateCommittedResource(heap_properties, heap_flags, desc,
                                 initial_resource_state, optimized_clear_value,
                                 riid_resource, resource);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Device::CreateHeap1(const D3D12_HEAP_DESC *desc,
                            ID3D12ProtectedResourceSession *protected_session,
                            REFIID riid, void **heap) {
  if (protected_session) {
    TRACE("ID3D12Device4::CreateHeap1 -> E_NOTIMPL (protected session)");
    return E_NOTIMPL;
  }
  return CreateHeap(desc, riid, heap);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateReservedResource1(
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session, REFIID riid,
    void **resource) {
  TRACE("ID3D12Device4::CreateReservedResource1 protected=%p",
        (void *)protected_session);
  if (protected_session)
    return E_NOTIMPL;
  return CreateReservedResource(desc, initial_state, optimized_clear_value,
                                riid, resource);
}

D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
MTLD3D12Device::GetResourceAllocationInfo1(
    D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
    UINT resource_descs_count, const D3D12_RESOURCE_DESC *resource_descs,
    D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) {
  TRACE("ID3D12Device4::GetResourceAllocationInfo1 count=%u sideband=%p",
        resource_descs_count, (void *)resource_allocation_info1);
  return FillResourceAllocationInfoWithSideband(
      __ret, visible_mask, resource_descs_count, resource_descs,
      resource_allocation_info1);
}

/*** ID3D12Device5 ***/
HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateLifetimeTracker(
    ID3D12LifetimeOwner *owner, REFIID riid, void **tracker) {
  TRACE("ID3D12Device5::CreateLifetimeTracker -> E_NOTIMPL");
  return E_NOTIMPL;
}

void STDMETHODCALLTYPE MTLD3D12Device::RemoveDevice() {
  TRACE("ID3D12Device5::RemoveDevice");
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::EnumerateMetaCommands(
    UINT *meta_commands_count, D3D12_META_COMMAND_DESC *descs) {
  TRACE("ID3D12Device5::EnumerateMetaCommands -> E_NOTIMPL");
  if (meta_commands_count)
    *meta_commands_count = 0;
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::EnumerateMetaCommandParameters(
    REFGUID command_id, D3D12_META_COMMAND_PARAMETER_STAGE stage,
    UINT *total_structure_size_in_bytes, UINT *parameter_count,
    D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs) {
  TRACE("ID3D12Device5::EnumerateMetaCommandParameters -> E_NOTIMPL");
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateMetaCommand(
    REFGUID command_id, UINT node_mask, const void *creation_parameters_data,
    SIZE_T creation_parameters_data_size_in_bytes, REFIID riid,
    void **meta_command) {
  TRACE("ID3D12Device5::CreateMetaCommand -> E_NOTIMPL");
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateStateObject(
    const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object) {
  TRACE("ID3D12Device5::CreateStateObject type=%u subobjects=%u",
        desc ? (unsigned)desc->Type : 0xFFFFFFFFu,
        desc ? desc->NumSubobjects : 0);
  if (!state_object)
    return E_POINTER;
  *state_object = nullptr;
  if (!desc || (desc->NumSubobjects && !desc->pSubobjects))
    return E_INVALIDARG;
  auto *object = new MTLD3D12StateObject(this, desc);
  if (!object->Initialize(desc)) {
    object->Release();
    return E_FAIL;
  }
  HRESULT hr = object->QueryInterface(riid, state_object);
  object->Release();
  return hr;
}

static bool D3D12ResolveTriangleGeometryInfo(
    MTLD3D12Device *device,
    const D3D12_RAYTRACING_GEOMETRY_DESC *geometry,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags,
    WMTPrimitiveAccelerationStructureInfo &info) {
  info = {};
  if (!device)
    return false;
  if (!geometry ||
      geometry->Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES ||
      geometry->Triangles.VertexFormat != DXGI_FORMAT_R32G32B32_FLOAT ||
      !geometry->Triangles.VertexBuffer.StartAddress ||
      !geometry->Triangles.VertexBuffer.StrideInBytes)
    return false;

  auto *vertex_resource = device->LookupResourceByGPUAddress(
      geometry->Triangles.VertexBuffer.StartAddress);
  if (!vertex_resource || !vertex_resource->GetMTLBuffer().handle)
    return false;

  info.vertex_buffer = vertex_resource->GetMTLBuffer().handle;
  info.vertex_buffer_offset = geometry->Triangles.VertexBuffer.StartAddress -
                              vertex_resource->GetGPUVirtualAddress();
  info.vertex_stride = geometry->Triangles.VertexBuffer.StrideInBytes;
  info.opaque =
      (geometry->Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) != 0;
  info.allow_refit =
      (flags &
       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) != 0;

  if (geometry->Triangles.IndexFormat == DXGI_FORMAT_UNKNOWN) {
    info.index_type = WMTAccelerationStructureIndexTypeNone;
    info.triangle_count = geometry->Triangles.VertexCount / 3;
  } else {
    if (geometry->Triangles.IndexFormat != DXGI_FORMAT_R16_UINT &&
        geometry->Triangles.IndexFormat != DXGI_FORMAT_R32_UINT)
      return false;
    auto *index_resource = device->LookupResourceByGPUAddress(
        geometry->Triangles.IndexBuffer);
    if (!index_resource || !index_resource->GetMTLBuffer().handle)
      return false;
    info.index_buffer = index_resource->GetMTLBuffer().handle;
    info.index_buffer_offset = geometry->Triangles.IndexBuffer -
                               index_resource->GetGPUVirtualAddress();
    info.index_type = geometry->Triangles.IndexFormat == DXGI_FORMAT_R32_UINT
                          ? WMTAccelerationStructureIndexTypeUInt32
                          : WMTAccelerationStructureIndexTypeUInt16;
    info.triangle_count = geometry->Triangles.IndexCount / 3;
  }
  return info.triangle_count != 0;
}

bool D3D12ResolveTriangleAccelerationStructureInfo(
    MTLD3D12Device *device,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *inputs,
    WMTPrimitiveAccelerationStructureInfo &info) {
  if (!device || !inputs ||
      inputs->Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL ||
      inputs->NumDescs != 1)
    return false;
  const D3D12_RAYTRACING_GEOMETRY_DESC *geometry = nullptr;
  if (inputs->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY)
    geometry = inputs->pGeometryDescs;
  else if (inputs->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS &&
           inputs->ppGeometryDescs)
    geometry = inputs->ppGeometryDescs[0];
  return D3D12ResolveTriangleGeometryInfo(device, geometry, inputs->Flags,
                                          info);
}

bool D3D12ResolveAABBAccelerationStructureInfo(
    MTLD3D12Device *device,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *inputs,
    WMTAABBAccelerationStructureInfo &info) {
  info = {};
  if (!device || !inputs ||
      inputs->Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL ||
      inputs->NumDescs != 1)
    return false;

  const D3D12_RAYTRACING_GEOMETRY_DESC *geometry = nullptr;
  if (inputs->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY) {
    geometry = inputs->pGeometryDescs;
  } else if (inputs->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS &&
             inputs->ppGeometryDescs) {
    geometry = inputs->ppGeometryDescs[0];
  }
  if (!geometry ||
      geometry->Type !=
          D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS ||
      !geometry->AABBs.AABBs.StartAddress ||
      !geometry->AABBs.AABBs.StrideInBytes ||
      !geometry->AABBs.AABBCount)
    return false;

  auto *resource = device->LookupResourceByGPUAddress(
      geometry->AABBs.AABBs.StartAddress);
  if (!resource || !resource->GetMTLBuffer().handle)
    return false;
  info.bounding_box_buffer = resource->GetMTLBuffer().handle;
  info.bounding_box_buffer_offset = geometry->AABBs.AABBs.StartAddress -
                                    resource->GetGPUVirtualAddress();
  info.bounding_box_stride = geometry->AABBs.AABBs.StrideInBytes;
  info.bounding_box_count = geometry->AABBs.AABBCount;
  info.opaque =
      (geometry->Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) != 0;
  // Entry zero hosts the triangle indirection wrapper. Procedural geometry
  // selects the procedural indirection wrapper at entry one.
  info.intersection_function_table_offset = 1;
  info.allow_refit =
      (inputs->Flags &
       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) != 0;
  return true;
}

void STDMETHODCALLTYPE
MTLD3D12Device::GetRaytracingAccelerationStructurePrebuildInfo(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) {
  TRACE("ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo");
  if (!info)
    return;
  memset(info, 0, sizeof(*info));
  if (!m_metal_raytracing_supported)
    return;

  if (!desc)
    return;
  WMTAccelerationStructureSizes sizes = {};
  uint64_t primitive_count = 0;
  const char *kind = "unknown";
  if (desc->Type ==
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
    if (!desc->NumDescs) {
      TRACE("  prebuild Metal TLAS size query failed: no instances");
      return;
    }
    // A logical D3D12 instance can refer to the tagged mixed-geometry BLAS
    // fallback.  The replay path flattens that BLAS into one triangle and one
    // AABB instance because Metal does not safely accept mixed descriptor
    // arrays on this toolchain.  Reserve the worst-case two Metal instances
    // per D3D12 instance; over-allocation is legal for prebuild results and
    // avoids making the destination too small when the instance buffer is not
    // CPU-readable at query time.
    const uint64_t metal_instance_count =
        std::min<uint64_t>(uint64_t(desc->NumDescs) * 2u, UINT32_MAX);
    if (!GetMTLDevice().accelerationStructureSizesForInstances(
            metal_instance_count,
            (desc->Flags &
             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) !=
                0,
            sizes)) {
      TRACE("  prebuild Metal TLAS size query failed");
      return;
    }
    primitive_count = metal_instance_count;
    kind = "TLAS instances";
  } else if (desc->NumDescs > 1) {
    if (desc->NumDescs >
        CmdBuildRaytracingAccelerationStructure::kMaxGeometryDescs) {
      TRACE("  prebuild too many BLAS geometries=%u", desc->NumDescs);
      return;
    }
    bool has_aabb_geometry = false;
    for (UINT i = 0; i < desc->NumDescs; ++i) {
      const auto *geometry =
          desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
              ? (desc->pGeometryDescs ? &desc->pGeometryDescs[i] : nullptr)
              : (desc->ppGeometryDescs ? desc->ppGeometryDescs[i] : nullptr);
      has_aabb_geometry |=
          geometry &&
          geometry->Type ==
              D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    }
    if (has_aabb_geometry) {
      WMTPrimitiveAccelerationStructureInfo triangle_info = {};
      WMTAABBAccelerationStructureInfo aabb_info = {};
      bool valid = true;
      UINT triangle_count = 0;
      UINT aabb_count = 0;
      for (UINT i = 0; valid && i < desc->NumDescs; ++i) {
        const auto *geometry =
            desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
                ? (desc->pGeometryDescs ? &desc->pGeometryDescs[i] : nullptr)
                : (desc->ppGeometryDescs ? desc->ppGeometryDescs[i] : nullptr);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS one = *desc;
        one.NumDescs = 1;
        one.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        if (geometry &&
            geometry->Type ==
                D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
          one.pGeometryDescs = geometry;
          valid = ++aabb_count == 1 &&
                  D3D12ResolveAABBAccelerationStructureInfo(this, &one,
                                                             aabb_info);
          if (valid)
            primitive_count += aabb_info.bounding_box_count;
        } else {
          one.pGeometryDescs = geometry;
          valid = ++triangle_count == 1 &&
                  D3D12ResolveTriangleGeometryInfo(this, geometry, desc->Flags,
                                                    triangle_info);
          if (valid)
            primitive_count += triangle_info.triangle_count;
        }
      }
      WMTAccelerationStructureSizes triangle_sizes = {};
      WMTAccelerationStructureSizes aabb_sizes = {};
      WMTAccelerationStructureSizes instance_sizes = {};
      const bool allow_update =
          (desc->Flags &
           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) !=
          0;
      if (!valid || triangle_count != 1 || aabb_count != 1 ||
          !GetMTLDevice().accelerationStructureSizesForTriangles(
              triangle_info, triangle_sizes) ||
          !GetMTLDevice().accelerationStructureSizesForAABBs(
              aabb_info, aabb_sizes) ||
          !GetMTLDevice().accelerationStructureSizesForInstances(
              2, allow_update, instance_sizes)) {
        TRACE("  prebuild mixed-geometry composite size query failed valid=%d "
              "triangles=%u aabbs=%u tri_handle=%llu aabb_handle=%llu "
              "aabb_addr=0x%llx stride=%u count=%u "
              "tri_size=%llu aabb_size=%llu instance_size=%llu",
              valid ? 1 : 0, triangle_count, aabb_count,
              (unsigned long long)triangle_info.vertex_buffer,
              (unsigned long long)aabb_info.bounding_box_buffer,
              (unsigned long long)(desc->pGeometryDescs && desc->NumDescs > 1
                                       ? desc->pGeometryDescs[1].AABBs.AABBs.StartAddress
                                       : 0),
              desc->pGeometryDescs && desc->NumDescs > 1
                  ? desc->pGeometryDescs[1].AABBs.AABBs.StrideInBytes
                  : 0,
              desc->pGeometryDescs && desc->NumDescs > 1
                  ? desc->pGeometryDescs[1].AABBs.AABBCount
                  : 0,
              (unsigned long long)triangle_sizes.acceleration_structure_size,
              (unsigned long long)aabb_sizes.acceleration_structure_size,
              (unsigned long long)instance_sizes.acceleration_structure_size);
        return;
      }
      sizes.acceleration_structure_size =
          instance_sizes.acceleration_structure_size;
      sizes.build_scratch_buffer_size = std::max(
          {triangle_sizes.build_scratch_buffer_size,
           aabb_sizes.build_scratch_buffer_size,
           instance_sizes.build_scratch_buffer_size});
      sizes.refit_scratch_buffer_size = std::max(
          {triangle_sizes.refit_scratch_buffer_size,
           aabb_sizes.refit_scratch_buffer_size,
           instance_sizes.refit_scratch_buffer_size});
      kind = "BLAS mixed geometries";
    } else {
    std::array<WMTPrimitiveAccelerationStructureInfo,
               CmdBuildRaytracingAccelerationStructure::kMaxGeometryDescs>
        metal_infos = {};
    for (UINT i = 0; i < desc->NumDescs; i++) {
      const auto *geometry =
          desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
              ? (desc->pGeometryDescs ? &desc->pGeometryDescs[i] : nullptr)
              : (desc->ppGeometryDescs ? desc->ppGeometryDescs[i] : nullptr);
      if (!D3D12ResolveTriangleGeometryInfo(this, geometry, desc->Flags,
                                            metal_infos[i])) {
        TRACE("  prebuild unsupported multi-geometry BLAS entry=%u", i);
        return;
      }
      primitive_count += metal_infos[i].triangle_count;
    }
    if (!GetMTLDevice().accelerationStructureSizesForTriangleGeometries(
            metal_infos.data(), desc->NumDescs, sizes)) {
      TRACE("  prebuild multi-geometry Metal size query failed");
      return;
    }
    kind = "BLAS triangle geometries";
    }
  } else {
    const D3D12_RAYTRACING_GEOMETRY_DESC *geometry = nullptr;
    if (desc->NumDescs == 1 &&
        desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY)
      geometry = desc->pGeometryDescs;
    else if (desc->NumDescs == 1 &&
             desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS &&
             desc->ppGeometryDescs)
      geometry = desc->ppGeometryDescs[0];
    if (geometry &&
        geometry->Type ==
            D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
      WMTAABBAccelerationStructureInfo metal_info = {};
      if (!D3D12ResolveAABBAccelerationStructureInfo(this, desc, metal_info) ||
          !GetMTLDevice().accelerationStructureSizesForAABBs(metal_info,
                                                             sizes)) {
        TRACE("  prebuild unsupported AABB input shape");
        return;
      }
      primitive_count = metal_info.bounding_box_count;
      kind = "BLAS AABBs";
    } else {
      WMTPrimitiveAccelerationStructureInfo metal_info = {};
      if (!D3D12ResolveTriangleAccelerationStructureInfo(this, desc,
                                                          metal_info) ||
          !GetMTLDevice().accelerationStructureSizesForTriangles(metal_info,
                                                                 sizes)) {
        TRACE("  prebuild unsupported input shape");
        return;
      }
      primitive_count = metal_info.triangle_count;
      kind = "BLAS triangles";
    }
  }
  auto align_256 = [](uint64_t value) { return (value + 255ull) & ~255ull; };
  info->ResultDataMaxSizeInBytes =
      align_256(sizes.acceleration_structure_size);
  info->ScratchDataSizeInBytes =
      align_256(sizes.build_scratch_buffer_size);
  info->UpdateScratchDataSizeInBytes =
      align_256(sizes.refit_scratch_buffer_size);
  TRACE("  prebuild %s=%llu result=%llu scratch=%llu update=%llu", kind,
        (unsigned long long)primitive_count,
        (unsigned long long)info->ResultDataMaxSizeInBytes,
        (unsigned long long)info->ScratchDataSizeInBytes,
        (unsigned long long)info->UpdateScratchDataSizeInBytes);
}

D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE
MTLD3D12Device::CheckDriverMatchingIdentifier(
    D3D12_SERIALIZED_DATA_TYPE serialized_data_type,
    const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER
        *identifier_to_check) {
  if (serialized_data_type !=
      D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE)
    return D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
  if (!identifier_to_check)
    return D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
  const auto &expected = GetRaytracingSerializationIdentifier();
  if (!std::memcmp(identifier_to_check, &expected, sizeof(expected))) {
    TRACE("ID3D12Device5::CheckDriverMatchingIdentifier -> COMPATIBLE");
    return D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
  }
  if (!std::memcmp(&identifier_to_check->DriverOpaqueGUID,
                   &expected.DriverOpaqueGUID, sizeof(expected.DriverOpaqueGUID)))
    return D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION;
  return D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
}

/*** ID3D12Device6 ***/
HRESULT STDMETHODCALLTYPE MTLD3D12Device::SetBackgroundProcessingMode(
    D3D12_BACKGROUND_PROCESSING_MODE mode, D3D12_MEASUREMENTS_ACTION action,
    HANDLE event, WINBOOL *further_measurements_desired) {
  TRACE("ID3D12Device6::SetBackgroundProcessingMode -> E_NOTIMPL");
  return E_NOTIMPL;
}

/*** ID3D12Device7 ***/
HRESULT STDMETHODCALLTYPE
MTLD3D12Device::AddToStateObject(const D3D12_STATE_OBJECT_DESC *addition,
                                 ID3D12StateObject *state_object_to_grow_from,
                                 REFIID riid, void **new_state_object) {
  TRACE("ID3D12Device7::AddToStateObject type=%u subobjects=%u base=%p",
        addition ? (unsigned)addition->Type : 0xFFFFFFFFu,
        addition ? addition->NumSubobjects : 0, state_object_to_grow_from);
  if (!new_state_object)
    return E_POINTER;
  *new_state_object = nullptr;
  if (!addition || !state_object_to_grow_from ||
      (addition->NumSubobjects && !addition->pSubobjects))
    return E_INVALIDARG;
  auto *object =
      new MTLD3D12StateObject(this, addition, state_object_to_grow_from);
  if (!object->InitializeAddition(addition)) {
    object->Release();
    return E_NOTIMPL;
  }
  HRESULT hr = object->QueryInterface(riid, new_state_object);
  object->Release();
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateProtectedResourceSession1(
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc, REFIID riid,
    void **session) {
  TRACE("ID3D12Device7::CreateProtectedResourceSession1 -> E_NOTIMPL");
  return E_NOTIMPL;
}

/*** ID3D12Device8 ***/
static const int MAX_DESCS = 256;

static bool IsSamplerFeedbackFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ||
         format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE;
}

static bool ValidateSamplerFeedbackResourceDesc(
    const D3D12_RESOURCE_DESC1 &desc) {
  if (!IsSamplerFeedbackFormat(desc.Format))
    return true;
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         desc.Width != 0 && desc.Height != 0 &&
         desc.DepthOrArraySize != 0 && desc.SampleDesc.Count == 1 &&
         desc.SampleDesc.Quality == 0 &&
         desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
         (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
         desc.SamplerFeedbackMipRegion.Width != 0 &&
         desc.SamplerFeedbackMipRegion.Height != 0 &&
         desc.SamplerFeedbackMipRegion.Depth <= 1;
}

D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
MTLD3D12Device::GetResourceAllocationInfo2(
    D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
    UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs,
    D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) {
  TRACE("ID3D12Device8::GetResourceAllocationInfo2 count=%u sideband=%p",
        resource_descs_count, (void *)resource_allocation_info1);
  D3D12_RESOURCE_DESC descs_compat[MAX_DESCS];
  if (resource_descs_count > MAX_DESCS ||
      (resource_descs_count && !resource_descs)) {
    if (__ret) {
      __ret->SizeInBytes = 0;
      __ret->Alignment = 0;
    }
    if (resource_allocation_info1 && resource_descs_count <= MAX_DESCS)
      std::memset(resource_allocation_info1, 0,
                  resource_descs_count * sizeof(*resource_allocation_info1));
    return __ret;
  }
  UINT count = resource_descs_count;
  for (UINT i = 0; i < count; i++) {
    memcpy(&descs_compat[i], &resource_descs[i], sizeof(D3D12_RESOURCE_DESC));
  }
  return FillResourceAllocationInfoWithSideband(
      __ret, visible_mask, count, descs_compat, resource_allocation_info1);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommittedResource2(
    const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_resource_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource,
    void **resource) {
  if (!desc || !resource)
    return E_POINTER;
  if (!ValidateSamplerFeedbackResourceDesc(*desc)) {
    InitReturnPtr(resource);
    return E_INVALIDARG;
  }
  if (protected_session) {
    TRACE("ID3D12Device8::CreateCommittedResource2 -> E_NOTIMPL (protected "
          "session)");
    return E_NOTIMPL;
  }
  D3D12_RESOURCE_DESC desc_compat;
  memcpy(&desc_compat, desc, sizeof(D3D12_RESOURCE_DESC));
  HRESULT hr = CreateCommittedResource(
      heap_properties, heap_flags, &desc_compat, initial_resource_state,
      optimized_clear_value, riid_resource, resource);
  if (SUCCEEDED(hr) && resource && *resource &&
      (desc->Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ||
       desc->Format ==
           DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE)) {
    if (!static_cast<MTLD3D12Resource *>(
             static_cast<ID3D12Resource *>(*resource))
             ->ConfigureSamplerFeedback(desc->SamplerFeedbackMipRegion)) {
      static_cast<ID3D12Resource *>(*resource)->Release();
      *resource = nullptr;
      hr = E_OUTOFMEMORY;
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreatePlacedResource1(
    ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
    void **resource) {
  if (!desc || !resource)
    return E_POINTER;
  if (!ValidateSamplerFeedbackResourceDesc(*desc)) {
    InitReturnPtr(resource);
    return E_INVALIDARG;
  }
  D3D12_RESOURCE_DESC desc_compat;
  memcpy(&desc_compat, desc, sizeof(D3D12_RESOURCE_DESC));
  HRESULT hr = CreatePlacedResource(heap, heap_offset, &desc_compat,
                                    initial_state, optimized_clear_value, riid,
                                    resource);
  if (SUCCEEDED(hr) && resource && *resource &&
      (desc->Format == DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ||
       desc->Format ==
           DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE)) {
    if (!static_cast<MTLD3D12Resource *>(
             static_cast<ID3D12Resource *>(*resource))
             ->ConfigureSamplerFeedback(desc->SamplerFeedbackMipRegion)) {
      static_cast<ID3D12Resource *>(*resource)->Release();
      *resource = nullptr;
      hr = E_OUTOFMEMORY;
    }
  }
  return hr;
}

void STDMETHODCALLTYPE MTLD3D12Device::CreateSamplerFeedbackUnorderedAccessView(
    ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource,
    D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor) {
  auto *d = reinterpret_cast<D3D12Descriptor *>(dst_descriptor.ptr);
  auto *feedback = static_cast<MTLD3D12Resource *>(feedback_resource);
  auto *target = static_cast<MTLD3D12Resource *>(targeted_resource);
  if (d) {
    d->resource = nullptr;
    d->resource_uav_counter = nullptr;
    d->sampler_feedback_target = nullptr;
    d->is_sampler_feedback = false;
    d->metal_texture_view = {};
    d->metal_texture_gpu_id = 0;
  }
  if (!d || !feedback || !target || !feedback->IsSamplerFeedback()) {
    UpdateDescriptorTableMirror(this, d);
    TRACE("CreateSamplerFeedbackUAV rejected target=%p feedback=%p desc=%p configured=%d",
          (void *)targeted_resource, (void *)feedback_resource, (void *)d,
          feedback ? feedback->IsSamplerFeedback() : 0);
    return;
  }
  D3D12_RESOURCE_DESC target_desc = {};
  D3D12_RESOURCE_DESC feedback_desc = {};
  target->GetDesc(&target_desc);
  feedback->GetDesc(&feedback_desc);
  if (target_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      feedback_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      target_desc.Width != feedback_desc.Width ||
      target_desc.Height != feedback_desc.Height ||
      target_desc.DepthOrArraySize != feedback_desc.DepthOrArraySize ||
      target_desc.MipLevels != feedback_desc.MipLevels ||
      target_desc.SampleDesc.Count != 1 ||
      feedback_desc.SampleDesc.Count != 1 ||
      !(feedback_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
      (feedback_desc.Format !=
           DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE &&
       feedback_desc.Format !=
           DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE)) {
    TRACE("CreateSamplerFeedbackUAV rejected dimensions/formats target_dim=%u feedback_dim=%u format=%u",
          (unsigned)target_desc.Dimension, (unsigned)feedback_desc.Dimension,
          (unsigned)feedback_desc.Format);
    return;
  }
  d->resource = feedback_resource;
  d->resource_uav_counter = nullptr;
  d->sampler_feedback_target = targeted_resource;
  d->is_sampler_feedback = true;
  d->metal_texture_view = {};
  d->metal_texture_gpu_id = 0;
  d->type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  d->range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  d->uav = {};
  d->uav.Format = DXGI_FORMAT_R32_TYPELESS;
  d->uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  d->uav.Buffer.FirstElement = feedback->GetSamplerFeedbackDataOffset() / 4;
  d->uav.Buffer.NumElements = static_cast<UINT>(
      (feedback->GetBufferByteLength() -
       feedback->GetSamplerFeedbackDataOffset()) /
      4);
  d->uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  UpdateDescriptorTableMirror(this, d);
  TRACE("CreateSamplerFeedbackUAV target=%p feedback=%p physical=%ux%u row=%u bytes=%llu",
        (void *)targeted_resource, (void *)feedback_resource,
        feedback->GetSamplerFeedbackWidth(),
        feedback->GetSamplerFeedbackHeight(),
        feedback->GetSamplerFeedbackRowPitch(),
        (unsigned long long)feedback->GetBufferByteLength());
}

void STDMETHODCALLTYPE MTLD3D12Device::GetCopyableFootprints1(
    const D3D12_RESOURCE_DESC1 *resource_desc, UINT first_subresource,
    UINT subresources_count, UINT64 base_offset,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows_count,
    UINT64 *row_size_in_bytes, UINT64 *total_bytes) {
  D3D12_RESOURCE_DESC desc_compat;
  memcpy(&desc_compat, resource_desc, sizeof(D3D12_RESOURCE_DESC));
  GetCopyableFootprints(&desc_compat, first_subresource, subresources_count,
                        base_offset, layouts, rows_count, row_size_in_bytes,
                        total_bytes);
}

/*** ID3D12Device9 ***/
HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateShaderCacheSession(
    const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid, void **session) {
  if (!desc || !session)
    return E_POINTER;
  InitReturnPtr(session);
  TRACE("ID3D12Device9::CreateShaderCacheSession mode=%u flags=0x%x riid=%s",
        (unsigned)desc->Mode, (unsigned)desc->Flags, str::format(riid).c_str());
  auto *cache = new MTLD3D12ShaderCacheSession(this, *desc);
  HRESULT hr = cache->QueryInterface(riid, session);
  if (FAILED(hr))
    delete cache;
  TRACE("ID3D12Device9::CreateShaderCacheSession -> 0x%lx session=%p", hr,
        session ? *session : nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Device::ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS kinds,
                                   D3D12_SHADER_CACHE_CONTROL_FLAGS control) {
  TRACE("ID3D12Device9::ShaderCacheControl kinds=0x%x control=0x%x -> S_OK",
        (unsigned)kinds, (unsigned)control);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommandQueue1(
    const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator_id, REFIID riid,
    void **command_queue) {
  TRACE(
      "ID3D12Device9::CreateCommandQueue1 -> delegating to CreateCommandQueue");
  return CreateCommandQueue(desc, riid, command_queue);
}

/*** ID3D12Device10 ***/
static D3D12_RESOURCE_STATES
ResourceStateForBarrierLayout(D3D12_BARRIER_LAYOUT layout) {
  switch (layout) {
  case D3D12_BARRIER_LAYOUT_GENERIC_READ:
  case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
  case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
    return D3D12_RESOURCE_STATE_GENERIC_READ;
  case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
    return D3D12_RESOURCE_STATE_RENDER_TARGET;
  case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
  case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
  case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
    return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
    return D3D12_RESOURCE_STATE_DEPTH_WRITE;
  case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
    return D3D12_RESOURCE_STATE_DEPTH_READ;
  case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
  case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
  case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
    return static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
  case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
  case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE:
    return D3D12_RESOURCE_STATE_COPY_SOURCE;
  case D3D12_BARRIER_LAYOUT_COPY_DEST:
  case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
  case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST:
    return D3D12_RESOURCE_STATE_COPY_DEST;
  case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:
    return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
  case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:
    return D3D12_RESOURCE_STATE_RESOLVE_DEST;
  case D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE:
    return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
  default:
    return D3D12_RESOURCE_STATE_COMMON;
  }
}

static bool ValidateCastableFormats(const D3D12_RESOURCE_DESC1 *desc,
                                    UINT32 count,
                                    const DXGI_FORMAT *formats) {
  if (!desc)
    return false;
  if (!count)
    return true;
  if (!formats || desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return false;
  const UINT resource_block = FormatBlockSize(desc->Format);
  const UINT resource_bytes = FormatBytesPerTexel(desc->Format);
  if (!resource_bytes)
    return false;
  for (UINT32 i = 0; i < count; i++) {
    const DXGI_FORMAT format = formats[i];
    const UINT view_block = FormatBlockSize(format);
    const UINT view_bytes = FormatBytesPerTexel(format);
    if (format == DXGI_FORMAT_UNKNOWN || !view_bytes ||
        MTLD3D12PipelineState::DXGIToMTLPixelFormat(format) ==
            WMTPixelFormatInvalid)
      return false;
    if (resource_block == 1) {
      if (view_block != 1 || view_bytes != resource_bytes)
        return false;
      continue;
    }
    if (view_bytes != resource_bytes)
      return false;
    if (view_block == resource_block)
      continue;
    if (view_block != 1 || desc->MipLevels != 1 ||
        desc->DepthOrArraySize != 1)
      return false;
  }
  return true;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateCommittedResource3(
    const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    UINT32 castable_formats_count, DXGI_FORMAT *castable_formats,
    REFIID riid_resource, void **resource) {
  if (protected_session) {
    TRACE("ID3D12Device10::CreateCommittedResource3 -> E_NOTIMPL (protected "
          "session)");
    return E_NOTIMPL;
  }
  if (!ValidateCastableFormats(desc, castable_formats_count,
                               castable_formats)) {
    TRACE("ID3D12Device10::CreateCommittedResource3 rejected invalid "
          "castable-format list count=%u",
          castable_formats_count);
    return E_INVALIDARG;
  }
  TRACE("ID3D12Device10::CreateCommittedResource3 castable_formats=%u",
        castable_formats_count);
  HRESULT hr = CreateCommittedResource2(
      heap_properties, heap_flags, desc,
      ResourceStateForBarrierLayout(initial_layout),
      optimized_clear_value, protected_session, riid_resource, resource);
  if (SUCCEEDED(hr) && resource && *resource) {
    static_cast<MTLD3D12Resource *>(
        static_cast<ID3D12Resource *>(*resource))
        ->SetCastableFormats(castable_formats_count, castable_formats);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreatePlacedResource2(
    ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc,
    D3D12_BARRIER_LAYOUT initial_layout,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid,
    void **resource) {
  if (!ValidateCastableFormats(desc, castable_formats_count,
                               castable_formats)) {
    TRACE("ID3D12Device10::CreatePlacedResource2 rejected invalid "
          "castable-format list count=%u",
          castable_formats_count);
    return E_INVALIDARG;
  }
  TRACE("ID3D12Device10::CreatePlacedResource2 castable_formats=%u",
        castable_formats_count);
  HRESULT hr = CreatePlacedResource1(
      heap, heap_offset, desc, ResourceStateForBarrierLayout(initial_layout),
      optimized_clear_value, riid, resource);
  if (SUCCEEDED(hr) && resource && *resource) {
    static_cast<MTLD3D12Resource *>(
        static_cast<ID3D12Resource *>(*resource))
        ->SetCastableFormats(castable_formats_count, castable_formats);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Device::CreateReservedResource2(
    const D3D12_RESOURCE_DESC *desc, D3D12_BARRIER_LAYOUT initial_layout,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid,
    void **resource) {
  TRACE("ID3D12Device10::CreateReservedResource2 protected=%p castable=%u",
        (void *)protected_session, castable_formats_count);
  if (protected_session ||
      (castable_formats_count && !castable_formats))
    return E_INVALIDARG;
  HRESULT hr = CreateReservedResource(
      desc, ResourceStateForBarrierLayout(initial_layout), optimized_clear_value,
      riid, resource);
  if (SUCCEEDED(hr) && resource && *resource && castable_formats_count)
    static_cast<MTLD3D12Resource *>(
        static_cast<ID3D12Resource *>(*resource))
        ->SetCastableFormats(castable_formats_count, castable_formats);
  return hr;
}

/*** ID3D12Device11Compat ***/
void STDMETHODCALLTYPE
MTLD3D12Device::CreateSampler2(const D3D12SamplerDesc2Compat *desc,
                               D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (!desc) {
    TRACE("ID3D12Device11::CreateSampler2 -> ignored null desc");
    return;
  }

  D3D12_SAMPLER_DESC compat = {};
  compat.Filter = desc->Filter;
  compat.AddressU = desc->AddressU;
  compat.AddressV = desc->AddressV;
  compat.AddressW = desc->AddressW;
  compat.MipLODBias = desc->MipLODBias;
  compat.MaxAnisotropy = desc->MaxAnisotropy;
  compat.ComparisonFunc = desc->ComparisonFunc;
  compat.MinLOD = desc->MinLOD;
  compat.MaxLOD = desc->MaxLOD;
  for (int i = 0; i < 4; i++) {
    compat.BorderColor[i] =
        (desc->Flags & D3D12SamplerFlagUintBorderColorCompat)
            ? static_cast<FLOAT>(desc->UintBorderColor[i])
            : desc->FloatBorderColor[i];
  }

  TRACE("ID3D12Device11::CreateSampler2 flags=0x%x -> delegating to "
        "CreateSampler",
        (unsigned)desc->Flags);
  CreateSampler(&compat, descriptor);
}

/*** ID3D12Device12Compat ***/
D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
MTLD3D12Device::GetResourceAllocationInfo3(
    D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
    UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs,
    const UINT32 *num_castable_formats,
    const DXGI_FORMAT *const *castable_formats,
    D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) {
  TRACE("ID3D12Device12::GetResourceAllocationInfo3 count=%u "
        "castable_counts=%p castable_formats=%p -> delegating",
        resource_descs_count, num_castable_formats, castable_formats);
  return GetResourceAllocationInfo2(__ret, visible_mask, resource_descs_count,
                                    resource_descs, resource_allocation_info1);
}

} // namespace dxmt
