#define INITGUID
#include "d3d12_dxgi_device.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pipeline_state.hpp"
#include "d3d12_trace.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <d3d12.h>
#include <atomic>
#include <cstdarg>
#include <exception>
#include <vector>
#include <cstring>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace {

constexpr UINT kD3D12AgilitySDKVersion = 619;

constexpr GUID kCLSID_D3D12SDKConfiguration = {
    0x7cda6aca,
    0xa03e,
    0x49c8,
    {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
constexpr GUID kIID_ID3D12SDKConfiguration = {
    0xe9eb5314,
    0x33aa,
    0x42b2,
    {0xa7, 0x18, 0xd7, 0x7f, 0x58, 0xb1, 0xf1, 0xc7}};
constexpr GUID kIID_ID3D12SDKConfiguration1 = {
    0x8aaf9303,
    0xad25,
    0x48b9,
    {0x9a, 0x57, 0xd9, 0xc3, 0x7e, 0x00, 0x9d, 0x9f}};
constexpr GUID kCLSID_D3D12DeviceFactory = {
    0x114863bf,
    0xc386,
    0x4aee,
    {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};
constexpr GUID kIID_ID3D12DeviceFactory = {
    0x61f307d3,
    0xd34e,
    0x4e7c,
    {0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb}};
constexpr GUID kIID_ID3D12DeviceConfiguration = {
    0x78dbf87b,
    0xf766,
    0x422b,
    {0xa6, 0x1c, 0xc8, 0xc4, 0x46, 0xbd, 0xb9, 0xad}};
constexpr GUID kIID_ID3D12DeviceConfiguration1 = {
    0xed342442,
    0x6343,
    0x4e16,
    {0xbb, 0x82, 0xa3, 0xa5, 0x77, 0x87, 0x4e, 0x56}};
constexpr GUID kCLSID_D3D12StateObjectFactory = {
    0x54e1c9f3,
    0x1303,
    0x4112,
    {0xbf, 0x8e, 0x7b, 0xf2, 0xbb, 0x60, 0x6a, 0x73}};
constexpr GUID kCLSID_D3D12RuntimeValidationControl = {
    0xe5b53e74,
    0x3fca,
    0x47b4,
    {0x88, 0xb9, 0xa8, 0xb4, 0x1e, 0xf8, 0xfb, 0x73}};
constexpr GUID kCLSID_D3D12ApplicationIdentity = {
    0x08d8e1e8,
    0x75a6,
    0x42a7,
    {0xbf, 0x3a, 0xd0, 0x5f, 0xe5, 0x29, 0xc4, 0x7c}};
constexpr GUID kIID_ID3D12StateObjectDatabase = {
    0xc56060b7,
    0xb5fc,
    0x4135,
    {0x98, 0xe0, 0xa1, 0xe9, 0x99, 0x7e, 0xac, 0xe0}};
constexpr GUID kIID_ID3D12StateObjectDatabaseFactory = {
    0xf5b066f0,
    0x648a,
    0x4611,
    {0xbd, 0x41, 0x27, 0xfd, 0x09, 0x48, 0xb9, 0xeb}};
constexpr GUID kIID_ID3D12RuntimeValidationControl = {
    0xc706c811,
    0x3663,
    0x4bf1,
    {0x91, 0xb9, 0x1e, 0x8a, 0x7c, 0x11, 0x4a, 0xb9}};
constexpr GUID kIID_ID3D12ApplicationIdentity = {
    0x82dc6c85,
    0x727b,
    0x4a8d,
    {0x91, 0x69, 0xdb, 0x6c, 0xe3, 0xe9, 0x75, 0xa0}};

struct ID3D12SDKConfiguration : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT SDKVersion,
                                                  LPCSTR SDKPath) = 0;
};

struct ID3D12SDKConfiguration1 : public ID3D12SDKConfiguration {
  virtual HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT SDKVersion,
                                                        LPCSTR SDKPath,
                                                        REFIID riid,
                                                        void **ppvFactory) = 0;
  virtual void STDMETHODCALLTYPE FreeUnusedSDKs() = 0;
};

enum D3D12DeviceFactoryFlagsCompat : UINT {
  D3D12DeviceFactoryFlagNone = 0,
  D3D12DeviceFactoryFlagAllowReturningExistingDevice = 1,
  D3D12DeviceFactoryFlagAllowReturningIncompatibleExistingDevice = 2,
  D3D12DeviceFactoryFlagDisallowStoringNewDeviceAsSingleton = 4,
};

struct ID3D12DeviceFactoryCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() = 0;
  virtual HRESULT STDMETHODCALLTYPE ApplyToGlobalState() = 0;
  virtual HRESULT STDMETHODCALLTYPE
  SetFlags(D3D12DeviceFactoryFlagsCompat flags) = 0;
  virtual D3D12DeviceFactoryFlagsCompat STDMETHODCALLTYPE GetFlags() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid,
                                                              REFIID iid,
                                                              void **ppv) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(
      UINT num_features, const IID *iids, void *configuration_structs,
      UINT *configuration_struct_sizes) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL feature_level, REFIID riid,
               void **device) = 0;
};

enum D3D12DeviceFlagsCompat : UINT {
  D3D12DeviceFlagNone = 0,
  D3D12DeviceFlagDebugLayerEnabled = 1,
  D3D12DeviceFlagGPUBasedValidationEnabled = 2,
  D3D12DeviceFlagSynchronizedCommandQueueValidationDisabled = 4,
  D3D12DeviceFlagDREDAutoBreadcrumbsEnabled = 8,
  D3D12DeviceFlagDREDPageFaultReportingEnabled = 16,
  D3D12DeviceFlagDREDWatsonReportingEnabled = 32,
  D3D12DeviceFlagDREDBreadcrumbContextEnabled = 64,
  D3D12DeviceFlagDREDUseMarkersOnlyBreadcrumbs = 128,
  D3D12DeviceFlagShaderInstrumentationEnabled = 256,
  D3D12DeviceFlagAutoDebugNameEnabled = 512,
  D3D12DeviceFlagForceLegacyStateValidation = 1024,
};

struct D3D12DeviceConfigurationDescCompat {
  D3D12DeviceFlagsCompat Flags;
  UINT GpuBasedValidationFlags;
  UINT SDKVersion;
  UINT NumEnabledExperimentalFeatures;
};

struct ID3D12DeviceConfigurationCompat : public IUnknown {
  virtual D3D12DeviceConfigurationDescCompat *STDMETHODCALLTYPE
  GetDesc(D3D12DeviceConfigurationDescCompat *ret) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  GetEnabledExperimentalFeatures(GUID *guids, UINT num_guids) = 0;
  virtual HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **result,
      ID3DBlob **error) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(
      const void *blob, SIZE_T size, REFIID riid, void **deserializer) = 0;
};

struct ID3D12DeviceConfiguration1Compat
    : public ID3D12DeviceConfigurationCompat {
  virtual HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *library_blob, SIZE_T size,
      LPCWSTR root_signature_subobject_name, REFIID riid,
      void **deserializer) = 0;
};

struct ID3D12RuntimeValidationControlCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE
  DisableFailuresFromStricterValidationInAppLocalRuntime(BOOL disable) = 0;
  virtual BOOL STDMETHODCALLTYPE
  FailuresFromStricterValidationInAppLocalRuntimeDisabled() = 0;
};

union D3D12VersionNumberCompat {
  UINT64 Version;
  UINT16 VersionParts[4];
};

struct D3D12ApplicationDescCompat {
  LPCWSTR pExeFilename;
  LPCWSTR pName;
  D3D12VersionNumberCompat Version;
  LPCWSTR pEngineName;
  D3D12VersionNumberCompat EngineVersion;
};

struct ID3D12ApplicationIdentityCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetApplicationIdentity(
      const D3D12ApplicationDescCompat *desc, REFGUID app_id) = 0;
};

typedef void(STDMETHODCALLTYPE *D3D12ApplicationDescFuncCompat)(
    const D3D12ApplicationDescCompat *application_desc, void *context);
typedef void(STDMETHODCALLTYPE *D3D12PipelineStateFuncCompat)(
    const void *key, UINT key_size, UINT version,
    const D3D12_PIPELINE_STATE_STREAM_DESC *desc, void *context);
typedef void(STDMETHODCALLTYPE *D3D12StateObjectFuncCompat)(
    const void *key, UINT key_size, UINT version,
    const D3D12_STATE_OBJECT_DESC *desc, const void *parent_key,
    UINT parent_key_size, void *context);

enum D3D12StateObjectDatabaseFlagsCompat : UINT {
  D3D12StateObjectDatabaseFlagNone = 0,
  D3D12StateObjectDatabaseFlagReadOnly = 1,
};

struct ID3D12StateObjectDatabaseCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE
  SetApplicationDesc(const D3D12ApplicationDescCompat *application_desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetApplicationDesc(
      D3D12ApplicationDescFuncCompat callback, void *context) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  StorePipelineStateDesc(const void *key, UINT key_size, UINT version,
                         const D3D12_PIPELINE_STATE_STREAM_DESC *desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE FindPipelineStateDesc(
      const void *key, UINT key_size, D3D12PipelineStateFuncCompat callback,
      void *context) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  StoreStateObjectDesc(const void *key, UINT key_size, UINT version,
                       const D3D12_STATE_OBJECT_DESC *desc,
                       const void *parent_key, UINT parent_key_size) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  FindStateObjectDesc(const void *key, UINT key_size,
                      D3D12StateObjectFuncCompat callback, void *context) = 0;
  virtual HRESULT STDMETHODCALLTYPE FindObjectVersion(const void *key,
                                                      UINT key_size,
                                                      UINT *version) = 0;
};

struct ID3D12StateObjectDatabaseFactoryCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateStateObjectDatabaseFromFile(
      LPCWSTR database_file, D3D12StateObjectDatabaseFlagsCompat flags,
      REFIID riid, void **state_object_database) = 0;
};

static void TraceAgility(const char *fmt, ...) {
  FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
  if (!f)
    return;

  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fprintf(f, "\n");
  fclose(f);
}

struct AppLocalAgilityRuntimeState {
  bool checked = false;
  bool exe_exports_present = false;
  UINT sdk_version = 0;
  std::string sdk_path;
  std::string resolved_dir;
  HMODULE d3d12core = nullptr;
  HMODULE sdk_layers = nullptr;
  HMODULE state_object_compiler = nullptr;
};

static AppLocalAgilityRuntimeState &GetAppLocalAgilityRuntimeState() {
  static AppLocalAgilityRuntimeState state;
  return state;
}

static std::string NormalizeWindowsPath(std::string path) {
  for (char &ch : path) {
    if (ch == '/')
      ch = '\\';
  }
  return path;
}

static std::string WindowsParentPath(const std::string &path) {
  size_t pos = path.find_last_of("\\/");
  if (pos == std::string::npos)
    return std::string();
  return path.substr(0, pos);
}

static std::string JoinWindowsPath(const std::string &base,
                                   const std::string &child) {
  if (base.empty())
    return child;
  if (child.empty())
    return base;
  if (base.back() == '\\' || base.back() == '/')
    return base + child;
  return base + "\\" + child;
}

static std::string ResolveAppLocalSdkDir(const std::string &exe_path,
                                         std::string sdk_path) {
  sdk_path = NormalizeWindowsPath(std::move(sdk_path));
  while (sdk_path.rfind(".\\", 0) == 0) {
    sdk_path.erase(0, 2);
  }
  if (sdk_path.size() >= 2 && sdk_path[1] == ':')
    return sdk_path;
  return JoinWindowsPath(WindowsParentPath(exe_path), sdk_path);
}

static bool LooksLikeRelativeWindowsSdkPath(const char *path) {
  if (!path || !path[0])
    return false;
  return (path[0] == '.' && (path[1] == '\\' || path[1] == '/')) ||
         (std::strlen(path) > 2 && path[1] == ':');
}

static const char *ResolveSdkPathExportValue(FARPROC export_value) {
  if (!export_value)
    return nullptr;

  const char *direct = reinterpret_cast<const char *>(export_value);
  if (LooksLikeRelativeWindowsSdkPath(direct))
    return direct;

  const char *const *indirect =
      reinterpret_cast<const char *const *>(export_value);
  if (indirect && LooksLikeRelativeWindowsSdkPath(*indirect))
    return *indirect;

  return nullptr;
}

static void TryLoadAppLocalAgilityModule(const std::string &directory,
                                         const char *filename, HMODULE *out) {
  if (!out)
    return;
  const std::string path = JoinWindowsPath(directory, filename);
  SetLastError(ERROR_SUCCESS);
  *out = LoadLibraryA(path.c_str());
  TraceAgility("AppLocalAgility LoadLibrary path=%s -> handle=%p error=%lu",
               path.c_str(), *out, GetLastError());
}

static void EnsureAppLocalAgilityRuntimeLoaded() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto &state = GetAppLocalAgilityRuntimeState();
    state.checked = true;

    char exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
      TraceAgility("AppLocalAgility failed to resolve current exe path");
      return;
    }

    HMODULE exe_module = GetModuleHandleA(nullptr);
    if (!exe_module) {
      TraceAgility("AppLocalAgility current exe module handle is null");
      return;
    }

    auto *sdk_version =
        reinterpret_cast<UINT *>(GetProcAddress(exe_module, "D3D12SDKVersion"));
    FARPROC sdk_path_export = GetProcAddress(exe_module, "D3D12SDKPath");
    const char *sdk_path = ResolveSdkPathExportValue(sdk_path_export);
    if (!sdk_version || !sdk_path || !sdk_path[0]) {
      TraceAgility("AppLocalAgility exports absent version_ptr=%p "
                   "path_export=%p resolved_path=%p exe=%s",
                   sdk_version, sdk_path_export, sdk_path, exe_path);
      return;
    }

    state.exe_exports_present = true;
    state.sdk_version = *sdk_version;
    state.sdk_path = sdk_path;
    state.resolved_dir = ResolveAppLocalSdkDir(exe_path, state.sdk_path);

    TraceAgility("AppLocalAgility exports version=%u path=%s resolved_dir=%s",
                 state.sdk_version, state.sdk_path.c_str(),
                 state.resolved_dir.c_str());

    TryLoadAppLocalAgilityModule(state.resolved_dir, "D3D12Core.dll",
                                 &state.d3d12core);
    TryLoadAppLocalAgilityModule(state.resolved_dir, "d3d12SDKLayers.dll",
                                 &state.sdk_layers);
    TryLoadAppLocalAgilityModule(state.resolved_dir,
                                 "D3D12StateObjectCompiler.dll",
                                 &state.state_object_compiler);
  });
}

class MTLD3D12SDKConfiguration final : public ID3D12SDKConfiguration1 {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;

    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12SDKConfiguration ||
        riid == kIID_ID3D12SDKConfiguration1) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT SDKVersion,
                                          LPCSTR SDKPath) override {
    TraceAgility("ID3D12SDKConfiguration::SetSDKVersion version=%u path=%s "
                 "accepted_runtime=%u",
                 SDKVersion, SDKPath ? SDKPath : "(null)",
                 kD3D12AgilitySDKVersion);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT SDKVersion, LPCSTR SDKPath,
                                                REFIID riid,
                                                void **ppvFactory) override;

  void STDMETHODCALLTYPE FreeUnusedSDKs() override {
    TraceAgility("ID3D12SDKConfiguration1::FreeUnusedSDKs");
  }

private:
  std::atomic<ULONG> m_ref = {1};
};

class MTLD3D12DeviceFactory final : public ID3D12DeviceFactoryCompat {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12DeviceFactory) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() override {
    TraceAgility("DeviceFactory::InitializeFromGlobalState");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ApplyToGlobalState() override {
    TraceAgility("DeviceFactory::ApplyToGlobalState");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SetFlags(D3D12DeviceFactoryFlagsCompat flags) override {
    m_flags = flags;
    TraceAgility("DeviceFactory::SetFlags flags=0x%x", flags);
    return S_OK;
  }

  D3D12DeviceFactoryFlagsCompat STDMETHODCALLTYPE GetFlags() override {
    TraceAgility("DeviceFactory::GetFlags -> 0x%x", m_flags);
    return m_flags;
  }

  HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid,
                                                      REFIID iid,
                                                      void **ppv) override {
    if (!ppv)
      return E_POINTER;
    TraceAgility("DeviceFactory::GetConfigurationInterface clsid=%s iid=%s",
                 dxmt::str::format(clsid).c_str(),
                 dxmt::str::format(iid).c_str());
    if (iid == kIID_ID3D12DeviceConfiguration ||
        iid == kIID_ID3D12DeviceConfiguration1) {
      extern HRESULT CreateD3D12DeviceConfiguration(REFIID riid, void **ppv);
      return CreateD3D12DeviceConfiguration(iid, ppv);
    }
    return D3D12GetInterface(clsid, iid, ppv);
  }

  HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(UINT num_features,
                                                       const IID *iids, void *,
                                                       UINT *) override {
    if (num_features && !iids)
      return E_INVALIDARG;
    TraceAgility("DeviceFactory::EnableExperimentalFeatures count=%u",
                 num_features);
    for (UINT i = 0; i < num_features; i++) {
      TraceAgility("  feature[%u]=%s", i, dxmt::str::format(iids[i]).c_str());
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateDevice(IUnknown *adapter,
                                         D3D_FEATURE_LEVEL feature_level,
                                         REFIID riid, void **device) override {
    TraceAgility("DeviceFactory::CreateDevice adapter=%p FL=%d riid=%s",
                 adapter, feature_level, dxmt::str::format(riid).c_str());
    return D3D12CreateDevice(adapter, feature_level, riid, device);
  }

private:
  std::atomic<ULONG> m_ref = {1};
  D3D12DeviceFactoryFlagsCompat m_flags = D3D12DeviceFactoryFlagNone;
};

class MTLD3D12DeviceConfiguration final
    : public ID3D12DeviceConfiguration1Compat {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12DeviceConfiguration ||
        riid == kIID_ID3D12DeviceConfiguration1) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  D3D12DeviceConfigurationDescCompat *STDMETHODCALLTYPE
  GetDesc(D3D12DeviceConfigurationDescCompat *ret) override {
    if (!ret)
      return nullptr;
    ret->Flags = D3D12DeviceFlagNone;
    ret->GpuBasedValidationFlags = 0;
    ret->SDKVersion = kD3D12AgilitySDKVersion;
    ret->NumEnabledExperimentalFeatures = 0;
    TraceAgility("DeviceConfiguration::GetDesc sdk=%u",
                 kD3D12AgilitySDKVersion);
    return ret;
  }

  HRESULT STDMETHODCALLTYPE
  GetEnabledExperimentalFeatures(GUID *guids, UINT num_guids) override {
    if (num_guids && !guids)
      return E_POINTER;
    TraceAgility(
        "DeviceConfiguration::GetEnabledExperimentalFeatures count=%u -> S_OK",
        num_guids);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **result,
      ID3DBlob **error) override {
    TraceAgility(
        "DeviceConfiguration::SerializeVersionedRootSignature version=%u",
        desc ? desc->Version : 0);
    return D3D12SerializeVersionedRootSignature(desc, result, error);
  }

  HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(
      const void *blob, SIZE_T size, REFIID riid,
      void **deserializer) override {
    TraceAgility("DeviceConfiguration::"
                 "CreateVersionedRootSignatureDeserializer size=%zu riid=%s",
                 size, dxmt::str::format(riid).c_str());
    return D3D12CreateVersionedRootSignatureDeserializer(blob, size, riid,
                                                         deserializer);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *library_blob, SIZE_T size,
      LPCWSTR root_signature_subobject_name, REFIID riid,
      void **deserializer) override {
    TraceAgility("DeviceConfiguration::"
                 "CreateVersionedRootSignatureDeserializerFromSubobjectInLibrar"
                 "y size=%zu subobject=%ls riid=%s",
                 size,
                 root_signature_subobject_name ? root_signature_subobject_name
                                               : L"(null)",
                 dxmt::str::format(riid).c_str());
    return D3D12CreateVersionedRootSignatureDeserializer(library_blob, size,
                                                         riid, deserializer);
  }

private:
  std::atomic<ULONG> m_ref = {1};
};

HRESULT CreateD3D12DeviceConfiguration(REFIID riid, void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = nullptr;
  auto *configuration = new MTLD3D12DeviceConfiguration();
  HRESULT hr = configuration->QueryInterface(riid, ppv);
  configuration->Release();
  TraceAgility("CreateD3D12DeviceConfiguration riid=%s -> 0x%lx out=%p",
               dxmt::str::format(riid).c_str(), hr, ppv ? *ppv : nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12SDKConfiguration::CreateDeviceFactory(
    UINT SDKVersion, LPCSTR SDKPath, REFIID riid, void **ppvFactory) {
  if (!ppvFactory)
    return E_POINTER;

  *ppvFactory = nullptr;
  auto *factory = new MTLD3D12DeviceFactory();
  HRESULT hr = factory->QueryInterface(riid, ppvFactory);
  factory->Release();
  TraceAgility("ID3D12SDKConfiguration1::CreateDeviceFactory version=%u "
               "path=%s riid=%s -> 0x%lx out=%p",
               SDKVersion, SDKPath ? SDKPath : "(null)",
               dxmt::str::format(riid).c_str(), hr,
               ppvFactory ? *ppvFactory : nullptr);
  return hr;
}

constexpr uint32_t kStateDatabaseMagic = 0x31424453u; // SDB1
constexpr uint32_t kStateDatabaseVersion = 5u;
constexpr size_t kStateDatabaseMaxBytes = 16u * 1024u * 1024u;
constexpr uint32_t kStateDatabaseMaxEntries = 4096u;
constexpr uint32_t kStateDatabaseMaxKeyBytes = 4096u;
constexpr uint32_t kStateDatabaseMaxStringChars = 1u * 1024u * 1024u;
// These Agility state-database subobject values are newer than the vendored
// D3D12 header. They are descriptor-only forms and remain separate from the
// runtime pointer-bearing subobjects (1, 2, and 6).
constexpr UINT kStateSubobjectGlobalSerializedRootSignature = 31u;
constexpr UINT kStateSubobjectLocalSerializedRootSignature = 32u;
constexpr UINT kStateSubobjectExistingCollectionByKey = 36u;

struct D3D12SerializedRootSignatureDescCompat {
  const void *pSerializedBlob;
  SIZE_T SerializedBlobSizeInBytes;
};
struct D3D12GlobalSerializedRootSignatureCompat {
  D3D12SerializedRootSignatureDescCompat Desc;
};
struct D3D12LocalSerializedRootSignatureCompat {
  D3D12SerializedRootSignatureDescCompat Desc;
};
struct D3D12ExistingCollectionByKeyDescCompat {
  const void *pKey;
  UINT KeySize;
  UINT NumExports;
  const D3D12_EXPORT_DESC *pExports;
};

static void AppendStateDatabaseU32(std::vector<uint8_t> &data, uint32_t value) {
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value >> 16));
  data.push_back(static_cast<uint8_t>(value >> 24));
}

static void AppendStateDatabaseU64(std::vector<uint8_t> &data, uint64_t value) {
  AppendStateDatabaseU32(data, static_cast<uint32_t>(value));
  AppendStateDatabaseU32(data, static_cast<uint32_t>(value >> 32));
}

static void AppendStateDatabaseBytes(std::vector<uint8_t> &data,
                                     const void *bytes, size_t size) {
  const auto *begin = static_cast<const uint8_t *>(bytes);
  data.insert(data.end(), begin, begin + size);
}

static void AppendStateDatabaseString(std::vector<uint8_t> &data,
                                      const std::wstring &value) {
  AppendStateDatabaseU32(data, static_cast<uint32_t>(value.size()));
  AppendStateDatabaseBytes(data, value.data(), value.size() * sizeof(wchar_t));
}

static bool ReadStateDatabaseU32(const std::vector<uint8_t> &data, size_t &offset,
                                 uint32_t &value) {
  if (offset > data.size() || data.size() - offset < sizeof(uint32_t))
    return false;
  value = static_cast<uint32_t>(data[offset]) |
          (static_cast<uint32_t>(data[offset + 1]) << 8) |
          (static_cast<uint32_t>(data[offset + 2]) << 16) |
          (static_cast<uint32_t>(data[offset + 3]) << 24);
  offset += sizeof(uint32_t);
  return true;
}

static bool ReadStateDatabaseU64(const std::vector<uint8_t> &data, size_t &offset,
                                 uint64_t &value) {
  uint32_t low = 0, high = 0;
  if (!ReadStateDatabaseU32(data, offset, low) ||
      !ReadStateDatabaseU32(data, offset, high))
    return false;
  value = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
  return true;
}

static bool ReadStateDatabaseBytes(const std::vector<uint8_t> &data,
                                   size_t &offset, void *destination,
                                   size_t size) {
  if (offset > data.size() || data.size() - offset < size)
    return false;
  if (size)
    memcpy(destination, data.data() + offset, size);
  offset += size;
  return true;
}

static bool ReadStateDatabaseString(const std::vector<uint8_t> &data,
                                    size_t &offset, std::wstring &value) {
  uint32_t chars = 0;
  if (!ReadStateDatabaseU32(data, offset, chars) ||
      chars > kStateDatabaseMaxStringChars || offset > data.size() ||
      static_cast<size_t>(chars) >
          (data.size() - offset) / sizeof(wchar_t))
    return false;
  value.resize(chars);
  return ReadStateDatabaseBytes(data, offset, value.data(),
                                static_cast<size_t>(chars) * sizeof(wchar_t));
}

static HRESULT ReadStateDatabaseFile(LPCWSTR path, std::vector<uint8_t> &data) {
  if (!path || !*path)
    return E_INVALIDARG;
  HANDLE file = CreateFileW(path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return S_FALSE;
    return HRESULT_FROM_WIN32(error);
  }
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      static_cast<uint64_t>(size.QuadPart) > kStateDatabaseMaxBytes) {
    CloseHandle(file);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }
  data.resize(static_cast<size_t>(size.QuadPart));
  size_t offset = 0;
  while (offset < data.size()) {
    DWORD chunk = 0;
    const DWORD request = static_cast<DWORD>(std::min<size_t>(
        data.size() - offset, static_cast<size_t>(0x40000000u)));
    if (!ReadFile(file, data.data() + offset, request, &chunk, nullptr) ||
        chunk == 0) {
      const DWORD error = GetLastError();
      CloseHandle(file);
      return HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT);
    }
    offset += chunk;
  }
  CloseHandle(file);
  return S_OK;
}

static HRESULT WriteStateDatabaseFile(LPCWSTR path,
                                      const std::vector<uint8_t> &data) {
  if (!path || !*path || data.empty() || data.size() > kStateDatabaseMaxBytes)
    return E_INVALIDARG;
  std::wstring temporary(path);
  temporary += L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return HRESULT_FROM_WIN32(GetLastError());
  size_t offset = 0;
  HRESULT result = S_OK;
  while (offset < data.size()) {
    DWORD chunk = 0;
    const DWORD request = static_cast<DWORD>(std::min<size_t>(
        data.size() - offset, static_cast<size_t>(0x40000000u)));
    if (!WriteFile(file, data.data() + offset, request, &chunk, nullptr) ||
        chunk == 0) {
      result = HRESULT_FROM_WIN32(GetLastError());
      break;
    }
    offset += chunk;
  }
  if (SUCCEEDED(result) && !FlushFileBuffers(file))
    result = HRESULT_FROM_WIN32(GetLastError());
  CloseHandle(file);
  if (FAILED(result)) {
    DeleteFileW(temporary.c_str());
    return result;
  }
  if (!MoveFileExW(temporary.c_str(), path,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    result = HRESULT_FROM_WIN32(GetLastError());
    DeleteFileW(temporary.c_str());
  }
  return result;
}

class MTLD3D12StateObjectDatabase final
    : public ID3D12StateObjectDatabaseCompat {
public:
  HRESULT Initialize(LPCWSTR database_file,
                     D3D12StateObjectDatabaseFlagsCompat flags) {
    if (!database_file || !*database_file ||
        (flags & ~D3D12StateObjectDatabaseFlagReadOnly))
      return E_INVALIDARG;
    m_file_path = database_file;
    m_read_only = (flags & D3D12StateObjectDatabaseFlagReadOnly) != 0;
    return Load();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12StateObjectDatabase) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  HRESULT STDMETHODCALLTYPE SetApplicationDesc(
      const D3D12ApplicationDescCompat *application_desc) override {
    if (!application_desc)
      return E_INVALIDARG;
    if (m_read_only)
      return E_ACCESSDENIED;
    m_application_desc = *application_desc;
    CopyApplicationString(application_desc->pExeFilename, m_exe_filename,
                          m_application_desc.pExeFilename);
    CopyApplicationString(application_desc->pName, m_application_name,
                          m_application_desc.pName);
    CopyApplicationString(application_desc->pEngineName, m_engine_name,
                          m_application_desc.pEngineName);
    m_has_application_desc = true;
    TraceAgility("StateObjectDatabase::SetApplicationDesc name=%ls engine=%ls",
                 m_application_desc.pName ? m_application_desc.pName
                                          : L"(null)",
                 m_application_desc.pEngineName ? m_application_desc.pEngineName
                                                : L"(null)");
    return Persist();
  }

  HRESULT STDMETHODCALLTYPE GetApplicationDesc(
      D3D12ApplicationDescFuncCompat callback, void *context) override {
    if (!callback)
      return E_POINTER;
    if (!m_has_application_desc)
      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    callback(&m_application_desc, context);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE StorePipelineStateDesc(
      const void *key, UINT key_size, UINT version,
      const D3D12_PIPELINE_STATE_STREAM_DESC *desc) override {
    if (!key || !key_size || !desc || !desc->pPipelineStateSubobjectStream ||
        !desc->SizeInBytes)
      return E_INVALIDARG;
    if (m_read_only)
      return E_ACCESSDENIED;
    if (key_size > kStateDatabaseMaxKeyBytes ||
        desc->SizeInBytes > kStateDatabaseMaxBytes)
      return E_INVALIDARG;
    auto key_bytes = MakeKey(key, key_size);
    auto &entry = m_pipeline_descs[key_bytes];
    entry.version = version;
    auto *stream =
        static_cast<const uint8_t *>(desc->pPipelineStateSubobjectStream);
    entry.stream.assign(stream, stream + desc->SizeInBytes);
    TraceAgility("StateObjectDatabase::StorePipelineStateDesc key_size=%u "
                 "version=%u bytes=%zu",
                 key_size, version, desc->SizeInBytes);
    return Persist();
  }

  HRESULT STDMETHODCALLTYPE FindPipelineStateDesc(
      const void *key, UINT key_size, D3D12PipelineStateFuncCompat callback,
      void *context) override {
    if (!key || !key_size)
      return E_INVALIDARG;
    if (!callback)
      return E_POINTER;

    auto key_bytes = MakeKey(key, key_size);
    auto entry = m_pipeline_descs.find(key_bytes);
    if (entry == m_pipeline_descs.end()) {
      TraceAgility("StateObjectDatabase::FindPipelineStateDesc key_size=%u -> "
                   "not found",
                   key_size);
      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    D3D12_PIPELINE_STATE_STREAM_DESC desc = {};
    desc.SizeInBytes = entry->second.stream.size();
    desc.pPipelineStateSubobjectStream = entry->second.stream.data();
    callback(entry->first.data(), static_cast<UINT>(entry->first.size()),
             entry->second.version, &desc, context);
    TraceAgility("StateObjectDatabase::FindPipelineStateDesc key_size=%u -> "
                 "hit version=%u bytes=%zu",
                 key_size, entry->second.version, desc.SizeInBytes);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE StoreStateObjectDesc(
      const void *key, UINT key_size, UINT version,
      const D3D12_STATE_OBJECT_DESC *desc, const void *parent_key,
      UINT parent_key_size) override {
    if (!key || !key_size || !desc || (parent_key_size && !parent_key))
      return E_INVALIDARG;
    if (m_read_only)
      return E_ACCESSDENIED;
    if (key_size > kStateDatabaseMaxKeyBytes ||
        parent_key_size > kStateDatabaseMaxKeyBytes ||
        desc->NumSubobjects > 64u)
      return E_INVALIDARG;
    if (desc->NumSubobjects && !desc->pSubobjects)
      return E_INVALIDARG;

    StateObjectDescEntry entry;
    entry.version = version;
    entry.type = desc->Type;
    entry.subobjects.reserve(desc->NumSubobjects);
    for (UINT i = 0; i < desc->NumSubobjects; ++i) {
      const auto &subobject = desc->pSubobjects[i];
      if (!subobject.pDesc)
        return E_INVALIDARG;
      StateObjectSubobjectEntry stored;
      stored.type = subobject.Type;
      if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
        const auto *library =
            static_cast<const D3D12_DXIL_LIBRARY_DESC *>(subobject.pDesc);
        if (!library->DXILLibrary.pShaderBytecode ||
            !library->DXILLibrary.BytecodeLength ||
            library->DXILLibrary.BytecodeLength > kStateDatabaseMaxBytes ||
            library->NumExports > 64u ||
            (library->NumExports && !library->pExports))
          return E_INVALIDARG;
        const auto *bytes = static_cast<const uint8_t *>(
            library->DXILLibrary.pShaderBytecode);
        stored.library.assign(
            bytes, bytes + library->DXILLibrary.BytecodeLength);
        stored.exports.reserve(library->NumExports);
        for (UINT export_index = 0; export_index < library->NumExports;
             ++export_index) {
          const auto &export_desc = library->pExports[export_index];
          if (!export_desc.Name)
            return E_INVALIDARG;
          StateObjectExportEntry export_entry;
          export_entry.name = export_desc.Name;
          if (export_desc.ExportToRename)
            export_entry.rename = export_desc.ExportToRename;
          export_entry.flags = static_cast<UINT>(export_desc.Flags);
          stored.exports.push_back(std::move(export_entry));
        }
      } else if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP) {
        const auto *hit_group =
            static_cast<const D3D12_HIT_GROUP_DESC *>(subobject.pDesc);
        if (!hit_group->HitGroupExport)
          return E_INVALIDARG;
        stored.hit_group_type = static_cast<UINT>(hit_group->Type);
        stored.hit_group_export = hit_group->HitGroupExport;
        if (hit_group->AnyHitShaderImport)
          stored.hit_group_any_hit = hit_group->AnyHitShaderImport;
        if (hit_group->ClosestHitShaderImport)
          stored.hit_group_closest_hit = hit_group->ClosestHitShaderImport;
        if (hit_group->IntersectionShaderImport)
          stored.hit_group_intersection = hit_group->IntersectionShaderImport;
      } else if (subobject.Type ==
                 D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        const auto *association =
            static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(
                subobject.pDesc);
        if (!association->pSubobjectToAssociate ||
            association->NumExports > 64u ||
            (association->NumExports && !association->pExports))
          return E_INVALIDARG;
        const uintptr_t array_begin = reinterpret_cast<uintptr_t>(
            desc->pSubobjects);
        const uintptr_t array_end =
            array_begin + sizeof(D3D12_STATE_SUBOBJECT) * desc->NumSubobjects;
        const uintptr_t target = reinterpret_cast<uintptr_t>(
            association->pSubobjectToAssociate);
        if (target < array_begin || target >= array_end ||
            (target - array_begin) % sizeof(D3D12_STATE_SUBOBJECT) != 0)
          return E_INVALIDARG;
        stored.subobject_association_target = static_cast<UINT>(
            (target - array_begin) / sizeof(D3D12_STATE_SUBOBJECT));
        stored.subobject_association_exports.reserve(association->NumExports);
        for (UINT export_index = 0; export_index < association->NumExports;
             ++export_index) {
          if (!association->pExports[export_index])
            return E_INVALIDARG;
          stored.subobject_association_exports.emplace_back(
              association->pExports[export_index]);
        }
      } else if (subobject.Type ==
                 D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        const auto *association =
            static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(
                subobject.pDesc);
        if (!association->SubobjectToAssociate ||
            association->NumExports > 64u ||
            (association->NumExports && !association->pExports))
          return E_INVALIDARG;
        stored.dxil_association_target = association->SubobjectToAssociate;
        stored.dxil_association_exports.reserve(association->NumExports);
        for (UINT export_index = 0; export_index < association->NumExports;
             ++export_index) {
          if (!association->pExports[export_index])
            return E_INVALIDARG;
          stored.dxil_association_exports.emplace_back(
              association->pExports[export_index]);
        }
      } else if (static_cast<UINT>(subobject.Type) ==
                     kStateSubobjectGlobalSerializedRootSignature ||
                 static_cast<UINT>(subobject.Type) ==
                     kStateSubobjectLocalSerializedRootSignature) {
        const auto *serialized =
            static_cast<const D3D12GlobalSerializedRootSignatureCompat *>(
                subobject.pDesc);
        if (!serialized->Desc.pSerializedBlob ||
            !serialized->Desc.SerializedBlobSizeInBytes ||
            serialized->Desc.SerializedBlobSizeInBytes > kStateDatabaseMaxBytes)
          return E_INVALIDARG;
        const auto *bytes = static_cast<const uint8_t *>(
            serialized->Desc.pSerializedBlob);
        stored.serialized_root_signature.assign(
            bytes, bytes + serialized->Desc.SerializedBlobSizeInBytes);
      } else if (static_cast<UINT>(subobject.Type) ==
                 kStateSubobjectExistingCollectionByKey) {
        const auto *collection =
            static_cast<const D3D12ExistingCollectionByKeyDescCompat *>(
                subobject.pDesc);
        if (!collection->pKey || !collection->KeySize ||
            collection->KeySize > kStateDatabaseMaxKeyBytes ||
            collection->NumExports > 64u ||
            (collection->NumExports && !collection->pExports))
          return E_INVALIDARG;
        const auto *key = static_cast<const uint8_t *>(collection->pKey);
        stored.existing_collection_key.assign(
            key, key + collection->KeySize);
        stored.existing_collection_exports.reserve(collection->NumExports);
        for (UINT export_index = 0; export_index < collection->NumExports;
             ++export_index) {
          const auto &export_desc = collection->pExports[export_index];
          if (!export_desc.Name)
            return E_INVALIDARG;
          StateObjectExportEntry export_entry;
          export_entry.name = export_desc.Name;
          if (export_desc.ExportToRename)
            export_entry.rename = export_desc.ExportToRename;
          export_entry.flags = static_cast<UINT>(export_desc.Flags);
          stored.existing_collection_exports.push_back(
              std::move(export_entry));
        }
      } else {
        const size_t desc_size = StateSubobjectDescSize(subobject.Type);
        if (!desc_size) {
          TraceAgility("StateObjectDatabase::StoreStateObjectDesc key_size=%u "
                       "version=%u subobject=%u type=%u -> E_NOTIMPL",
                       key_size, version, i,
                       static_cast<UINT>(subobject.Type));
          return E_NOTIMPL;
        }
        const auto *bytes = static_cast<const uint8_t *>(subobject.pDesc);
        stored.desc.assign(bytes, bytes + desc_size);
      }
      entry.subobjects.push_back(std::move(stored));
    }
    if (parent_key_size) {
      const auto *parent = static_cast<const uint8_t *>(parent_key);
      entry.parent_key.assign(parent, parent + parent_key_size);
    }
    m_state_object_descs[MakeKey(key, key_size)] = std::move(entry);
    TraceAgility("StateObjectDatabase::StoreStateObjectDesc key_size=%u "
                 "version=%u type=%u subobjects=%u parent_key_size=%u",
                 key_size, version, static_cast<UINT>(desc->Type),
                 desc->NumSubobjects, parent_key_size);
    return Persist();
  }

  HRESULT STDMETHODCALLTYPE FindStateObjectDesc(
      const void *key, UINT key_size, D3D12StateObjectFuncCompat callback,
      void *context) override {
    if (!key || !key_size)
      return E_INVALIDARG;
    if (!callback)
      return E_POINTER;
    auto key_bytes = MakeKey(key, key_size);
    auto entry = m_state_object_descs.find(key_bytes);
    if (entry == m_state_object_descs.end())
      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    std::vector<D3D12_DXIL_LIBRARY_DESC> libraries;
    std::vector<std::vector<D3D12_EXPORT_DESC>> library_exports;
    std::vector<D3D12GlobalSerializedRootSignatureCompat>
        global_serialized_roots;
    std::vector<D3D12LocalSerializedRootSignatureCompat>
        local_serialized_roots;
    std::vector<D3D12ExistingCollectionByKeyDescCompat>
        existing_collections_by_key;
    std::vector<std::vector<D3D12_EXPORT_DESC>>
        existing_collection_by_key_exports;
    std::vector<D3D12_HIT_GROUP_DESC> hit_groups;
    std::vector<D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION> dxil_associations;
    std::vector<std::vector<const WCHAR *>> association_exports;
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> subobject_associations;
    std::vector<std::vector<const WCHAR *>> subobject_association_exports;
    std::vector<UINT> subobject_association_targets;
    subobjects.reserve(entry->second.subobjects.size());
    libraries.reserve(entry->second.subobjects.size());
    library_exports.reserve(entry->second.subobjects.size());
    global_serialized_roots.reserve(entry->second.subobjects.size());
    local_serialized_roots.reserve(entry->second.subobjects.size());
    existing_collections_by_key.reserve(entry->second.subobjects.size());
    existing_collection_by_key_exports.reserve(entry->second.subobjects.size());
    hit_groups.reserve(entry->second.subobjects.size());
    dxil_associations.reserve(entry->second.subobjects.size());
    association_exports.reserve(entry->second.subobjects.size());
    subobject_associations.reserve(entry->second.subobjects.size());
    subobject_association_exports.reserve(entry->second.subobjects.size());
    subobject_association_targets.reserve(entry->second.subobjects.size());
    for (const auto &stored : entry->second.subobjects) {
      D3D12_STATE_SUBOBJECT subobject = {};
      subobject.Type = stored.type;
      if (stored.type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
        libraries.emplace_back();
        library_exports.emplace_back();
        auto &library = libraries.back();
        auto &exports = library_exports.back();
        exports.reserve(stored.exports.size());
        library.DXILLibrary.pShaderBytecode = stored.library.data();
        library.DXILLibrary.BytecodeLength = stored.library.size();
        library.NumExports = static_cast<UINT>(stored.exports.size());
        for (const auto &stored_export : stored.exports) {
          D3D12_EXPORT_DESC export_desc = {};
          export_desc.Name = stored_export.name.empty()
                                ? nullptr
                                : stored_export.name.c_str();
          export_desc.ExportToRename = stored_export.rename.empty()
                                           ? nullptr
                                           : stored_export.rename.c_str();
          export_desc.Flags = static_cast<D3D12_EXPORT_FLAGS>(
              stored_export.flags);
          exports.push_back(export_desc);
        }
        library.pExports = exports.empty() ? nullptr : exports.data();
        subobject.pDesc = &library;
      } else if (static_cast<UINT>(stored.type) ==
                 kStateSubobjectGlobalSerializedRootSignature) {
        global_serialized_roots.emplace_back();
        auto &serialized = global_serialized_roots.back();
        serialized.Desc.pSerializedBlob =
            stored.serialized_root_signature.data();
        serialized.Desc.SerializedBlobSizeInBytes =
            stored.serialized_root_signature.size();
        subobject.pDesc = &serialized;
      } else if (static_cast<UINT>(stored.type) ==
                 kStateSubobjectLocalSerializedRootSignature) {
        local_serialized_roots.emplace_back();
        auto &serialized = local_serialized_roots.back();
        serialized.Desc.pSerializedBlob =
            stored.serialized_root_signature.data();
        serialized.Desc.SerializedBlobSizeInBytes =
            stored.serialized_root_signature.size();
        subobject.pDesc = &serialized;
      } else if (static_cast<UINT>(stored.type) ==
                 kStateSubobjectExistingCollectionByKey) {
        existing_collections_by_key.emplace_back();
        existing_collection_by_key_exports.emplace_back();
        auto &collection = existing_collections_by_key.back();
        auto &exports = existing_collection_by_key_exports.back();
        collection.pKey = stored.existing_collection_key.data();
        collection.KeySize =
            static_cast<UINT>(stored.existing_collection_key.size());
        collection.NumExports =
            static_cast<UINT>(stored.existing_collection_exports.size());
        exports.reserve(stored.existing_collection_exports.size());
        for (const auto &stored_export :
             stored.existing_collection_exports) {
          D3D12_EXPORT_DESC export_desc = {};
          export_desc.Name = stored_export.name.empty()
                                ? nullptr
                                : stored_export.name.c_str();
          export_desc.ExportToRename = stored_export.rename.empty()
                                           ? nullptr
                                           : stored_export.rename.c_str();
          export_desc.Flags = static_cast<D3D12_EXPORT_FLAGS>(
              stored_export.flags);
          exports.push_back(export_desc);
        }
        collection.pExports = exports.empty() ? nullptr : exports.data();
        subobject.pDesc = &collection;
      } else if (stored.type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP) {
        hit_groups.emplace_back();
        auto &hit_group = hit_groups.back();
        hit_group.HitGroupExport = stored.hit_group_export.c_str();
        hit_group.Type = static_cast<D3D12_HIT_GROUP_TYPE>(
            stored.hit_group_type);
        hit_group.AnyHitShaderImport =
            stored.hit_group_any_hit.empty()
                ? nullptr
                : stored.hit_group_any_hit.c_str();
        hit_group.ClosestHitShaderImport =
            stored.hit_group_closest_hit.empty()
                ? nullptr
                : stored.hit_group_closest_hit.c_str();
        hit_group.IntersectionShaderImport =
            stored.hit_group_intersection.empty()
                ? nullptr
                : stored.hit_group_intersection.c_str();
        subobject.pDesc = &hit_group;
      } else if (stored.type ==
                 D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        dxil_associations.emplace_back();
        association_exports.emplace_back();
        auto &association = dxil_associations.back();
        auto &exports = association_exports.back();
        exports.reserve(stored.dxil_association_exports.size());
        association.SubobjectToAssociate =
            stored.dxil_association_target.c_str();
        association.NumExports =
            static_cast<UINT>(stored.dxil_association_exports.size());
        for (const auto &export_name : stored.dxil_association_exports)
          exports.push_back(export_name.c_str());
        association.pExports = exports.empty() ? nullptr : exports.data();
        subobject.pDesc = &association;
      } else if (stored.type ==
                 D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        subobject_associations.emplace_back();
        subobject_association_exports.emplace_back();
        subobject_association_targets.push_back(
            stored.subobject_association_target);
        auto &association = subobject_associations.back();
        auto &exports = subobject_association_exports.back();
        exports.reserve(stored.subobject_association_exports.size());
        association.NumExports = static_cast<UINT>(
            stored.subobject_association_exports.size());
        for (const auto &export_name : stored.subobject_association_exports)
          exports.push_back(export_name.c_str());
        association.pExports = exports.empty() ? nullptr : exports.data();
        subobject.pDesc = &association;
      } else {
        subobject.pDesc = stored.desc.data();
      }
      subobjects.push_back(subobject);
    }
    for (size_t i = 0; i < subobject_associations.size(); ++i) {
      const UINT target = subobject_association_targets[i];
      subobject_associations[i].pSubobjectToAssociate =
          target < subobjects.size() ? &subobjects[target] : nullptr;
    }
    D3D12_STATE_OBJECT_DESC desc = {};
    desc.Type = entry->second.type;
    desc.NumSubobjects = static_cast<UINT>(subobjects.size());
    desc.pSubobjects = subobjects.data();
    callback(entry->first.data(), static_cast<UINT>(entry->first.size()),
             entry->second.version, &desc,
             entry->second.parent_key.empty()
                 ? nullptr
                 : entry->second.parent_key.data(),
             static_cast<UINT>(entry->second.parent_key.size()), context);
    TraceAgility("StateObjectDatabase::FindStateObjectDesc key_size=%u -> "
                 "hit version=%u type=%u subobjects=%zu parent_key_size=%zu",
                 key_size, entry->second.version,
                 static_cast<UINT>(entry->second.type), subobjects.size(),
                 entry->second.parent_key.size());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE FindObjectVersion(const void *key, UINT key_size,
                                              UINT *version) override {
    if (!key || !key_size || !version)
      return E_INVALIDARG;
    auto key_bytes = MakeKey(key, key_size);
    auto pipeline = m_pipeline_descs.find(key_bytes);
    if (pipeline != m_pipeline_descs.end()) {
      *version = pipeline->second.version;
      return S_OK;
    }
    auto state_object = m_state_object_descs.find(key_bytes);
    if (state_object != m_state_object_descs.end()) {
      *version = state_object->second.version;
      return S_OK;
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  }

private:
  struct PipelineDescEntry {
    UINT version = 0;
    std::vector<uint8_t> stream;
  };

  struct StateObjectExportEntry {
    std::wstring name;
    std::wstring rename;
    UINT flags = 0;
  };

  struct StateObjectSubobjectEntry {
    D3D12_STATE_SUBOBJECT_TYPE type = D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG;
    std::vector<uint8_t> desc;
    std::vector<uint8_t> library;
    std::vector<uint8_t> serialized_root_signature;
    std::vector<uint8_t> existing_collection_key;
    std::vector<StateObjectExportEntry> exports;
    std::vector<StateObjectExportEntry> existing_collection_exports;
    UINT hit_group_type = 0;
    std::wstring hit_group_export;
    std::wstring hit_group_any_hit;
    std::wstring hit_group_closest_hit;
    std::wstring hit_group_intersection;
    std::wstring dxil_association_target;
    std::vector<std::wstring> dxil_association_exports;
    UINT subobject_association_target = UINT_MAX;
    std::vector<std::wstring> subobject_association_exports;
  };

  struct StateObjectDescEntry {
    UINT version = 0;
    D3D12_STATE_OBJECT_TYPE type = D3D12_STATE_OBJECT_TYPE_COLLECTION;
    std::vector<StateObjectSubobjectEntry> subobjects;
    std::vector<uint8_t> parent_key;
  };

  static void CopyApplicationString(LPCWSTR source, std::wstring &storage,
                                    LPCWSTR &destination) {
    if (!source) {
      storage.clear();
      destination = nullptr;
      return;
    }
    storage.assign(source);
    destination = storage.c_str();
  }

  static size_t StateSubobjectDescSize(D3D12_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
    case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
      return sizeof(D3D12_STATE_OBJECT_CONFIG);
    case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
      return sizeof(D3D12_NODE_MASK);
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
      return sizeof(D3D12_RAYTRACING_SHADER_CONFIG);
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
      return sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG);
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
      return sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG1);
    default:
      return 0;
    }
  }

  std::vector<uint8_t> Serialize() const {
    std::vector<uint8_t> data;
    data.reserve(1024);
    AppendStateDatabaseU32(data, kStateDatabaseMagic);
    AppendStateDatabaseU32(data, kStateDatabaseVersion);
    AppendStateDatabaseU32(data, m_has_application_desc ? 1u : 0u);
    if (m_has_application_desc) {
      AppendStateDatabaseString(data, m_exe_filename);
      AppendStateDatabaseString(data, m_application_name);
      AppendStateDatabaseU64(data, m_application_desc.Version.Version);
      AppendStateDatabaseString(data, m_engine_name);
      AppendStateDatabaseU64(data, m_application_desc.EngineVersion.Version);
    }
    AppendStateDatabaseU32(data, static_cast<uint32_t>(m_pipeline_descs.size()));
    for (const auto &item : m_pipeline_descs) {
      AppendStateDatabaseU32(data, static_cast<uint32_t>(item.first.size()));
      AppendStateDatabaseBytes(data, item.first.data(), item.first.size());
      AppendStateDatabaseU32(data, item.second.version);
      AppendStateDatabaseU32(data, static_cast<uint32_t>(item.second.stream.size()));
      AppendStateDatabaseBytes(data, item.second.stream.data(),
                               item.second.stream.size());
    }
    AppendStateDatabaseU32(data,
                           static_cast<uint32_t>(m_state_object_descs.size()));
    for (const auto &item : m_state_object_descs) {
      AppendStateDatabaseU32(data, static_cast<uint32_t>(item.first.size()));
      AppendStateDatabaseBytes(data, item.first.data(), item.first.size());
      AppendStateDatabaseU32(data, item.second.version);
      AppendStateDatabaseU32(data, static_cast<uint32_t>(item.second.type));
      AppendStateDatabaseU32(
          data, static_cast<uint32_t>(item.second.subobjects.size()));
      for (const auto &subobject : item.second.subobjects) {
        AppendStateDatabaseU32(data, static_cast<uint32_t>(subobject.type));
        if (subobject.type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
          AppendStateDatabaseU32(data,
                                 static_cast<uint32_t>(subobject.library.size()));
          AppendStateDatabaseBytes(data, subobject.library.data(),
                                   subobject.library.size());
          AppendStateDatabaseU32(
              data, static_cast<uint32_t>(subobject.exports.size()));
          for (const auto &export_entry : subobject.exports) {
            AppendStateDatabaseString(data, export_entry.name);
            AppendStateDatabaseString(data, export_entry.rename);
            AppendStateDatabaseU32(data, export_entry.flags);
          }
        } else if (subobject.type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP) {
          AppendStateDatabaseU32(data, subobject.hit_group_type);
          AppendStateDatabaseString(data, subobject.hit_group_export);
          AppendStateDatabaseString(data, subobject.hit_group_any_hit);
          AppendStateDatabaseString(data, subobject.hit_group_closest_hit);
          AppendStateDatabaseString(data, subobject.hit_group_intersection);
        } else if (subobject.type ==
                   D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
          AppendStateDatabaseU32(data, subobject.subobject_association_target);
          AppendStateDatabaseU32(
              data, static_cast<uint32_t>(subobject.subobject_association_exports.size()));
          for (const auto &export_name : subobject.subobject_association_exports)
            AppendStateDatabaseString(data, export_name);
        } else if (subobject.type ==
                   D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
          AppendStateDatabaseString(data, subobject.dxil_association_target);
          AppendStateDatabaseU32(
              data, static_cast<uint32_t>(subobject.dxil_association_exports.size()));
          for (const auto &export_name : subobject.dxil_association_exports)
            AppendStateDatabaseString(data, export_name);
        } else if (static_cast<UINT>(subobject.type) ==
                       kStateSubobjectGlobalSerializedRootSignature ||
                   static_cast<UINT>(subobject.type) ==
                       kStateSubobjectLocalSerializedRootSignature) {
          AppendStateDatabaseU32(
              data, static_cast<uint32_t>(subobject.serialized_root_signature.size()));
          AppendStateDatabaseBytes(data, subobject.serialized_root_signature.data(),
                                   subobject.serialized_root_signature.size());
        } else if (static_cast<UINT>(subobject.type) ==
                   kStateSubobjectExistingCollectionByKey) {
          AppendStateDatabaseU32(
              data, static_cast<uint32_t>(subobject.existing_collection_key.size()));
          AppendStateDatabaseBytes(data, subobject.existing_collection_key.data(),
                                   subobject.existing_collection_key.size());
          AppendStateDatabaseU32(
              data, static_cast<uint32_t>(subobject.existing_collection_exports.size()));
          for (const auto &export_entry : subobject.existing_collection_exports) {
            AppendStateDatabaseString(data, export_entry.name);
            AppendStateDatabaseString(data, export_entry.rename);
            AppendStateDatabaseU32(data, export_entry.flags);
          }
        } else {
          AppendStateDatabaseU32(data,
                                 static_cast<uint32_t>(subobject.desc.size()));
          AppendStateDatabaseBytes(data, subobject.desc.data(),
                                   subobject.desc.size());
        }
      }
      AppendStateDatabaseU32(data,
                             static_cast<uint32_t>(item.second.parent_key.size()));
      AppendStateDatabaseBytes(data, item.second.parent_key.data(),
                               item.second.parent_key.size());
    }
    return data;
  }

  HRESULT Persist() const {
    if (m_file_path.empty())
      return S_OK;
    return WriteStateDatabaseFile(m_file_path.c_str(), Serialize());
  }

  HRESULT Load() {
    std::vector<uint8_t> data;
    HRESULT result = ReadStateDatabaseFile(m_file_path.c_str(), data);
    if (result == S_FALSE)
      return S_OK;
    if (FAILED(result))
      return result;
    size_t offset = 0;
    uint32_t magic = 0, format_version = 0, application_present = 0;
    auto invalid = [] { return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT); };
    if (!ReadStateDatabaseU32(data, offset, magic) ||
        !ReadStateDatabaseU32(data, offset, format_version) ||
        magic != kStateDatabaseMagic || format_version != kStateDatabaseVersion ||
        !ReadStateDatabaseU32(data, offset, application_present) ||
        application_present > 1u)
      return invalid();

    m_application_desc = {};
    m_exe_filename.clear();
    m_application_name.clear();
    m_engine_name.clear();
    m_has_application_desc = application_present != 0;
    if (m_has_application_desc) {
      uint64_t version = 0, engine_version = 0;
      if (!ReadStateDatabaseString(data, offset, m_exe_filename) ||
          !ReadStateDatabaseString(data, offset, m_application_name) ||
          !ReadStateDatabaseU64(data, offset, version) ||
          !ReadStateDatabaseString(data, offset, m_engine_name) ||
          !ReadStateDatabaseU64(data, offset, engine_version))
        return invalid();
      m_application_desc.pExeFilename =
          m_exe_filename.empty() ? nullptr : m_exe_filename.c_str();
      m_application_desc.pName =
          m_application_name.empty() ? nullptr : m_application_name.c_str();
      m_application_desc.Version.Version = version;
      m_application_desc.pEngineName =
          m_engine_name.empty() ? nullptr : m_engine_name.c_str();
      m_application_desc.EngineVersion.Version = engine_version;
    }

    m_pipeline_descs.clear();
    m_state_object_descs.clear();
    uint32_t pipeline_count = 0;
    if (!ReadStateDatabaseU32(data, offset, pipeline_count) ||
        pipeline_count > kStateDatabaseMaxEntries)
      return invalid();
    for (uint32_t i = 0; i < pipeline_count; ++i) {
      uint32_t key_size = 0, version = 0, stream_size = 0;
      if (!ReadStateDatabaseU32(data, offset, key_size) || key_size == 0 ||
          key_size > kStateDatabaseMaxKeyBytes ||
          offset > data.size() || data.size() - offset < key_size)
        return invalid();
      std::vector<uint8_t> key(key_size);
      if (!ReadStateDatabaseBytes(data, offset, key.data(), key.size()) ||
          !ReadStateDatabaseU32(data, offset, version) ||
          !ReadStateDatabaseU32(data, offset, stream_size) || stream_size == 0 ||
          stream_size > kStateDatabaseMaxBytes || offset > data.size() ||
          data.size() - offset < stream_size)
        return invalid();
      PipelineDescEntry entry;
      entry.version = version;
      entry.stream.resize(stream_size);
      if (!ReadStateDatabaseBytes(data, offset, entry.stream.data(),
                                  entry.stream.size()))
        return invalid();
      m_pipeline_descs.emplace(std::move(key), std::move(entry));
    }

    uint32_t state_count = 0;
    if (!ReadStateDatabaseU32(data, offset, state_count) ||
        state_count > kStateDatabaseMaxEntries)
      return invalid();
    for (uint32_t i = 0; i < state_count; ++i) {
      uint32_t key_size = 0, version = 0, type = 0, subobject_count = 0;
      if (!ReadStateDatabaseU32(data, offset, key_size) || key_size == 0 ||
          key_size > kStateDatabaseMaxKeyBytes || offset > data.size() ||
          data.size() - offset < key_size)
        return invalid();
      std::vector<uint8_t> key(key_size);
      if (!ReadStateDatabaseBytes(data, offset, key.data(), key.size()) ||
          !ReadStateDatabaseU32(data, offset, version) ||
          !ReadStateDatabaseU32(data, offset, type) ||
          !ReadStateDatabaseU32(data, offset, subobject_count) ||
          subobject_count > 64u)
        return invalid();
      StateObjectDescEntry entry;
      entry.version = version;
      entry.type = static_cast<D3D12_STATE_OBJECT_TYPE>(type);
      entry.subobjects.reserve(subobject_count);
      for (uint32_t sub = 0; sub < subobject_count; ++sub) {
        uint32_t subobject_type = 0;
        if (!ReadStateDatabaseU32(data, offset, subobject_type))
          return invalid();
        StateObjectSubobjectEntry stored;
        stored.type = static_cast<D3D12_STATE_SUBOBJECT_TYPE>(subobject_type);
        if (stored.type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
          uint32_t library_size = 0, export_count = 0;
          if (!ReadStateDatabaseU32(data, offset, library_size) ||
              library_size == 0 || library_size > kStateDatabaseMaxBytes ||
              offset > data.size() || data.size() - offset < library_size)
            return invalid();
          stored.library.resize(library_size);
          if (!ReadStateDatabaseBytes(data, offset, stored.library.data(),
                                      stored.library.size()) ||
              !ReadStateDatabaseU32(data, offset, export_count) ||
              export_count > 64u)
            return invalid();
          stored.exports.reserve(export_count);
          for (uint32_t export_index = 0; export_index < export_count;
               ++export_index) {
            StateObjectExportEntry export_entry;
            if (!ReadStateDatabaseString(data, offset, export_entry.name) ||
                !ReadStateDatabaseString(data, offset, export_entry.rename) ||
                !ReadStateDatabaseU32(data, offset, export_entry.flags) ||
                export_entry.name.empty())
              return invalid();
            stored.exports.push_back(std::move(export_entry));
          }
        } else if (stored.type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP) {
          if (!ReadStateDatabaseU32(data, offset, stored.hit_group_type) ||
              !ReadStateDatabaseString(data, offset, stored.hit_group_export) ||
              !ReadStateDatabaseString(data, offset, stored.hit_group_any_hit) ||
              !ReadStateDatabaseString(data, offset, stored.hit_group_closest_hit) ||
              !ReadStateDatabaseString(data, offset, stored.hit_group_intersection) ||
              stored.hit_group_export.empty())
            return invalid();
        } else if (stored.type ==
                   D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
          uint32_t export_count = 0;
          if (!ReadStateDatabaseU32(data, offset,
                                    stored.subobject_association_target) ||
              !ReadStateDatabaseU32(data, offset, export_count) ||
              export_count > 64u)
            return invalid();
          stored.subobject_association_exports.reserve(export_count);
          for (uint32_t export_index = 0; export_index < export_count;
               ++export_index) {
            std::wstring export_name;
            if (!ReadStateDatabaseString(data, offset, export_name) ||
                export_name.empty())
              return invalid();
            stored.subobject_association_exports.push_back(
                std::move(export_name));
          }
        } else if (stored.type ==
                   D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
          uint32_t export_count = 0;
          if (!ReadStateDatabaseString(data, offset,
                                       stored.dxil_association_target) ||
              stored.dxil_association_target.empty() ||
              !ReadStateDatabaseU32(data, offset, export_count) ||
              export_count > 64u)
            return invalid();
          stored.dxil_association_exports.reserve(export_count);
          for (uint32_t export_index = 0; export_index < export_count;
               ++export_index) {
            std::wstring export_name;
            if (!ReadStateDatabaseString(data, offset, export_name) ||
                export_name.empty())
              return invalid();
            stored.dxil_association_exports.push_back(std::move(export_name));
          }
        } else if (static_cast<UINT>(stored.type) ==
                       kStateSubobjectGlobalSerializedRootSignature ||
                   static_cast<UINT>(stored.type) ==
                       kStateSubobjectLocalSerializedRootSignature) {
          uint32_t blob_size = 0;
          if (!ReadStateDatabaseU32(data, offset, blob_size) || blob_size == 0 ||
              blob_size > kStateDatabaseMaxBytes || offset > data.size() ||
              data.size() - offset < blob_size)
            return invalid();
          stored.serialized_root_signature.resize(blob_size);
          if (!ReadStateDatabaseBytes(data, offset,
                                      stored.serialized_root_signature.data(),
                                      stored.serialized_root_signature.size()))
            return invalid();
        } else if (static_cast<UINT>(stored.type) ==
                   kStateSubobjectExistingCollectionByKey) {
          uint32_t collection_key_size = 0, export_count = 0;
          if (!ReadStateDatabaseU32(data, offset, collection_key_size) ||
              collection_key_size == 0 ||
              collection_key_size > kStateDatabaseMaxKeyBytes ||
              offset > data.size() || data.size() - offset < collection_key_size)
            return invalid();
          stored.existing_collection_key.resize(collection_key_size);
          if (!ReadStateDatabaseBytes(data, offset,
                                      stored.existing_collection_key.data(),
                                      stored.existing_collection_key.size()) ||
              !ReadStateDatabaseU32(data, offset, export_count) ||
              export_count > 64u)
            return invalid();
          stored.existing_collection_exports.reserve(export_count);
          for (uint32_t export_index = 0; export_index < export_count;
               ++export_index) {
            StateObjectExportEntry export_entry;
            if (!ReadStateDatabaseString(data, offset, export_entry.name) ||
                !ReadStateDatabaseString(data, offset, export_entry.rename) ||
                !ReadStateDatabaseU32(data, offset, export_entry.flags) ||
                export_entry.name.empty())
              return invalid();
            stored.existing_collection_exports.push_back(
                std::move(export_entry));
          }
        } else {
          uint32_t desc_size = 0;
          if (!ReadStateDatabaseU32(data, offset, desc_size) ||
              desc_size != StateSubobjectDescSize(stored.type) ||
              offset > data.size() || data.size() - offset < desc_size)
            return invalid();
          stored.desc.resize(desc_size);
          if (!ReadStateDatabaseBytes(data, offset, stored.desc.data(),
                                      stored.desc.size()))
            return invalid();
        }
        entry.subobjects.push_back(std::move(stored));
      }
      for (const auto &stored : entry.subobjects)
        if (stored.type ==
                D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION &&
            stored.subobject_association_target >= entry.subobjects.size())
          return invalid();
      uint32_t parent_size = 0;
      if (!ReadStateDatabaseU32(data, offset, parent_size) ||
          parent_size > kStateDatabaseMaxKeyBytes || offset > data.size() ||
          data.size() - offset < parent_size)
        return invalid();
      entry.parent_key.resize(parent_size);
      if (!ReadStateDatabaseBytes(data, offset, entry.parent_key.data(),
                                  entry.parent_key.size()))
        return invalid();
      m_state_object_descs.emplace(std::move(key), std::move(entry));
    }
    if (offset != data.size())
      return invalid();
    TraceAgility("StateObjectDatabase::Load file=%ls pipelines=%zu states=%zu",
                 m_file_path.c_str(), m_pipeline_descs.size(),
                 m_state_object_descs.size());
    return S_OK;
  }

  static std::vector<uint8_t> MakeKey(const void *key, UINT key_size) {
    auto *bytes = static_cast<const uint8_t *>(key);
    return std::vector<uint8_t>(bytes, bytes + key_size);
  }

  std::atomic<ULONG> m_ref = {1};
  std::wstring m_file_path;
  bool m_read_only = false;
  bool m_has_application_desc = false;
  D3D12ApplicationDescCompat m_application_desc = {};
  std::wstring m_exe_filename;
  std::wstring m_application_name;
  std::wstring m_engine_name;
  std::map<std::vector<uint8_t>, PipelineDescEntry> m_pipeline_descs;
  std::map<std::vector<uint8_t>, StateObjectDescEntry> m_state_object_descs;
};

class MTLD3D12StateObjectDatabaseFactory final
    : public ID3D12StateObjectDatabaseFactoryCompat {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12StateObjectDatabaseFactory) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  HRESULT STDMETHODCALLTYPE CreateStateObjectDatabaseFromFile(
      LPCWSTR database_file, D3D12StateObjectDatabaseFlagsCompat flags,
      REFIID riid, void **state_object_database) override {
    if (!state_object_database)
      return E_POINTER;
    *state_object_database = nullptr;
    TraceAgility(
        "StateObjectDatabaseFactory::CreateStateObjectDatabaseFromFile "
        "file=%ls flags=0x%x riid=%s",
        database_file ? database_file : L"(null)", flags,
        dxmt::str::format(riid).c_str());
    if (!database_file || !*database_file ||
        (flags & ~D3D12StateObjectDatabaseFlagReadOnly))
      return E_INVALIDARG;
    auto *database = new MTLD3D12StateObjectDatabase();
    HRESULT hr = database->Initialize(database_file, flags);
    if (SUCCEEDED(hr))
      hr = database->QueryInterface(riid, state_object_database);
    database->Release();
    return hr;
  }

private:
  std::atomic<ULONG> m_ref = {1};
};

class MTLD3D12RuntimeValidationControl final
    : public ID3D12RuntimeValidationControlCompat {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12RuntimeValidationControl) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  HRESULT STDMETHODCALLTYPE
  DisableFailuresFromStricterValidationInAppLocalRuntime(
      BOOL disable) override {
    m_disabled = disable;
    TraceAgility(
        "RuntimeValidationControl::"
        "DisableFailuresFromStricterValidationInAppLocalRuntime disabled=%d",
        disable);
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE
  FailuresFromStricterValidationInAppLocalRuntimeDisabled() override {
    TraceAgility(
        "RuntimeValidationControl::"
        "FailuresFromStricterValidationInAppLocalRuntimeDisabled -> %d",
        m_disabled);
    return m_disabled;
  }

private:
  std::atomic<ULONG> m_ref = {1};
  BOOL m_disabled = FALSE;
};

class MTLD3D12ApplicationIdentity final
    : public ID3D12ApplicationIdentityCompat {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12ApplicationIdentity) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --m_ref;
    if (!ref)
      delete this;
    return ref;
  }

  HRESULT STDMETHODCALLTYPE SetApplicationIdentity(
      const D3D12ApplicationDescCompat *desc, REFGUID app_id) override {
    if (!desc)
      return E_INVALIDARG;
    TraceAgility("ApplicationIdentity::SetApplicationIdentity name=%ls "
                 "engine=%ls app_id=%s",
                 desc->pName ? desc->pName : L"(null)",
                 desc->pEngineName ? desc->pEngineName : L"(null)",
                 dxmt::str::format(app_id).c_str());
    return S_OK;
  }

private:
  std::atomic<ULONG> m_ref = {1};
};

} // namespace

#pragma pack(push, 1)
struct _RSHeader {
  uint32_t num_parameters;
  uint32_t num_static_samplers;
  uint32_t flags;
};
struct _RSParameter {
  uint8_t type;
  uint8_t visibility;
  union {
    struct {
      uint32_t register_space;
      uint32_t register_index;
      uint32_t num_32bit_values;
    } constants;
    struct {
      uint32_t register_space;
      uint32_t register_index;
    } descriptor;
    struct {
      uint32_t num_ranges;
    } table;
  };
};
struct _RSDescriptorRange {
  uint8_t range_type;
  uint32_t num_descriptors;
  uint32_t base_register;
  uint32_t register_space;
  uint32_t offset_in_table;
};
struct _RSStaticSampler {
  uint32_t filter;
  uint32_t address_u;
  uint32_t address_v;
  uint32_t address_w;
  float mip_lod_bias;
  uint32_t max_anisotropy;
  uint32_t comparison_func;
  uint32_t border_color;
  float min_lod;
  float max_lod;
  uint32_t register_space;
  uint32_t register_index;
  uint32_t shader_register_space;
  uint8_t shader_visibility;
};

struct _DXContainerHeader {
  uint8_t magic[4];
  uint8_t digest[16];
  uint16_t major;
  uint16_t minor;
  uint32_t file_size;
  uint32_t part_count;
};

struct _DXContainerPartHeader {
  uint8_t name[4];
  uint32_t size;
};

struct _DXRootSignatureHeader {
  uint32_t version;
  uint32_t num_parameters;
  uint32_t parameters_offset;
  uint32_t num_static_samplers;
  uint32_t static_sampler_offset;
  uint32_t flags;
};

struct _DXRootParameterHeader {
  uint32_t parameter_type;
  uint32_t shader_visibility;
  uint32_t parameter_offset;
};

struct _DXRootConstants {
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t num_32bit_values;
};

struct _DXRootDescriptor10 {
  uint32_t shader_register;
  uint32_t register_space;
};

struct _DXRootDescriptor11 {
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t flags;
};

struct _DXDescriptorTable {
  uint32_t num_ranges;
  uint32_t ranges_offset;
};

struct _DXDescriptorRange10 {
  uint32_t range_type;
  uint32_t num_descriptors;
  uint32_t base_shader_register;
  uint32_t register_space;
  uint32_t offset_in_table;
};

struct _DXDescriptorRange11 {
  uint32_t range_type;
  uint32_t num_descriptors;
  uint32_t base_shader_register;
  uint32_t register_space;
  uint32_t flags;
  uint32_t offset_in_table;
};

struct _DXStaticSampler {
  uint32_t filter;
  uint32_t address_u;
  uint32_t address_v;
  uint32_t address_w;
  float mip_lod_bias;
  uint32_t max_anisotropy;
  uint32_t comparison_func;
  uint32_t border_color;
  float min_lod;
  float max_lod;
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t shader_visibility;
};
#pragma pack(pop)

static bool _RSRangeContains(size_t size, uint32_t offset, size_t bytes) {
  return offset <= size && bytes <= size - offset;
}

class _RSBlob : public ID3DBlob {
  ULONG m_ref = 1;
  std::vector<uint8_t> m_data;

public:
  _RSBlob(std::vector<uint8_t> &&data) : m_data(std::move(data)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) {
    if (riid == IID_IUnknown || riid == IID_ID3D10Blob ||
        riid == __uuidof(ID3DBlob)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() { return ++m_ref; }
  ULONG STDMETHODCALLTYPE Release() {
    ULONG r = --m_ref;
    if (!r)
      delete this;
    return r;
  }
  LPVOID STDMETHODCALLTYPE GetBufferPointer() { return m_data.data(); }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() { return m_data.size(); }
};

class _RSDeserializer final : public ID3D12RootSignatureDeserializer,
                              public ID3D12VersionedRootSignatureDeserializer {
public:
  _RSDeserializer(const void *data, SIZE_T size) {
    m_valid = Parse(data, size);
  }

  bool Valid() const { return m_valid; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_ID3D12RootSignatureDeserializer) {
      *ppv = static_cast<ID3D12RootSignatureDeserializer *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_ID3D12VersionedRootSignatureDeserializer) {
      *ppv = static_cast<ID3D12VersionedRootSignatureDeserializer *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --m_ref;
    if (!r)
      delete this;
    return r;
  }

  const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetRootSignatureDesc() override {
    return &m_desc;
  }

  HRESULT STDMETHODCALLTYPE GetRootSignatureDescAtVersion(
      D3D_ROOT_SIGNATURE_VERSION version,
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **desc) override {
    if (!desc)
      return E_POINTER;
    *desc = nullptr;

    if (version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
      *desc = &m_versioned_desc0;
      return S_OK;
    }

    if (version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
      *desc = &m_versioned_desc1;
      return S_OK;
    }

    return E_INVALIDARG;
  }

  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetUnconvertedRootSignatureDesc() override {
    return m_unconverted_version == D3D_ROOT_SIGNATURE_VERSION_1_1
               ? &m_versioned_desc1
               : &m_versioned_desc0;
  }

private:
  void FinalizeDescs(UINT flags, D3D_ROOT_SIGNATURE_VERSION original_version) {
    m_desc.NumParameters = static_cast<UINT>(m_params0.size());
    m_desc.pParameters = m_params0.empty() ? nullptr : m_params0.data();
    m_desc.NumStaticSamplers = static_cast<UINT>(m_static_samplers.size());
    m_desc.pStaticSamplers =
        m_static_samplers.empty() ? nullptr : m_static_samplers.data();
    m_desc.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags);

    m_desc1.NumParameters = static_cast<UINT>(m_params1.size());
    m_desc1.pParameters = m_params1.empty() ? nullptr : m_params1.data();
    m_desc1.NumStaticSamplers = static_cast<UINT>(m_static_samplers.size());
    m_desc1.pStaticSamplers =
        m_static_samplers.empty() ? nullptr : m_static_samplers.data();
    m_desc1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags);

    m_versioned_desc0.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    m_versioned_desc0.Desc_1_0 = m_desc;
    m_versioned_desc1.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    m_versioned_desc1.Desc_1_1 = m_desc1;
    m_unconverted_version = original_version;
  }

  bool ParseRTS0(const uint8_t *data, SIZE_T size) {
    if (!data || size < sizeof(_DXRootSignatureHeader))
      return false;

    auto *hdr = reinterpret_cast<const _DXRootSignatureHeader *>(data);
    if ((hdr->version != D3D_ROOT_SIGNATURE_VERSION_1_0 &&
         hdr->version != D3D_ROOT_SIGNATURE_VERSION_1_1) ||
        hdr->num_parameters > 64 || hdr->num_static_samplers > 64 ||
        !_RSRangeContains(size, hdr->parameters_offset,
                          hdr->num_parameters * sizeof(_DXRootParameterHeader)))
      return false;

    if (hdr->num_static_samplers > 0 &&
        !_RSRangeContains(size, hdr->static_sampler_offset,
                          hdr->num_static_samplers * sizeof(_DXStaticSampler)))
      return false;

    m_params0.clear();
    m_ranges0.clear();
    m_params1.clear();
    m_ranges1.clear();
    m_static_samplers.clear();
    m_params0.resize(hdr->num_parameters);
    m_ranges0.resize(hdr->num_parameters);
    m_params1.resize(hdr->num_parameters);
    m_ranges1.resize(hdr->num_parameters);
    m_static_samplers.resize(hdr->num_static_samplers);

    auto *param_headers = reinterpret_cast<const _DXRootParameterHeader *>(
        data + hdr->parameters_offset);

    for (UINT i = 0; i < hdr->num_parameters; i++) {
      const auto &src = param_headers[i];
      if (!_RSRangeContains(size, src.parameter_offset, sizeof(uint32_t)))
        return false;

      auto &dst0 = m_params0[i];
      auto &dst1 = m_params1[i];
      dst0.ParameterType =
          static_cast<D3D12_ROOT_PARAMETER_TYPE>(src.parameter_type);
      dst0.ShaderVisibility =
          static_cast<D3D12_SHADER_VISIBILITY>(src.shader_visibility);
      dst1.ParameterType = dst0.ParameterType;
      dst1.ShaderVisibility = dst0.ShaderVisibility;

      switch (dst0.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
        if (!_RSRangeContains(size, src.parameter_offset,
                              sizeof(_DXRootConstants)))
          return false;

        auto *constants = reinterpret_cast<const _DXRootConstants *>(
            data + src.parameter_offset);
        dst0.Constants.RegisterSpace = constants->register_space;
        dst0.Constants.ShaderRegister = constants->shader_register;
        dst0.Constants.Num32BitValues = constants->num_32bit_values;
        dst1.Constants = dst0.Constants;
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV: {
        size_t descriptor_size =
            hdr->version == D3D_ROOT_SIGNATURE_VERSION_1_0
                ? sizeof(_DXRootDescriptor10)
                : sizeof(_DXRootDescriptor10) + sizeof(uint32_t);
        if (!_RSRangeContains(size, src.parameter_offset, descriptor_size))
          return false;

        auto *descriptor = reinterpret_cast<const _DXRootDescriptor10 *>(
            data + src.parameter_offset);
        dst0.Descriptor.RegisterSpace = descriptor->register_space;
        dst0.Descriptor.ShaderRegister = descriptor->shader_register;
        dst1.Descriptor.RegisterSpace = descriptor->register_space;
        dst1.Descriptor.ShaderRegister = descriptor->shader_register;
        dst1.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
        if (hdr->version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
          auto *descriptor1 = reinterpret_cast<const _DXRootDescriptor11 *>(
              data + src.parameter_offset);
          dst1.Descriptor.Flags =
              static_cast<D3D12_ROOT_DESCRIPTOR_FLAGS>(descriptor1->flags);
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
        if (!_RSRangeContains(size, src.parameter_offset,
                              sizeof(_DXDescriptorTable)))
          return false;

        auto *table = reinterpret_cast<const _DXDescriptorTable *>(
            data + src.parameter_offset);
        if (table->num_ranges > 256)
          return false;

        size_t range_size = hdr->version == D3D_ROOT_SIGNATURE_VERSION_1_0
                                ? sizeof(_DXDescriptorRange10)
                                : sizeof(_DXDescriptorRange11);
        const uint8_t *ranges_base =
            data + src.parameter_offset + sizeof(*table);
        uint32_t inline_ranges_offset = src.parameter_offset + sizeof(*table);
        if (_RSRangeContains(size, table->ranges_offset,
                             table->num_ranges * range_size)) {
          ranges_base = data + table->ranges_offset;
        } else if (!_RSRangeContains(size, inline_ranges_offset,
                                     table->num_ranges * range_size)) {
          return false;
        }

        m_ranges0[i].resize(table->num_ranges);
        m_ranges1[i].resize(table->num_ranges);
        for (UINT r = 0; r < table->num_ranges; r++) {
          auto &out_range0 = m_ranges0[i][r];
          auto &out_range1 = m_ranges1[i][r];
          if (hdr->version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
            auto *src_range = reinterpret_cast<const _DXDescriptorRange10 *>(
                ranges_base + r * range_size);
            out_range0.RangeType =
                static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(src_range->range_type);
            out_range0.NumDescriptors = src_range->num_descriptors;
            out_range0.BaseShaderRegister = src_range->base_shader_register;
            out_range0.RegisterSpace = src_range->register_space;
            out_range0.OffsetInDescriptorsFromTableStart =
                src_range->offset_in_table;
            out_range1.RangeType = out_range0.RangeType;
            out_range1.NumDescriptors = out_range0.NumDescriptors;
            out_range1.BaseShaderRegister = out_range0.BaseShaderRegister;
            out_range1.RegisterSpace = out_range0.RegisterSpace;
            out_range1.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
            out_range1.OffsetInDescriptorsFromTableStart =
                out_range0.OffsetInDescriptorsFromTableStart;
          } else {
            auto *src_range = reinterpret_cast<const _DXDescriptorRange11 *>(
                ranges_base + r * range_size);
            out_range0.RangeType =
                static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(src_range->range_type);
            out_range0.NumDescriptors = src_range->num_descriptors;
            out_range0.BaseShaderRegister = src_range->base_shader_register;
            out_range0.RegisterSpace = src_range->register_space;
            out_range0.OffsetInDescriptorsFromTableStart =
                src_range->offset_in_table;
            out_range1.RangeType = out_range0.RangeType;
            out_range1.NumDescriptors = out_range0.NumDescriptors;
            out_range1.BaseShaderRegister = out_range0.BaseShaderRegister;
            out_range1.RegisterSpace = out_range0.RegisterSpace;
            out_range1.Flags =
                static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(src_range->flags);
            out_range1.OffsetInDescriptorsFromTableStart =
                out_range0.OffsetInDescriptorsFromTableStart;
          }
        }
        dst0.DescriptorTable.NumDescriptorRanges = table->num_ranges;
        dst0.DescriptorTable.pDescriptorRanges =
            m_ranges0[i].empty() ? nullptr : m_ranges0[i].data();
        dst1.DescriptorTable.NumDescriptorRanges = table->num_ranges;
        dst1.DescriptorTable.pDescriptorRanges =
            m_ranges1[i].empty() ? nullptr : m_ranges1[i].data();
        break;
      }
      default:
        return false;
      }
    }

    auto *samplers = reinterpret_cast<const _DXStaticSampler *>(
        data + hdr->static_sampler_offset);
    for (UINT i = 0; i < hdr->num_static_samplers; i++) {
      const auto &src = samplers[i];
      auto &dst = m_static_samplers[i];
      dst.Filter = static_cast<D3D12_FILTER>(src.filter);
      dst.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(src.address_u);
      dst.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(src.address_v);
      dst.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(src.address_w);
      dst.MipLODBias = src.mip_lod_bias;
      dst.MaxAnisotropy = src.max_anisotropy;
      dst.ComparisonFunc =
          static_cast<D3D12_COMPARISON_FUNC>(src.comparison_func);
      dst.BorderColor =
          static_cast<D3D12_STATIC_BORDER_COLOR>(src.border_color);
      dst.MinLOD = src.min_lod;
      dst.MaxLOD = src.max_lod;
      dst.ShaderRegister = src.shader_register;
      dst.RegisterSpace = src.register_space;
      dst.ShaderVisibility =
          static_cast<D3D12_SHADER_VISIBILITY>(src.shader_visibility);
    }

    FinalizeDescs(hdr->flags,
                  static_cast<D3D_ROOT_SIGNATURE_VERSION>(hdr->version));
    TraceAgility("D3D12RootSignatureDeserializer parsed RTS0 version=%u "
                 "params=%u samplers=%u flags=0x%x",
                 hdr->version, hdr->num_parameters, hdr->num_static_samplers,
                 hdr->flags);
    return true;
  }

  bool ParsePrivate(const void *data, SIZE_T size) {
    if (!data || size < sizeof(_RSHeader))
      return false;

    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    const uint8_t *end = ptr + size;
    auto canRead = [&](SIZE_T bytes) -> bool {
      return bytes <= static_cast<SIZE_T>(end - ptr);
    };

    auto *hdr = reinterpret_cast<const _RSHeader *>(ptr);
    if (hdr->num_parameters > size / sizeof(_RSParameter) ||
        hdr->num_static_samplers > size / sizeof(_RSStaticSampler))
      return false;

    ptr += sizeof(_RSHeader);
    m_params0.clear();
    m_ranges0.clear();
    m_params1.clear();
    m_ranges1.clear();
    m_static_samplers.clear();
    m_params0.resize(hdr->num_parameters);
    m_ranges0.resize(hdr->num_parameters);
    m_params1.resize(hdr->num_parameters);
    m_ranges1.resize(hdr->num_parameters);
    m_static_samplers.resize(hdr->num_static_samplers);

    for (UINT i = 0; i < hdr->num_parameters; i++) {
      if (!canRead(sizeof(_RSParameter)))
        return false;

      auto *src = reinterpret_cast<const _RSParameter *>(ptr);
      auto &dst0 = m_params0[i];
      auto &dst1 = m_params1[i];
      dst0.ParameterType = static_cast<D3D12_ROOT_PARAMETER_TYPE>(src->type);
      dst0.ShaderVisibility =
          static_cast<D3D12_SHADER_VISIBILITY>(src->visibility);
      dst1.ParameterType = dst0.ParameterType;
      dst1.ShaderVisibility = dst0.ShaderVisibility;
      ptr += sizeof(_RSParameter);

      switch (dst0.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        dst0.Constants.RegisterSpace = src->constants.register_space;
        dst0.Constants.ShaderRegister = src->constants.register_index;
        dst0.Constants.Num32BitValues = src->constants.num_32bit_values;
        dst1.Constants = dst0.Constants;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        dst0.Descriptor.RegisterSpace = src->descriptor.register_space;
        dst0.Descriptor.ShaderRegister = src->descriptor.register_index;
        dst1.Descriptor.RegisterSpace = dst0.Descriptor.RegisterSpace;
        dst1.Descriptor.ShaderRegister = dst0.Descriptor.ShaderRegister;
        dst1.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        if (src->table.num_ranges >
            static_cast<UINT>((end - ptr) / sizeof(_RSDescriptorRange)))
          return false;
        m_ranges0[i].resize(src->table.num_ranges);
        m_ranges1[i].resize(src->table.num_ranges);
        for (UINT r = 0; r < src->table.num_ranges; r++) {
          auto *range = reinterpret_cast<const _RSDescriptorRange *>(ptr);
          auto &out_range0 = m_ranges0[i][r];
          auto &out_range1 = m_ranges1[i][r];
          out_range0.RangeType =
              static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(range->range_type);
          out_range0.NumDescriptors = range->num_descriptors;
          out_range0.BaseShaderRegister = range->base_register;
          out_range0.RegisterSpace = range->register_space;
          out_range0.OffsetInDescriptorsFromTableStart = range->offset_in_table;
          out_range1.RangeType = out_range0.RangeType;
          out_range1.NumDescriptors = out_range0.NumDescriptors;
          out_range1.BaseShaderRegister = out_range0.BaseShaderRegister;
          out_range1.RegisterSpace = out_range0.RegisterSpace;
          out_range1.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
          out_range1.OffsetInDescriptorsFromTableStart =
              out_range0.OffsetInDescriptorsFromTableStart;
          ptr += sizeof(_RSDescriptorRange);
        }
        dst0.DescriptorTable.NumDescriptorRanges = src->table.num_ranges;
        dst0.DescriptorTable.pDescriptorRanges = m_ranges0[i].data();
        dst1.DescriptorTable.NumDescriptorRanges = src->table.num_ranges;
        dst1.DescriptorTable.pDescriptorRanges = m_ranges1[i].data();
        break;
      default:
        return false;
      }
    }

    for (UINT i = 0; i < hdr->num_static_samplers; i++) {
      if (!canRead(sizeof(_RSStaticSampler)))
        return false;

      auto *src = reinterpret_cast<const _RSStaticSampler *>(ptr);
      auto &dst = m_static_samplers[i];
      dst.Filter = static_cast<D3D12_FILTER>(src->filter);
      dst.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(src->address_u);
      dst.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(src->address_v);
      dst.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(src->address_w);
      dst.MipLODBias = src->mip_lod_bias;
      dst.MaxAnisotropy = src->max_anisotropy;
      dst.ComparisonFunc =
          static_cast<D3D12_COMPARISON_FUNC>(src->comparison_func);
      dst.BorderColor =
          static_cast<D3D12_STATIC_BORDER_COLOR>(src->border_color);
      dst.MinLOD = src->min_lod;
      dst.MaxLOD = src->max_lod;
      dst.ShaderRegister = src->register_index;
      dst.RegisterSpace = src->register_space;
      dst.ShaderVisibility =
          static_cast<D3D12_SHADER_VISIBILITY>(src->shader_visibility);
      ptr += sizeof(_RSStaticSampler);
    }

    FinalizeDescs(hdr->flags, D3D_ROOT_SIGNATURE_VERSION_1_0);
    return ptr <= end;
  }

  bool Parse(const void *data, SIZE_T size) {
    if (!data || size < sizeof(_RSHeader))
      return false;

    auto *bytes = static_cast<const uint8_t *>(data);
    if (size >= sizeof(_DXContainerHeader) && memcmp(bytes, "DXBC", 4) == 0) {
      auto *container = reinterpret_cast<const _DXContainerHeader *>(bytes);
      if (container->part_count <= 256 && container->file_size <= size &&
          _RSRangeContains(size, sizeof(_DXContainerHeader),
                           container->part_count * sizeof(uint32_t))) {
        auto *part_offsets = reinterpret_cast<const uint32_t *>(
            bytes + sizeof(_DXContainerHeader));
        for (UINT i = 0; i < container->part_count; i++) {
          uint32_t offset = part_offsets[i];
          if (!_RSRangeContains(size, offset, sizeof(_DXContainerPartHeader)))
            continue;

          auto *part =
              reinterpret_cast<const _DXContainerPartHeader *>(bytes + offset);
          if (memcmp(part->name, "RTS0", 4) != 0)
            continue;
          if (!_RSRangeContains(size, offset + sizeof(*part), part->size))
            continue;
          if (ParseRTS0(bytes + offset + sizeof(*part), part->size))
            return true;
        }
      }
    }

    if (ParseRTS0(bytes, size))
      return true;

    return ParsePrivate(data, size);
  }

  ULONG m_ref = 1;
  bool m_valid = false;
  D3D_ROOT_SIGNATURE_VERSION m_unconverted_version =
      D3D_ROOT_SIGNATURE_VERSION_1_0;
  D3D12_ROOT_SIGNATURE_DESC m_desc = {};
  D3D12_ROOT_SIGNATURE_DESC1 m_desc1 = {};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC m_versioned_desc0 = {};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC m_versioned_desc1 = {};
  std::vector<D3D12_ROOT_PARAMETER> m_params0;
  std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> m_ranges0;
  std::vector<D3D12_ROOT_PARAMETER1> m_params1;
  std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> m_ranges1;
  std::vector<D3D12_STATIC_SAMPLER_DESC> m_static_samplers;
};

static HRESULT _SerializeRootSig(const D3D12_ROOT_SIGNATURE_DESC *desc,
                                 ID3DBlob **ppBlob) {
  if (!desc || !ppBlob)
    return E_INVALIDARG;
  *ppBlob = nullptr;

  std::vector<uint8_t> buf;
  size_t total = sizeof(_RSHeader) + desc->NumParameters * sizeof(_RSParameter);
  for (UINT i = 0; i < desc->NumParameters; i++) {
    if (desc->pParameters[i].ParameterType ==
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
      total += desc->pParameters[i].DescriptorTable.NumDescriptorRanges *
               sizeof(_RSDescriptorRange);
  }
  total += desc->NumStaticSamplers * sizeof(_RSStaticSampler);
  buf.resize(total);

  auto *hdr = reinterpret_cast<_RSHeader *>(buf.data());
  hdr->num_parameters = desc->NumParameters;
  hdr->num_static_samplers = desc->NumStaticSamplers;
  hdr->flags = desc->Flags;

  uint8_t *ptr = buf.data() + sizeof(_RSHeader);
  for (UINT i = 0; i < desc->NumParameters; i++) {
    auto &p = desc->pParameters[i];
    auto *out = reinterpret_cast<_RSParameter *>(ptr);
    out->type = (uint8_t)p.ParameterType;
    out->visibility = (uint8_t)p.ShaderVisibility;
    switch (p.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      out->constants.register_space = p.Constants.RegisterSpace;
      out->constants.register_index = p.Constants.ShaderRegister;
      out->constants.num_32bit_values = p.Constants.Num32BitValues;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      out->descriptor.register_space = p.Descriptor.RegisterSpace;
      out->descriptor.register_index = p.Descriptor.ShaderRegister;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      out->table.num_ranges = p.DescriptorTable.NumDescriptorRanges;
      ptr += sizeof(_RSParameter);
      for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; r++) {
        auto &rng = p.DescriptorTable.pDescriptorRanges[r];
        auto *orng = reinterpret_cast<_RSDescriptorRange *>(ptr);
        orng->range_type = (uint8_t)rng.RangeType;
        orng->num_descriptors = rng.NumDescriptors;
        orng->base_register = rng.BaseShaderRegister;
        orng->register_space = rng.RegisterSpace;
        orng->offset_in_table = rng.OffsetInDescriptorsFromTableStart;
        ptr += sizeof(_RSDescriptorRange);
      }
      continue;
    }
    ptr += sizeof(_RSParameter);
  }

  for (UINT i = 0; i < desc->NumStaticSamplers; i++) {
    auto &s = desc->pStaticSamplers[i];
    auto *out = reinterpret_cast<_RSStaticSampler *>(ptr);
    out->filter = s.Filter;
    out->address_u = s.AddressU;
    out->address_v = s.AddressV;
    out->address_w = s.AddressW;
    out->mip_lod_bias = s.MipLODBias;
    out->max_anisotropy = s.MaxAnisotropy;
    out->comparison_func = s.ComparisonFunc;
    out->border_color = s.BorderColor;
    out->min_lod = s.MinLOD;
    out->max_lod = s.MaxLOD;
    out->register_space = s.RegisterSpace;
    out->register_index = s.ShaderRegister;
    out->shader_register_space = s.RegisterSpace;
    out->shader_visibility = (uint8_t)s.ShaderVisibility;
    ptr += sizeof(_RSStaticSampler);
  }

  *ppBlob = new _RSBlob(std::move(buf));
  return S_OK;
}

using namespace dxmt;

using PFN_CreateDXGIFactory1_Dynamic = HRESULT(WINAPI *)(REFIID, void **);

static HRESULT DXMTCreateDXGIFactory1(REFIID riid, void **factory) {
  HMODULE dxgi = LoadLibraryA("dxgi.dll");
  if (!dxgi) {
    auto gle = GetLastError();
    DXMTD3D12Trace("Entry", "LoadLibraryA(dxgi.dll) failed gle=%lu", gle);
    return HRESULT_FROM_WIN32(gle);
  }

  auto proc = reinterpret_cast<PFN_CreateDXGIFactory1_Dynamic>(
      GetProcAddress(dxgi, "CreateDXGIFactory1"));
  if (!proc) {
    auto gle = GetLastError();
    DXMTD3D12Trace(
        "Entry", "GetProcAddress(dxgi!CreateDXGIFactory1) failed gle=%lu", gle);
    return HRESULT_FROM_WIN32(gle);
  }

  return proc(riid, factory);
}

extern "C" HRESULT WINAPI
D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
                  REFIID riid, void **ppDevice) {
  EnsureAppLocalAgilityRuntimeLoaded();
  {
    DXMTD3D12Trace("Entry", "D3D12CreateDevice CALLED FL=%d adapter=%p riid=%s",
                   MinimumFeatureLevel, pAdapter, str::format(riid).c_str());
  }
  const bool known_feature_level =
      MinimumFeatureLevel == D3D_FEATURE_LEVEL_11_0 ||
      MinimumFeatureLevel == D3D_FEATURE_LEVEL_11_1 ||
      MinimumFeatureLevel == D3D_FEATURE_LEVEL_12_0 ||
      MinimumFeatureLevel == D3D_FEATURE_LEVEL_12_1 ||
      MinimumFeatureLevel == D3D_FEATURE_LEVEL_12_2;
  if (!known_feature_level) {
    DXMTD3D12Trace("Entry", "D3D12CreateDevice INVALID FL=%d",
                   MinimumFeatureLevel);
    return E_INVALIDARG;
  }
  const D3D_FEATURE_LEVEL maximum_feature_level =
      dxmt::D3D12ConfiguredMaximumFeatureLevel();
  if (MinimumFeatureLevel > maximum_feature_level) {
    DXMTD3D12Trace("Entry",
                   "D3D12CreateDevice UNSUPPORTED FL=%d max=%d",
                   MinimumFeatureLevel, maximum_feature_level);
    return DXGI_ERROR_UNSUPPORTED;
  }

  const bool support_probe = ppDevice == nullptr;
  if (ppDevice)
    *ppDevice = nullptr;

  Com<IMTLDXGIAdapter> dxgi_adapter;

  if (pAdapter) {
    HRESULT adapter_hr = pAdapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter));
    DXMTD3D12Trace("Entry",
                   "D3D12CreateDevice adapter QI IMTLDXGIAdapter hr=0x%lx",
                   adapter_hr);
    if (FAILED(adapter_hr)) {
      Com<IDXGIAdapter1> generic_adapter;
      HRESULT generic_hr =
          pAdapter->QueryInterface(IID_PPV_ARGS(&generic_adapter));
      DXMTD3D12Trace(
          "Entry",
          "D3D12CreateDevice adapter fallback QI IDXGIAdapter1 hr=0x%lx",
          generic_hr);
      if (SUCCEEDED(generic_hr)) {
        DXGI_ADAPTER_DESC1 desc = {};
        HRESULT desc_hr = generic_adapter->GetDesc1(&desc);
        DXMTD3D12Trace("Entry",
                       "D3D12CreateDevice adapter fallback GetDesc1 hr=0x%lx "
                       "luid=%08lx:%08lx vendor=0x%x device=0x%x",
                       desc_hr, desc.AdapterLuid.HighPart,
                       desc.AdapterLuid.LowPart, desc.VendorId, desc.DeviceId);
        if (SUCCEEDED(desc_hr)) {
          Com<IDXGIFactory6> factory;
          HRESULT factory_hr = DXMTCreateDXGIFactory1(IID_PPV_ARGS(&factory));
          DXMTD3D12Trace(
              "Entry",
              "D3D12CreateDevice adapter fallback CreateDXGIFactory1 hr=0x%lx",
              factory_hr);
          if (SUCCEEDED(factory_hr)) {
            Com<IDXGIAdapter1> resolved_adapter;
            HRESULT enum_hr = factory->EnumAdapterByLuid(
                desc.AdapterLuid, IID_PPV_ARGS(&resolved_adapter));
            DXMTD3D12Trace("Entry",
                           "D3D12CreateDevice adapter fallback "
                           "EnumAdapterByLuid hr=0x%lx adapter=%p",
                           enum_hr, resolved_adapter.ptr());
            if (SUCCEEDED(enum_hr) && resolved_adapter) {
              adapter_hr =
                  resolved_adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter));
              DXMTD3D12Trace("Entry",
                             "D3D12CreateDevice adapter fallback resolved "
                             "IMTLDXGIAdapter hr=0x%lx",
                             adapter_hr);
            }
          }
        }
      }

      if (FAILED(adapter_hr)) {
        WARN("D3D12CreateDevice: adapter is not a DXMT adapter, falling back "
             "to default adapter");
      }
    }
  } else {
    Com<IDXGIFactory1> factory;
    HRESULT factory_hr = DXMTCreateDXGIFactory1(IID_PPV_ARGS(&factory));
    DXMTD3D12Trace("Entry", "D3D12CreateDevice CreateDXGIFactory1 hr=0x%lx",
                   factory_hr);
    if (FAILED(factory_hr)) {
      ERR("D3D12CreateDevice: failed to create DXGI factory");
      return E_FAIL;
    }
    Com<IDXGIAdapter> adapter;
    HRESULT enum_hr = factory->EnumAdapters(0, &adapter);
    DXMTD3D12Trace("Entry",
                   "D3D12CreateDevice default EnumAdapters hr=0x%lx adapter=%p",
                   enum_hr, adapter.ptr());
    if (FAILED(enum_hr)) {
      ERR("D3D12CreateDevice: no adapters available");
      return E_FAIL;
    }
    HRESULT adapter_hr = adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter));
    DXMTD3D12Trace(
        "Entry",
        "D3D12CreateDevice default adapter QI IMTLDXGIAdapter hr=0x%lx",
        adapter_hr);
    if (FAILED(adapter_hr)) {
      ERR("D3D12CreateDevice: default adapter is not DXMT");
      return E_FAIL;
    }
  }

  if (!dxgi_adapter) {
    Com<IDXGIFactory1> factory;
    HRESULT factory_hr = DXMTCreateDXGIFactory1(IID_PPV_ARGS(&factory));
    DXMTD3D12Trace(
        "Entry",
        "D3D12CreateDevice fallback-to-default CreateDXGIFactory1 hr=0x%lx",
        factory_hr);
    if (FAILED(factory_hr)) {
      ERR("D3D12CreateDevice: failed to create DXGI factory for fallback");
      return E_FAIL;
    }
    Com<IDXGIAdapter> adapter;
    HRESULT enum_hr = factory->EnumAdapters(0, &adapter);
    DXMTD3D12Trace("Entry",
                   "D3D12CreateDevice fallback-to-default EnumAdapters "
                   "hr=0x%lx adapter=%p",
                   enum_hr, adapter.ptr());
    if (FAILED(enum_hr)) {
      ERR("D3D12CreateDevice: no adapters available for fallback");
      return E_FAIL;
    }
    HRESULT adapter_hr = adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter));
    DXMTD3D12Trace("Entry",
                   "D3D12CreateDevice fallback-to-default adapter QI "
                   "IMTLDXGIAdapter hr=0x%lx",
                   adapter_hr);
    if (FAILED(adapter_hr)) {
      ERR("D3D12CreateDevice: fallback default adapter is not DXMT");
      return E_FAIL;
    }
  }

  if (support_probe) {
    DXMTD3D12Trace("Entry", "D3D12CreateDevice SUPPORT PROBE SUCCESS FL=%d",
                   MinimumFeatureLevel);
    return S_FALSE;
  }

  try {
    void *device_mem =
        VirtualAlloc((void *)0x500000000ULL, sizeof(MTLD3D12DXGIDevice),
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!device_mem) {
      device_mem =
          VirtualAlloc((void *)0x200000000ULL, sizeof(MTLD3D12DXGIDevice),
                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!device_mem) {
      device_mem = VirtualAlloc(nullptr, sizeof(MTLD3D12DXGIDevice),
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    {
      DXMTD3D12Trace("Entry", "Device allocated at %p size=%zu", device_mem,
                     sizeof(MTLD3D12DXGIDevice));
    }
    auto dxgi_device = new (device_mem) MTLD3D12DXGIDevice(
        CreateDXMTDevice({.device = dxgi_adapter->GetMTLDevice()}),
        dxgi_adapter.ptr());

    HRESULT hr = dxgi_device->QueryInterface(riid, ppDevice);
    if (FAILED(hr)) {
      {
        DXMTD3D12Trace("Entry", "D3D12CreateDevice QI FAILED hr=0x%lx FL=%d",
                       hr, MinimumFeatureLevel);
      }
      dxgi_device->Release();
      return hr;
    }

    Logger::info(str::format("D3D12CreateDevice: created device with FL ",
                             MinimumFeatureLevel));
    {
      DXMTD3D12Trace("Entry", "D3D12CreateDevice SUCCESS FL=%d",
                     MinimumFeatureLevel);
    }
    return S_OK;
  } catch (const std::exception &e) {
    Logger::err(str::format("D3D12CreateDevice: exception: ", e.what()));
    {
      DXMTD3D12Trace("Entry", "D3D12CreateDevice EXCEPTION: %s FL=%d", e.what(),
                     MinimumFeatureLevel);
    }
    return E_FAIL;
  }
}

extern "C" HRESULT WINAPI
D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
                            D3D_ROOT_SIGNATURE_VERSION Version,
                            ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob) {
  {
    FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
    if (f) {
      fprintf(f, "D3D12SerializeRootSignature version=%u params=%u\n", Version,
              pRootSignature ? pRootSignature->NumParameters : 0);
      fclose(f);
    }
  }
  return _SerializeRootSig(pRootSignature, ppBlob);
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
    ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob) {
  {
    FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
    if (f) {
      fprintf(f, "D3D12SerializeVersionedRootSignature version=%u\n",
              pRootSignature ? pRootSignature->Version : 0);
      fclose(f);
    }
  }
  if (!pRootSignature)
    return E_INVALIDARG;
  if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1_0)
    return _SerializeRootSig(&pRootSignature->Desc_1_0, ppBlob);
  if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
    const auto &d1 = pRootSignature->Desc_1_1;
    D3D12_ROOT_SIGNATURE_DESC desc0 = {};
    desc0.NumParameters = d1.NumParameters;
    desc0.NumStaticSamplers = d1.NumStaticSamplers;
    desc0.pStaticSamplers = d1.pStaticSamplers;
    desc0.Flags = d1.Flags;
    std::vector<D3D12_ROOT_PARAMETER> params(d1.NumParameters);
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    for (UINT i = 0; i < d1.NumParameters; i++) {
      auto &src = d1.pParameters[i];
      auto &dst = params[i];
      dst.ParameterType = src.ParameterType;
      dst.ShaderVisibility = src.ShaderVisibility;
      switch (src.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        dst.Constants = src.Constants;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        dst.Descriptor.ShaderRegister = src.Descriptor.ShaderRegister;
        dst.Descriptor.RegisterSpace = src.Descriptor.RegisterSpace;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
        dst.DescriptorTable.NumDescriptorRanges =
            src.DescriptorTable.NumDescriptorRanges;
        size_t base = ranges.size();
        for (UINT r = 0; r < src.DescriptorTable.NumDescriptorRanges; r++) {
          auto &rs = src.DescriptorTable.pDescriptorRanges[r];
          D3D12_DESCRIPTOR_RANGE dr = {};
          dr.RangeType = rs.RangeType;
          dr.NumDescriptors = rs.NumDescriptors;
          dr.BaseShaderRegister = rs.BaseShaderRegister;
          dr.RegisterSpace = rs.RegisterSpace;
          dr.OffsetInDescriptorsFromTableStart =
              rs.OffsetInDescriptorsFromTableStart;
          ranges.push_back(dr);
        }
        dst.DescriptorTable.pDescriptorRanges = ranges.data() + base;
        break;
      }
      }
    }
    desc0.pParameters = params.data();
    return _SerializeRootSig(&desc0, ppBlob);
  }
  return E_INVALIDARG;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    const void *pData, SIZE_T NumBytes, REFIID riid, void **ppDeserializer) {
  if (!ppDeserializer)
    return E_POINTER;
  *ppDeserializer = nullptr;

  auto *deserializer = new _RSDeserializer(pData, NumBytes);
  if (!deserializer->Valid()) {
    deserializer->Release();
    TraceAgility(
        "D3D12CreateRootSignatureDeserializer bytes=%zu -> E_INVALIDARG",
        NumBytes);
    return E_INVALIDARG;
  }

  HRESULT hr = deserializer->QueryInterface(riid, ppDeserializer);
  deserializer->Release();
  TraceAgility(
      "D3D12CreateRootSignatureDeserializer bytes=%zu riid=%s -> 0x%lx",
      NumBytes, str::format(riid).c_str(), hr);
  return hr;
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    const void *pData, SIZE_T NumBytes, REFIID riid, void **ppDeserializer) {
  return D3D12CreateRootSignatureDeserializer(pData, NumBytes, riid,
                                              ppDeserializer);
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **ppDebug) {
  EnsureAppLocalAgilityRuntimeLoaded();
  TraceAgility("D3D12GetDebugInterface riid=%s out=%p -> E_NOINTERFACE",
               str::format(riid).c_str(), ppDebug);
  if (ppDebug)
    *ppDebug = nullptr;
  return E_NOINTERFACE;
}

extern "C" UINT D3D12SDKVersion = kD3D12AgilitySDKVersion;
extern "C" const char D3D12SDKPath[] = ".\\D3D12\\x64\\";

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT feature_count, const IID *iids, void *configurations,
    UINT *configuration_sizes) {
  EnsureAppLocalAgilityRuntimeLoaded();
  if (feature_count && !iids)
    return E_INVALIDARG;

  TraceAgility("D3D12EnableExperimentalFeatures count=%u configs=%p sizes=%p",
               feature_count, configurations, configuration_sizes);
  for (UINT i = 0; i < feature_count; i++) {
    TraceAgility("  feature[%u]=%s", i, str::format(iids[i]).c_str());
  }
  return S_OK;
}

extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid,
                                            void **ppv) {
  EnsureAppLocalAgilityRuntimeLoaded();
  TraceAgility("D3D12GetInterface ENTER clsid=%s riid=%s out=%p",
               str::format(clsid).c_str(), str::format(riid).c_str(), ppv);
  if (!ppv)
    return E_POINTER;
  *ppv = nullptr;
  if (clsid == kCLSID_D3D12SDKConfiguration) {
    auto *configuration = new MTLD3D12SDKConfiguration();
    HRESULT hr = configuration->QueryInterface(riid, ppv);
    configuration->Release();
    TraceAgility("D3D12GetInterface SDKConfiguration riid=%s -> 0x%lx out=%p",
                 str::format(riid).c_str(), hr, ppv ? *ppv : nullptr);
    return hr;
  }
  if (clsid == kCLSID_D3D12DeviceFactory || clsid == kIID_ID3D12DeviceFactory) {
    auto *factory = new MTLD3D12DeviceFactory();
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    TraceAgility("D3D12GetInterface DeviceFactory riid=%s -> 0x%lx out=%p",
                 str::format(riid).c_str(), hr, ppv ? *ppv : nullptr);
    return hr;
  }
  if (riid == kIID_ID3D12DeviceConfiguration ||
      riid == kIID_ID3D12DeviceConfiguration1) {
    return CreateD3D12DeviceConfiguration(riid, ppv);
  }
  if (clsid == kCLSID_D3D12StateObjectFactory ||
      clsid == kIID_ID3D12StateObjectDatabaseFactory) {
    auto *factory = new MTLD3D12StateObjectDatabaseFactory();
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    TraceAgility("D3D12GetInterface StateObjectFactory riid=%s -> 0x%lx out=%p",
                 str::format(riid).c_str(), hr, ppv ? *ppv : nullptr);
    return hr;
  }
  if (clsid == kCLSID_D3D12RuntimeValidationControl) {
    auto *control = new MTLD3D12RuntimeValidationControl();
    HRESULT hr = control->QueryInterface(riid, ppv);
    control->Release();
    TraceAgility(
        "D3D12GetInterface RuntimeValidationControl riid=%s -> 0x%lx out=%p",
        str::format(riid).c_str(), hr, ppv ? *ppv : nullptr);
    return hr;
  }
  if (clsid == kCLSID_D3D12ApplicationIdentity) {
    auto *identity = new MTLD3D12ApplicationIdentity();
    HRESULT hr = identity->QueryInterface(riid, ppv);
    identity->Release();
    TraceAgility(
        "D3D12GetInterface ApplicationIdentity riid=%s -> 0x%lx out=%p",
        str::format(riid).c_str(), hr, ppv ? *ppv : nullptr);
    return hr;
  }

  Logger::warn(str::format("D3D12GetInterface: clsid=", clsid, " riid=", riid,
                           " -> E_NOINTERFACE"));
  TraceAgility("D3D12GetInterface clsid=%s riid=%s -> E_NOINTERFACE",
               str::format(clsid).c_str(), str::format(riid).c_str());
  return E_NOINTERFACE;
}

#ifdef _WIN32
extern void install_crash_handler();
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    install_crash_handler();
    FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
    if (f) {
      char exe[MAX_PATH];
      GetModuleFileNameA(NULL, exe, MAX_PATH);
      HMODULE exe_module = GetModuleHandleA(nullptr);
      auto *sdk_version = exe_module ? reinterpret_cast<UINT *>(GetProcAddress(
                                           exe_module, "D3D12SDKVersion"))
                                     : nullptr;
      FARPROC sdk_path_export =
          exe_module ? GetProcAddress(exe_module, "D3D12SDKPath") : nullptr;
      const char *sdk_path = ResolveSdkPathExportValue(sdk_path_export);
      fprintf(f, "=== d3d12.dll DllMain PROCESS_ATTACH pid=%lu exe=[%s] ===\n",
              GetCurrentProcessId(), exe);
      fprintf(f,
              "=== d3d12.dll DllMain exports exe_module=%p version_ptr=%p "
              "version=%u path_export=%p resolved_path=%p path=[%s] ===\n",
              exe_module, sdk_version, sdk_version ? *sdk_version : 0,
              sdk_path_export, sdk_path, sdk_path ? sdk_path : "");
      fclose(f);
    }
  } else if (reason == DLL_PROCESS_DETACH) {
    dxmt::ShutdownAsyncPipelineCompiler();
    FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
    if (f) {
      char exe[MAX_PATH];
      GetModuleFileNameA(NULL, exe, MAX_PATH);
      fprintf(f, "=== d3d12.dll DllMain PROCESS_DETACH pid=%lu exe=[%s] ===\n",
              GetCurrentProcessId(), exe);
      fclose(f);
    }
  }
  return TRUE;
}
#endif
