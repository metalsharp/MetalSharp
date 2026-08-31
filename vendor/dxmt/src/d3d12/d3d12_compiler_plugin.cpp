#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <new>
#include <vector>

namespace {

// The Microsoft State Object Compiler loads the IHV plugin through this DDI
// entry point. This table is the plugin boundary used by
// D3D12StateObjectCompiler.
struct DDIHandle {
  void *pDrvPrivate;
};
struct DDIRuntimeHandle {
  void *handle;
};

struct DDICompilerTarget {
  UINT AdapterFamilyIndex;
  UINT64 ABIVersion;
};
struct DDICompilerVersionNumber {
  union {
    UINT64 Version;
    UINT16 VersionParts[4];
  };
};
struct DDICompilerApplicationDesc {
  WCHAR *pExeFilename;
  WCHAR *pName;
  DDICompilerVersionNumber Version;
  WCHAR *pEngineName;
  DDICompilerVersionNumber EngineVersion;
};
struct DDICompilerAdapterFamily {
  WCHAR szAdapterFamily[128];
};

using DDIAllocationFunc = void *(APIENTRY *)(SIZE_T, void *);
struct DDICompilerCacheValueKey {
  const void *pKey;
  UINT KeySize;
};
struct DDICompilerCacheValue {
  void *pValue;
  SIZE_T ValueSize;
};
struct DDICompilerCacheTypedValue {
  UINT Type;
  DDICompilerCacheValue Value;
};
struct DDICompilerCacheConstValue {
  const void *pValue;
  SIZE_T ValueSize;
};
struct DDICompilerCacheTypedConstValue {
  UINT Type;
  DDICompilerCacheConstValue Value;
};
using DDIFindValueFunc = HRESULT(APIENTRY *)(
    DDIRuntimeHandle, const DDICompilerCacheValueKey *,
    DDICompilerCacheTypedValue *, UINT, DDIAllocationFunc, void *);
using DDIStoreValueFunc = HRESULT(APIENTRY *)(
    DDIRuntimeHandle, const DDICompilerCacheValueKey *,
    const DDICompilerCacheTypedConstValue *, UINT);
using DDISetObjectKeysFunc = HRESULT(APIENTRY *)(
    DDIRuntimeHandle, const DDICompilerCacheValueKey *, UINT);
struct DDICompilerCacheCallbacks {
  DDIFindValueFunc pfnCompilerCacheFindValue;
  DDIStoreValueFunc pfnCompilerCacheStoreValue;
  DDISetObjectKeysFunc pfnCompilerCacheSetObjectValueKeys;
};

struct DDICompilerDDIFuncs;
struct DDICompilerCapabilitiesFuncs;
struct DDICompilerFuncs;
struct DDICompilerOpenArgs {
  DDIRuntimeHandle hRTCompilerDDI;
  DDIHandle hCompilerDDI;
  DDICompilerDDIFuncs *pDDIFuncs;
};
struct DDIGetCapsArgsCompat {
  UINT type;
  void *info;
  void *data;
  UINT data_size;
};

using DDIDestroyCompilerDDIFunc = void(APIENTRY *)(DDIHandle);
using DDIGetSupportedVersionsFunc = HRESULT(APIENTRY *)(DDIHandle, UINT32 *,
                                                         UINT64 *);
using DDISetSelectedVersionFunc = HRESULT(APIENTRY *)(DDIHandle, UINT64,
                                                      UINT64);
using DDIFillDDITableFunc = HRESULT(APIENTRY *)(DDIHandle, UINT, void *,
                                                 SIZE_T);
using DDISetCallbackTableFunc = HRESULT(APIENTRY *)(DDIHandle, UINT,
                                                     const void *, SIZE_T);
struct DDICompilerDDIFuncs {
  DDIDestroyCompilerDDIFunc pfnDestroyCompilerDDI;
  DDIGetSupportedVersionsFunc pfnGetSupportedVersions;
  DDISetSelectedVersionFunc pfnSetSelectedVersion;
  DDIFillDDITableFunc pfnFillDDITable;
  DDISetCallbackTableFunc pfnSetCallbackDDITable;
};

using DDIGetCapsFunc = HRESULT(APIENTRY *)(
    DDIHandle, const DDICompilerTarget *, const DDICompilerApplicationDesc *,
    const void *);
using DDIEnumerateAdapterFamiliesFunc = HRESULT(APIENTRY *)(
    DDIHandle, UINT, DDICompilerAdapterFamily *);
using DDIGetAdapterFamilyABIVersionsFunc = HRESULT(APIENTRY *)(
    DDIHandle, UINT, UINT *, UINT64 *);
using DDIGetCompilerVersionFunc = HRESULT(APIENTRY *)(
    DDIHandle, UINT, DDICompilerVersionNumber *);
using DDIGetApplicationProfileVersionFunc = HRESULT(APIENTRY *)(
    DDIHandle, const DDICompilerTarget *, const DDICompilerApplicationDesc *,
    DDICompilerVersionNumber *);
using DDICheckFormatSupportFunc = HRESULT(APIENTRY *)(
    DDIHandle, const DDICompilerTarget *, DXGI_FORMAT, UINT *);
using DDICheckMultisampleQualityLevelsFunc = BOOL(APIENTRY *)(
    DDIHandle, const DDICompilerTarget *, DXGI_FORMAT, UINT, UINT);
struct DDICompilerCapabilitiesFuncs {
  DDIGetCapsFunc pfnGetCaps;
  DDIEnumerateAdapterFamiliesFunc pfnEnumerateAdapterFamilies;
  DDIGetAdapterFamilyABIVersionsFunc pfnGetAdapterFamilyABIVersions;
  DDIGetCompilerVersionFunc pfnGetCompilerVersion;
  DDIGetApplicationProfileVersionFunc pfnGetApplicationProfileVersion;
  DDICheckFormatSupportFunc pfnCheckFormatSupport;
  DDICheckMultisampleQualityLevelsFunc pfnCheckMultisampleQualityLevels;
};

using DDICalcPrivateCompilerSizeFunc = SIZE_T(APIENTRY *)(
    const DDICompilerTarget *, const DDICompilerApplicationDesc *);
using DDICreateCompilerFunc = HRESULT(APIENTRY *)(
    const DDICompilerTarget *, const DDICompilerApplicationDesc *, DDIHandle,
    DDIRuntimeHandle);
using DDIDestroyCompilerFunc = void(APIENTRY *)(DDIHandle);
using DDICompilePipelineStateFunc = HRESULT(APIENTRY *)(
    DDIHandle, DDIRuntimeHandle, UINT, const void *);
using DDICalcPrivateStateObjectSizeFunc = SIZE_T(APIENTRY *)(DDIHandle,
                                                             const void *);
using DDICompileCreateStateObjectFunc = HRESULT(APIENTRY *)(
    DDIHandle, DDIRuntimeHandle, UINT, const void *, DDIHandle);
using DDICalcPrivateAddToStateObjectSizeFunc = SIZE_T(APIENTRY *)(
    DDIHandle, const void *, DDIHandle);
using DDICompileAddToStateObjectFunc = HRESULT(APIENTRY *)(
    DDIHandle, DDIRuntimeHandle, UINT, const void *, DDIHandle, DDIHandle);
using DDIDestroyStateObjectFunc = void(APIENTRY *)(DDIHandle, DDIHandle);
struct DDICompilerFuncs {
  DDICalcPrivateCompilerSizeFunc pfnCalcPrivateCompilerSize;
  DDICreateCompilerFunc pfnCreateCompiler;
  DDIDestroyCompilerFunc pfnDestroyCompiler;
  DDICompilePipelineStateFunc pfnCompilePipelineState;
  DDICalcPrivateStateObjectSizeFunc pfnCalcPrivateStateObjectSize;
  DDICompileCreateStateObjectFunc pfnCompileCreateStateObject;
  DDICalcPrivateAddToStateObjectSizeFunc pfnCalcPrivateAddToStateObjectSize;
  DDICompileAddToStateObjectFunc pfnCompileAddToStateObject;
  DDIDestroyStateObjectFunc pfnDestroyStateObject;
};

constexpr UINT kDDITableCapabilities = 0;
constexpr UINT kDDITableCompiler = 1;
constexpr UINT kDDICallbackTableCache = 0;
struct DDIState {
  DDIRuntimeHandle runtime_handle = {nullptr};
  UINT64 selected_plugin_version = 0;
  UINT64 selected_usermode_version = 0;
  DDICompilerCacheCallbacks callbacks = {};
  bool callbacks_set = false;
};

struct DDICompilerObject {
  DDIState *state = nullptr;
  DDIRuntimeHandle runtime_handle = {nullptr};
};

struct DDIStateObject {
  DDICompilerObject *compiler = nullptr;
};

static DDIState *AsDDIState(DDIHandle handle) {
  return static_cast<DDIState *>(handle.pDrvPrivate);
}
static DDICompilerObject *AsDDICompiler(DDIHandle handle) {
  return static_cast<DDICompilerObject *>(handle.pDrvPrivate);
}
static bool IsValidDDITarget(const DDICompilerTarget *target) {
  return target && target->AdapterFamilyIndex == 0 &&
         (target->ABIVersion == 0 || target->ABIVersion == 1);
}

static const std::vector<UINT64> &SupportedPluginVersions() {
  // Agility 1.619.5's compiler host maps these two plugin DDI revisions to
  // the current usermode DDI table. Keep the list limited to versions the
  // plugin actually implements; advertising host-internal table revisions
  // would make negotiation succeed with the wrong shared structures.
  static const std::vector<UINT64> versions = {
      0x000c000000010000ull,
      0x000c000000020000ull,
  };
  return versions;
}

static HRESULT APIENTRY DDIGetSupportedVersions(DDIHandle handle,
                                                 UINT32 *entries,
                                                 UINT64 *versions) {

  if (!handle.pDrvPrivate || !entries)
    return E_INVALIDARG;
  const auto &supported = SupportedPluginVersions();
  if (!versions) {
    *entries = static_cast<UINT32>(supported.size());
    return S_OK;
  }
  UINT capacity = *entries;
  UINT count = static_cast<UINT>(supported.size());
  UINT copy_count = std::min(capacity, count);
  for (UINT i = 0; i < copy_count; ++i)
    versions[i] = supported[i];
  *entries = count;
  return capacity < count ? DXGI_ERROR_MORE_DATA : S_OK;
}

static HRESULT APIENTRY DDISetSelectedVersion(DDIHandle handle,
                                                UINT64 plugin_version,
                                                UINT64 usermode_version) {
  auto *state = AsDDIState(handle);
  const auto &supported = SupportedPluginVersions();
  if (!state || std::find(supported.begin(), supported.end(), plugin_version) ==
                    supported.end())
    return E_INVALIDARG;
  state->selected_plugin_version = plugin_version;
  state->selected_usermode_version = usermode_version;
  return S_OK;
}

static void APIENTRY DDIDestroyCompilerDDI(DDIHandle handle) {
  delete AsDDIState(handle);
}

static HRESULT APIENTRY DDISetCallbackDDITable(DDIHandle handle, UINT table_type,
                                                const void *table,
                                                SIZE_T table_size) {
  auto *state = AsDDIState(handle);
  if (!state || table_type != kDDICallbackTableCache || !table ||
      table_size < sizeof(DDICompilerCacheCallbacks))
    return E_INVALIDARG;
  memcpy(&state->callbacks, table, sizeof(state->callbacks));
  state->callbacks_set = true;
  return S_OK;
}

static HRESULT APIENTRY DDIEnumerateAdapterFamilies(
    DDIHandle handle, UINT index, DDICompilerAdapterFamily *family) {
  auto *state = AsDDIState(handle);
  if (!state || !family)
    return E_INVALIDARG;
  if (index != 0)
    return DXGI_ERROR_NOT_FOUND;
  memset(family, 0, sizeof(*family));
  const wchar_t name[] = L"Apple M4";
  std::wcsncpy(family->szAdapterFamily, name,
               sizeof(family->szAdapterFamily) / sizeof(wchar_t) - 1);
  return S_OK;
}

static HRESULT APIENTRY DDIGetAdapterFamilyABIVersions(
    DDIHandle handle, UINT index, UINT *entries, UINT64 *versions) {
  auto *state = AsDDIState(handle);
  if (!state || !entries)
    return E_INVALIDARG;
  if (index != 0) {
    *entries = 0;
    return DXGI_ERROR_NOT_FOUND;
  }
  if (!versions) {
    *entries = 1;
    return S_OK;
  }
  UINT capacity = *entries;
  *entries = 1;
  if (capacity < 1)
    return DXGI_ERROR_MORE_DATA;
  versions[0] = 1;
  return S_OK;
}

static HRESULT APIENTRY DDIGetCompilerVersion(
    DDIHandle handle, UINT index, DDICompilerVersionNumber *version) {
  auto *state = AsDDIState(handle);
  if (!state || !version)
    return E_INVALIDARG;
  if (index != 0)
    return DXGI_ERROR_NOT_FOUND;
  version->Version = 1;
  return S_OK;
}

static HRESULT APIENTRY DDIGetApplicationProfileVersion(
    DDIHandle handle, const DDICompilerTarget *target,
    const DDICompilerApplicationDesc *application,
    DDICompilerVersionNumber *version) {
  auto *state = AsDDIState(handle);
  if (!state || !IsValidDDITarget(target) || !application || !version)
    return E_INVALIDARG;
  version->Version = 1;
  return S_OK;
}

static HRESULT APIENTRY DDIGetCaps(DDIHandle handle,
                                   const DDICompilerTarget *target,
                                   const DDICompilerApplicationDesc *application,
                                   const void *caps) {
  const auto *args = static_cast<const DDIGetCapsArgsCompat *>(caps);
  auto *state = AsDDIState(handle);
  if (!state || !IsValidDDITarget(target) || !caps)
    return E_INVALIDARG;
  (void)application;
  if (!args->data)
    return E_INVALIDARG;
  if (args->type == 1074u) {
    if (args->data_size < sizeof(UINT) * 2)
      return E_INVALIDARG;
    struct PipelineSupportCompat {
      UINT highest_runtime_supported_feature_level;
      UINT maximum_driver_supported_feature_level;
    } support = {};
    memcpy(&support, args->data, sizeof(support));
    support.maximum_driver_supported_feature_level = 10u; // D3D12 11_0.
    memcpy(args->data, &support, sizeof(support));
    return S_OK;
  }
  if (args->type == 1012u) {
    if (args->data_size < sizeof(void *) * 2)
      return E_INVALIDARG;
    struct ShaderModelsCompat {
      UINT *count;
      UINT *models;
    } models = {};
    memcpy(&models, args->data, sizeof(models));
    if (!models.count)
      return E_INVALIDARG;
    constexpr UINT kShaderModels[] = {
        0x00050015u, // SM 5.1
        0x00060005u, 0x00060015u, 0x00060025u,
        0x00060035u, 0x00060045u, 0x00060055u,
        0x00060065u, 0x00060070u};
    constexpr UINT model_count = sizeof(kShaderModels) / sizeof(kShaderModels[0]);
    UINT capacity = *models.count;
    if (!models.models) {
      *models.count = model_count;
      return S_OK;
    }
    UINT copy_count = std::min(capacity, model_count);
    for (UINT i = 0; i < copy_count; ++i)
      models.models[i] = kShaderModels[i];
    *models.count = model_count;
    return capacity >= model_count ? S_OK : DXGI_ERROR_MORE_DATA;
  }
  if (args->type == 1004u) {
    // The selected 12.80/115 table asks for the extended shader-capability
    // record. Keep every field initialized; only the common capability bits
    // are advertised by this no-adapter compiler provider.
    memset(args->data, 0, args->data_size);
    if (args->data_size >= sizeof(UINT) * 6) {
      auto *words = static_cast<UINT *>(args->data);
      words[0] = 3u; // 10- and 16-bit minimum precision.
      // Optional shader capabilities remain conservative; regular DXIL/MSL
      // lowering is not the offline object-code compiler contract.
      words[1] = FALSE; // native binary64 ops.
      words[5] = FALSE; // WaveOps.
      if (args->data_size >= sizeof(UINT) * 9) {
        words[6] = 32u;
        words[7] = 32u;
        words[8] = 32u;
      }
    }
    return S_OK;
  }
  memset(args->data, 0, args->data_size);
  return S_OK;
}

static HRESULT APIENTRY DDICheckFormatSupport(
    DDIHandle handle, const DDICompilerTarget *target, DXGI_FORMAT format,
    UINT *support) {
  auto *state = AsDDIState(handle);
  if (!state || !IsValidDDITarget(target) || !support)
    return E_INVALIDARG;
  (void)format;
  *support = 0;
  return S_OK;
}

static BOOL APIENTRY DDICheckMultisampleQualityLevels(
    DDIHandle handle, const DDICompilerTarget *target, DXGI_FORMAT format,
    UINT sample_count, UINT quality_level) {
  auto *state = AsDDIState(handle);
  if (!state || !IsValidDDITarget(target))
    return FALSE;
  (void)format;
  return sample_count == 1 && quality_level == 0;
}

static SIZE_T APIENTRY DDICalcPrivateCompilerSize(
    const DDICompilerTarget *, const DDICompilerApplicationDesc *) {
  return sizeof(DDICompilerObject);
}

static HRESULT APIENTRY DDICreateCompiler(
    const DDICompilerTarget *, const DDICompilerApplicationDesc *,
    DDIHandle compiler_handle, DDIRuntimeHandle) {
  if (!compiler_handle.pDrvPrivate)
    return E_INVALIDARG;
  // No offline object-code compiler is exposed yet. Refuse compiler-object
  // creation rather than returning a token that could publish invalid bytes.
  return E_NOTIMPL;
}

static void APIENTRY DDIDestroyCompiler(DDIHandle compiler_handle) {
  if (auto *compiler = AsDDICompiler(compiler_handle))
    compiler->~DDICompilerObject();
}

static SIZE_T APIENTRY DDICalcPrivateStateObjectSize(DDIHandle compiler,
                                                      const void *create) {
  return AsDDICompiler(compiler) && create ? sizeof(DDIStateObject) : 0;
}
static SIZE_T APIENTRY DDICalcPrivateAddToStateObjectSize(
    DDIHandle compiler, const void *addition, DDIHandle existing) {
  return AsDDICompiler(compiler) && addition && existing.pDrvPrivate
             ? sizeof(DDIStateObject)
             : 0;
}

static HRESULT APIENTRY DDICompilePipelineState(
    DDIHandle compiler, DDIRuntimeHandle cache_session, UINT value_flags,
    const void *pipeline) {
  auto *object = AsDDICompiler(compiler);
  if (!object || !cache_session.handle || !pipeline || !value_flags)
    return E_INVALIDARG;
  // A valid object-code compiler is not available in the standalone DXMT
  // plugin boundary yet. Do not publish fabricated bytes as a successful
  // precompiled shader; the runtime will use the regular DXIL->MSL path.
  return E_NOTIMPL;
}

static HRESULT APIENTRY DDICompileCreateStateObject(
    DDIHandle compiler, DDIRuntimeHandle cache_session, UINT value_flags,
    const void *create, DDIHandle state_object_handle) {
  auto *object = AsDDICompiler(compiler);
  if (!object || !cache_session.handle || !value_flags || !create ||
      !state_object_handle.pDrvPrivate)
    return E_INVALIDARG;
  return E_NOTIMPL;
}

static HRESULT APIENTRY DDICompileAddToStateObject(
    DDIHandle compiler, DDIRuntimeHandle cache_session, UINT value_flags,
    const void *addition, DDIHandle existing, DDIHandle state_object) {
  auto *object = AsDDICompiler(compiler);
  if (!object || !cache_session.handle || !value_flags || !addition ||
      !existing.pDrvPrivate || !state_object.pDrvPrivate)
    return E_INVALIDARG;
  return E_NOTIMPL;
}

static void APIENTRY DDIDestroyStateObject(DDIHandle compiler,
                                            DDIHandle state_object) {
  if (!AsDDICompiler(compiler) || !state_object.pDrvPrivate)
    return;
  static_cast<DDIStateObject *>(state_object.pDrvPrivate)->~DDIStateObject();
}

static HRESULT APIENTRY DDIFillDDITable(DDIHandle handle, UINT table_type,
                                         void *table, SIZE_T table_size) {
  auto *state = AsDDIState(handle);
  if (!state || !state->selected_plugin_version || !table)
    return E_INVALIDARG;
  if (table_type == kDDITableCapabilities) {
    DDICompilerCapabilitiesFuncs funcs = {
        DDIGetCaps,
        DDIEnumerateAdapterFamilies,
        DDIGetAdapterFamilyABIVersions,
        DDIGetCompilerVersion,
        DDIGetApplicationProfileVersion,
        DDICheckFormatSupport,
        DDICheckMultisampleQualityLevels};
    if (table_size < sizeof(funcs))
      return E_INVALIDARG;
    memcpy(table, &funcs, sizeof(funcs));
    return S_OK;
  }
  if (table_type == kDDITableCompiler) {
    DDICompilerFuncs funcs = {
        DDICalcPrivateCompilerSize,
        DDICreateCompiler,
        DDIDestroyCompiler,
        DDICompilePipelineState,
        DDICalcPrivateStateObjectSize,
        DDICompileCreateStateObject,
        DDICalcPrivateAddToStateObjectSize,
        DDICompileAddToStateObject,
        DDIDestroyStateObject};
    if (table_size < sizeof(funcs))
      return E_INVALIDARG;
    memcpy(table, &funcs, sizeof(funcs));
    return S_OK;
  }
  return E_INVALIDARG;
}

} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI D3D12OpenCompilerDDI(
    void *open_args) {
  if (!open_args)
    return E_INVALIDARG;
  auto *args = static_cast<DDICompilerOpenArgs *>(open_args);
  if (!args->pDDIFuncs)
    return E_INVALIDARG;
  auto *state = new DDIState();
  state->runtime_handle = args->hRTCompilerDDI;
  args->hCompilerDDI.pDrvPrivate = state;
  DDICompilerDDIFuncs funcs = {
      DDIDestroyCompilerDDI, DDIGetSupportedVersions, DDISetSelectedVersion,
      DDIFillDDITable, DDISetCallbackDDITable};
  memcpy(args->pDDIFuncs, &funcs, sizeof(funcs));
  return S_OK;
}
