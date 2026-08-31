#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <d3d12.h>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

struct ModuleInfo {
    std::string name;
    HMODULE handle = nullptr;
    std::string path;
    bool loaded = false;
    bool has_required_symbol = false;
    uint32_t exported_sdk_version = 0;
    bool has_exported_sdk_version = false;
};

struct InterfaceProbe {
    const char* name;
    GUID iid;
    HRESULT hr = E_FAIL;
    bool supported = false;
    bool requires_contract_review = false;
};

static const GUID IID_D3D12Device0Probe = {
    0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID IID_D3D12Device1Probe = {
    0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}};
static const GUID IID_D3D12Device2Probe = {
    0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};
static const GUID IID_D3D12Device5Probe = {
    0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}};
static const GUID IID_D3D12Device10Probe = {
    0x517f8718, 0xaa66, 0x49f9, {0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x31}};
static const GUID IID_D3D12Device11Probe = {
    0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
static const GUID IID_D3D12Device12Probe = {
    0x5af5c532, 0x4c91, 0x4cd0, {0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5}};
static const GUID CLSID_D3D12SDKConfigurationProbe = {
    0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
static const GUID CLSID_D3D12StateObjectFactoryProbe = {
    0x54e1c9f3, 0x1303, 0x4112, {0xbf, 0x8e, 0x7b, 0xf2, 0xbb, 0x60, 0x6a, 0x73}};
static const GUID IID_ID3D12SDKConfiguration1Probe = {
    0x8aaf9303, 0xad25, 0x48b9, {0x9a, 0x57, 0xd9, 0xc3, 0x7e, 0x00, 0x9d, 0x9f}};
static const GUID IID_ID3D12DeviceConfiguration1Probe = {
    0xed342442, 0x6343, 0x4e16, {0xbb, 0x82, 0xa3, 0xa5, 0x77, 0x87, 0x4e, 0x56}};
static const GUID IID_ID3D12StateObjectDatabaseFactoryProbe = {
    0xf5b066f0, 0x648a, 0x4611, {0xbd, 0x41, 0x27, 0xfd, 0x09, 0x48, 0xb9, 0xeb}};
static const GUID IID_ID3D12StateObjectDatabaseProbe = {
    0xc56060b7, 0xb5fc, 0x4135, {0x98, 0xe0, 0xa1, 0xe9, 0x99, 0x7e, 0xac, 0xe0}};

enum D3D12DeviceFlagsCompat : UINT {
    D3D12DeviceFlagNone = 0,
};

struct D3D12DeviceConfigurationDescCompat {
    D3D12DeviceFlagsCompat Flags;
    UINT GpuBasedValidationFlags;
    UINT SDKVersion;
    UINT NumEnabledExperimentalFeatures;
};

struct ID3D12SDKConfiguration1Compat : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT SDKVersion, LPCSTR SDKPath) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT SDKVersion, LPCSTR SDKPath, REFIID riid,
                                                          void** ppvFactory) = 0;
    virtual void STDMETHODCALLTYPE FreeUnusedSDKs() = 0;
};

struct ID3D12DeviceConfiguration1Compat : public IUnknown {
    virtual D3D12DeviceConfigurationDescCompat* STDMETHODCALLTYPE GetDesc(D3D12DeviceConfigurationDescCompat* ret) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEnabledExperimentalFeatures(GUID* guids, UINT num_guids) = 0;
    virtual HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                                      ID3DBlob** result, ID3DBlob** error) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(const void* blob, SIZE_T size,
                                                                               REFIID riid, void** deserializer) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
        const void* library_blob, SIZE_T size, LPCWSTR root_signature_subobject_name, REFIID riid,
        void** deserializer) = 0;
};

typedef void(STDMETHODCALLTYPE* D3D12PipelineStateFuncCompat)(const void* key, UINT key_size, UINT version,
                                                              const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                                              void* context);
typedef void(STDMETHODCALLTYPE* D3D12StateObjectFuncCompat)(const void* key, UINT key_size, UINT version,
                                                            const D3D12_STATE_OBJECT_DESC* desc, const void* parent_key,
                                                            UINT parent_key_size, void* context);

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

typedef void(STDMETHODCALLTYPE* D3D12ApplicationDescFuncCompat)(
    const D3D12ApplicationDescCompat* application_desc, void* context);

struct ID3D12StateObjectDatabaseCompat : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetApplicationDesc(const void* application_desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetApplicationDesc(void* callback, void* context) = 0;
    virtual HRESULT STDMETHODCALLTYPE StorePipelineStateDesc(const void* key, UINT key_size, UINT version,
                                                             const D3D12_PIPELINE_STATE_STREAM_DESC* desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindPipelineStateDesc(const void* key, UINT key_size,
                                                            D3D12PipelineStateFuncCompat callback, void* context) = 0;
    virtual HRESULT STDMETHODCALLTYPE StoreStateObjectDesc(const void* key, UINT key_size, UINT version,
                                                           const D3D12_STATE_OBJECT_DESC* desc, const void* parent_key,
                                                           UINT parent_key_size) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindStateObjectDesc(const void* key, UINT key_size,
                                                          D3D12StateObjectFuncCompat callback, void* context) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindObjectVersion(const void* key, UINT key_size, UINT* version) = 0;
};

struct ID3D12StateObjectDatabaseFactoryCompat : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateStateObjectDatabaseFromFile(LPCWSTR database_file, UINT flags, REFIID riid,
                                                                        void** state_object_database) = 0;
};

static const GUID IID_ID3D12CompilerFactoryProbe = {
    0xc1ee4b59, 0x3f59, 0x47a5, {0x9b, 0x4e, 0xa8, 0x55, 0xc8, 0x58, 0xa8, 0x78}};
static const GUID IID_ID3D12CompilerFactoryChildProbe = {
    0xe0d06420, 0x9f31, 0x47e8, {0xae, 0x9a, 0xdd, 0x2b, 0xa2, 0x5a, 0xc0, 0xbc}};
static const GUID IID_ID3D12CompilerCacheSessionProbe = {
    0x5704e5e6, 0x054b, 0x4738, {0xb6, 0x61, 0x7b, 0x0d, 0x68, 0xd8, 0xdd, 0xe2}};
static const GUID IID_ID3D12CompilerProbe = {
    0x8c403c12, 0x993b, 0x4583, {0x80, 0xf1, 0x68, 0x24, 0x13, 0x8f, 0xa6, 0x8e}};
static const GUID IID_ID3D12CompilerStateObjectProbe = {
    0x5981cca4, 0xe8ae, 0x44ca, {0x9b, 0x92, 0x4f, 0xa8, 0x6f, 0x5a, 0x3a, 0x3a}};

struct CompilerAdapterFamilyCompat {
    WCHAR szAdapterFamily[128];
};
enum CompilerValueTypeCompat : UINT {
    CompilerValueTypeObjectCode = 0,
    CompilerValueTypeMetadata = 1,
    CompilerValueTypeDebugPdb = 2,
    CompilerValueTypePerformanceData = 3,
};
enum CompilerValueTypeFlagsCompat : UINT {
    CompilerValueTypeFlagsNone = 0,
    CompilerValueTypeFlagsObjectCode = 1,
    CompilerValueTypeFlagsMetadata = 2,
    CompilerValueTypeFlagsDebugPdb = 4,
    CompilerValueTypeFlagsPerformanceData = 8,
};
struct CompilerDatabasePathCompat {
    CompilerValueTypeFlagsCompat Types;
    LPCWSTR pPath;
};
struct CompilerCacheGroupKeyCompat {
    const void* pKey;
    UINT KeySize;
};
struct CompilerCacheValueKeyCompat {
    const void* pKey;
    UINT KeySize;
};
struct CompilerCacheValueCompat {
    void* pValue;
    UINT ValueSize;
};
struct CompilerCacheTypedValueCompat {
    CompilerValueTypeCompat Type;
    CompilerCacheValueCompat Value;
};
struct CompilerCacheConstValueCompat {
    const void* pValue;
    UINT ValueSize;
};
struct CompilerCacheTypedConstValueCompat {
    CompilerValueTypeCompat Type;
    CompilerCacheConstValueCompat Value;
};
struct CompilerTargetCompat {
    UINT AdapterFamilyIndex;
    UINT64 ABIVersion;
};
union CompilerVersionNumberCompat {
    UINT64 Version;
    UINT16 VersionParts[4];
};
struct ID3D12CompilerFactoryChildProbe : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void** factory) = 0;
};
using CompilerCacheAllocationFunc = void* (__stdcall *)(SIZE_T, void*);
using CompilerGroupValueKeysFunc = void (__stdcall *)(const CompilerCacheValueKeyCompat*, void*);
using CompilerGroupValuesFunc = void (__stdcall *)(UINT, const CompilerCacheTypedConstValueCompat*, void*);
struct ID3D12CompilerCacheSessionProbe : public ID3D12CompilerFactoryChildProbe {
    virtual HRESULT STDMETHODCALLTYPE FindGroup(const CompilerCacheGroupKeyCompat*, UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindGroupValueKeys(const CompilerCacheGroupKeyCompat*, const UINT*, CompilerGroupValueKeysFunc, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindGroupValues(const CompilerCacheGroupKeyCompat*, const UINT*, CompilerValueTypeFlagsCompat, CompilerGroupValuesFunc, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindValue(const CompilerCacheValueKeyCompat*, CompilerCacheTypedValueCompat*, UINT, CompilerCacheAllocationFunc, void*) = 0;
    virtual const D3D12ApplicationDescCompat* STDMETHODCALLTYPE GetApplicationDesc() = 0;
#if defined(_MSC_VER) || !defined(_WIN32)
    virtual CompilerTargetCompat STDMETHODCALLTYPE GetCompilerTarget() = 0;
#else
    virtual CompilerTargetCompat* STDMETHODCALLTYPE GetCompilerTarget(CompilerTargetCompat* ret) = 0;
#endif
    virtual CompilerValueTypeFlagsCompat STDMETHODCALLTYPE GetValueTypes() = 0;
    virtual HRESULT STDMETHODCALLTYPE StoreGroupValueKeys(const CompilerCacheGroupKeyCompat*, UINT, const CompilerCacheValueKeyCompat*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE StoreValue(const CompilerCacheValueKeyCompat*, const CompilerCacheTypedConstValueCompat*, UINT) = 0;
};
struct ID3D12CompilerStateObjectProbe : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCompiler(REFIID riid, void** compiler) = 0;
};
struct ID3D12CompilerProbe : public ID3D12CompilerFactoryChildProbe {
    virtual HRESULT STDMETHODCALLTYPE CompilePipelineState(const CompilerCacheGroupKeyCompat*, UINT, const D3D12_PIPELINE_STATE_STREAM_DESC*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CompileStateObject(const CompilerCacheGroupKeyCompat*, UINT, const D3D12_STATE_OBJECT_DESC*, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CompileAddToStateObject(const CompilerCacheGroupKeyCompat*, UINT, const D3D12_STATE_OBJECT_DESC*, ID3D12CompilerStateObjectProbe*, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCacheSession(REFIID, void**) = 0;
};
struct ID3D12CompilerFactoryProbe : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumerateAdapterFamilies(UINT, CompilerAdapterFamilyCompat*) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumerateAdapterFamilyABIVersions(UINT, UINT32*, UINT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumerateAdapterFamilyCompilerVersion(UINT, CompilerVersionNumberCompat*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetApplicationProfileVersion(const CompilerTargetCompat*, const D3D12ApplicationDescCompat*, CompilerVersionNumberCompat*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompilerCacheSession(const CompilerDatabasePathCompat*, UINT, const CompilerTargetCompat*, const D3D12ApplicationDescCompat*, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompiler(ID3D12CompilerCacheSessionProbe*, REFIID, void**) = 0;
};

// Agility 1.619 state-database-only descriptor forms are not present in the
// vendored legacy header. Their values and layouts are defined by the stable
// State Object Database contract.
static constexpr UINT kGlobalSerializedRootSignatureType = 31u;
static constexpr UINT kLocalSerializedRootSignatureType = 32u;
static constexpr UINT kExistingCollectionByKeyType = 36u;
struct SerializedRootSignatureDescCompat {
    const void* pSerializedBlob;
    SIZE_T SerializedBlobSizeInBytes;
};
struct GlobalSerializedRootSignatureCompat {
    SerializedRootSignatureDescCompat Desc;
};
struct LocalSerializedRootSignatureCompat {
    SerializedRootSignatureDescCompat Desc;
};
struct ExistingCollectionByKeyDescCompat {
    const void* pKey;
    UINT KeySize;
    UINT NumExports;
    const D3D12_EXPORT_DESC* pExports;
};

struct ApplicationCallbackState {
    bool called = false;
    std::wstring exe;
    std::wstring name;
    std::wstring engine;
    UINT64 version = 0;
    UINT64 engine_version = 0;
};

static void STDMETHODCALLTYPE application_desc_callback(
    const D3D12ApplicationDescCompat* desc, void* context) {
    auto* state = static_cast<ApplicationCallbackState*>(context);
    if (!state || !desc)
        return;
    state->called = true;
    state->exe = desc->pExeFilename ? desc->pExeFilename : L"";
    state->name = desc->pName ? desc->pName : L"";
    state->engine = desc->pEngineName ? desc->pEngineName : L"";
    state->version = desc->Version.Version;
    state->engine_version = desc->EngineVersion.Version;
}

struct PipelineCallbackState {
    bool called = false;
    UINT version = 0;
    SIZE_T size = 0;
};

static void STDMETHODCALLTYPE pipeline_state_callback(const void*, UINT, UINT version,
                                                      const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* context) {
    auto* state = static_cast<PipelineCallbackState*>(context);
    if (!state)
        return;
    state->called = true;
    state->version = version;
    state->size = desc ? desc->SizeInBytes : 0;
}

struct StateObjectCallbackState {
    bool called = false;
    UINT version = 0;
    D3D12_STATE_OBJECT_TYPE type = D3D12_STATE_OBJECT_TYPE_COLLECTION;
    UINT subobject_count = UINT_MAX;
    D3D12_STATE_SUBOBJECT_TYPE first_subobject_type =
        D3D12_STATE_SUBOBJECT_TYPE_MAX_VALID;
    D3D12_STATE_OBJECT_FLAGS config_flags = D3D12_STATE_OBJECT_FLAG_NONE;
    UINT node_mask = 0;
    UINT max_payload_size = 0;
    UINT max_attribute_size = 0;
    UINT max_recursion_depth = 0;
    UINT pipeline_flags = 0;
    bool dxil_library_present = false;
    UINT dxil_library_size = 0;
    UINT dxil_first_byte = 0;
    UINT dxil_export_count = 0;
    std::wstring dxil_first_export;
    std::wstring dxil_first_rename;
    UINT dxil_first_export_flags = 0;
    bool hit_group_present = false;
    UINT hit_group_type = 0;
    std::wstring hit_group_export;
    std::wstring hit_group_any_hit;
    std::wstring hit_group_closest_hit;
    std::wstring hit_group_intersection;
    bool dxil_association_present = false;
    std::wstring dxil_association_target;
    UINT dxil_association_export_count = 0;
    std::wstring dxil_association_first_export;
    bool subobject_association_present = false;
    UINT subobject_association_target_type = D3D12_STATE_SUBOBJECT_TYPE_MAX_VALID;
    UINT subobject_association_export_count = 0;
    std::wstring subobject_association_first_export;
    bool serialized_root_signature_present = false;
    UINT serialized_root_signature_type_mask = 0;
    SIZE_T serialized_root_signature_size = 0;
    UINT serialized_root_signature_first_byte = 0;
    bool existing_collection_by_key_present = false;
    UINT existing_collection_by_key_size = 0;
    UINT existing_collection_by_key_first_byte = 0;
    UINT existing_collection_by_key_export_count = 0;
    std::wstring existing_collection_by_key_first_export;
    std::wstring existing_collection_by_key_first_rename;
    std::array<uint8_t, 4> parent_key = {};
    UINT parent_key_size = 0;
};

static void STDMETHODCALLTYPE state_object_callback(
    const void*, UINT, UINT version, const D3D12_STATE_OBJECT_DESC* desc,
    const void* parent_key, UINT parent_key_size, void* context) {
    auto* state = static_cast<StateObjectCallbackState*>(context);
    if (!state)
        return;
    state->called = true;
    state->version = version;
    state->type = desc ? desc->Type : D3D12_STATE_OBJECT_TYPE_COLLECTION;
    state->subobject_count = desc ? desc->NumSubobjects : UINT_MAX;
    if (desc && desc->NumSubobjects && desc->pSubobjects) {
        state->first_subobject_type = desc->pSubobjects[0].Type;
        for (UINT i = 0; i < desc->NumSubobjects; ++i) {
            const auto& subobject = desc->pSubobjects[i];
            if (!subobject.pDesc)
                continue;
            if (static_cast<UINT>(subobject.Type) ==
                kGlobalSerializedRootSignatureType ||
                static_cast<UINT>(subobject.Type) ==
                kLocalSerializedRootSignatureType) {
                const auto* serialized =
                    static_cast<const GlobalSerializedRootSignatureCompat*>(
                        subobject.pDesc);
                state->serialized_root_signature_present =
                    serialized->Desc.pSerializedBlob != nullptr;
                state->serialized_root_signature_type_mask |=
                    static_cast<UINT>(subobject.Type) ==
                            kGlobalSerializedRootSignatureType
                        ? 1u
                        : 2u;
                state->serialized_root_signature_size =
                    serialized->Desc.SerializedBlobSizeInBytes;
                if (serialized->Desc.pSerializedBlob &&
                    serialized->Desc.SerializedBlobSizeInBytes)
                    state->serialized_root_signature_first_byte =
                        static_cast<const uint8_t*>(
                            serialized->Desc.pSerializedBlob)[0];
            } else if (static_cast<UINT>(subobject.Type) ==
                       kExistingCollectionByKeyType) {
                const auto* collection =
                    static_cast<const ExistingCollectionByKeyDescCompat*>(
                        subobject.pDesc);
                state->existing_collection_by_key_present =
                    collection->pKey != nullptr;
                state->existing_collection_by_key_size = collection->KeySize;
                if (collection->pKey && collection->KeySize)
                    state->existing_collection_by_key_first_byte =
                        static_cast<const uint8_t*>(collection->pKey)[0];
                state->existing_collection_by_key_export_count =
                    collection->NumExports;
                if (collection->NumExports && collection->pExports) {
                    state->existing_collection_by_key_first_export =
                        collection->pExports[0].Name
                            ? collection->pExports[0].Name
                            : L"";
                    state->existing_collection_by_key_first_rename =
                        collection->pExports[0].ExportToRename
                            ? collection->pExports[0].ExportToRename
                            : L"";
                }
            } else switch (subobject.Type) {
            case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
                state->config_flags =
                    static_cast<const D3D12_STATE_OBJECT_CONFIG*>(
                        subobject.pDesc)->Flags;
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
                state->node_mask =
                    static_cast<const D3D12_NODE_MASK*>(subobject.pDesc)->NodeMask;
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
                const auto* config =
                    static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(
                        subobject.pDesc);
                state->max_payload_size = config->MaxPayloadSizeInBytes;
                state->max_attribute_size = config->MaxAttributeSizeInBytes;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
                state->max_recursion_depth =
                    static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(
                        subobject.pDesc)->MaxTraceRecursionDepth;
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: {
                const auto* config =
                    static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(
                        subobject.pDesc);
                state->max_recursion_depth = config->MaxTraceRecursionDepth;
                state->pipeline_flags = static_cast<UINT>(config->Flags);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
                const auto* library =
                    static_cast<const D3D12_DXIL_LIBRARY_DESC*>(
                        subobject.pDesc);
                state->dxil_library_present = library != nullptr;
                if (library) {
                    state->dxil_library_size =
                        static_cast<UINT>(library->DXILLibrary.BytecodeLength);
                    if (library->DXILLibrary.pShaderBytecode &&
                        library->DXILLibrary.BytecodeLength)
                        state->dxil_first_byte = static_cast<const uint8_t*>(
                            library->DXILLibrary.pShaderBytecode)[0];
                    state->dxil_export_count = library->NumExports;
                    if (library->NumExports && library->pExports) {
                        state->dxil_first_export =
                            library->pExports[0].Name
                                ? library->pExports[0].Name
                                : L"";
                        state->dxil_first_rename =
                            library->pExports[0].ExportToRename
                                ? library->pExports[0].ExportToRename
                                : L"";
                        state->dxil_first_export_flags =
                            static_cast<UINT>(library->pExports[0].Flags);
                    }
                }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
                const auto* hit_group =
                    static_cast<const D3D12_HIT_GROUP_DESC*>(subobject.pDesc);
                state->hit_group_present = hit_group != nullptr;
                if (hit_group) {
                    state->hit_group_type = static_cast<UINT>(hit_group->Type);
                    state->hit_group_export =
                        hit_group->HitGroupExport ? hit_group->HitGroupExport : L"";
                    state->hit_group_any_hit = hit_group->AnyHitShaderImport
                                                   ? hit_group->AnyHitShaderImport
                                                   : L"";
                    state->hit_group_closest_hit =
                        hit_group->ClosestHitShaderImport
                            ? hit_group->ClosestHitShaderImport
                            : L"";
                    state->hit_group_intersection =
                        hit_group->IntersectionShaderImport
                            ? hit_group->IntersectionShaderImport
                            : L"";
                }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                const auto* association =
                    static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(
                        subobject.pDesc);
                state->dxil_association_present = association != nullptr;
                if (association) {
                    state->dxil_association_target =
                        association->SubobjectToAssociate
                            ? association->SubobjectToAssociate
                            : L"";
                    state->dxil_association_export_count = association->NumExports;
                    if (association->NumExports && association->pExports)
                        state->dxil_association_first_export =
                            association->pExports[0] ? association->pExports[0] : L"";
                }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                const auto* association =
                    static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(
                        subobject.pDesc);
                state->subobject_association_present = association != nullptr;
                if (association) {
                    state->subobject_association_target_type =
                        association->pSubobjectToAssociate
                            ? static_cast<UINT>(
                                  association->pSubobjectToAssociate->Type)
                            : static_cast<UINT>(D3D12_STATE_SUBOBJECT_TYPE_MAX_VALID);
                    state->subobject_association_export_count =
                        association->NumExports;
                    if (association->NumExports && association->pExports)
                        state->subobject_association_first_export =
                            association->pExports[0] ? association->pExports[0] : L"";
                }
                break;
            }
            default:
                break;
            }
        }
    }
    state->parent_key_size = parent_key_size;
    if (parent_key && parent_key_size == state->parent_key.size())
        std::memcpy(state->parent_key.data(), parent_key, parent_key_size);
}

static std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

static std::string narrow_ascii(const wchar_t* value) {
    std::string result;
    if (!value)
        return result;
    while (*value) {
        result.push_back(*value < 0x80 ? static_cast<char>(*value) : '?');
        ++value;
    }
    return result;
}

static std::string getenv_string(const char* key) {
    DWORD needed = GetEnvironmentVariableA(key, nullptr, 0);
    if (needed == 0)
        return "";
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (written == 0)
        return "";
    value.resize(written);
    return value;
}

struct CompilerCacheCallbackState {
    std::vector<std::vector<uint8_t>> keys;
    std::vector<std::pair<CompilerValueTypeCompat, std::vector<uint8_t>>> values;
};

static void __stdcall compiler_group_value_keys_callback(
    const CompilerCacheValueKeyCompat* key, void* context) {
    auto* state = static_cast<CompilerCacheCallbackState*>(context);
    if (!state || !key || (!key->pKey && key->KeySize))
        return;
    const auto* bytes = static_cast<const uint8_t*>(key->pKey);
    state->keys.emplace_back(bytes, bytes + key->KeySize);
}

static void __stdcall compiler_group_values_callback(
    UINT, const CompilerCacheTypedConstValueCompat* value, void* context) {
    auto* state = static_cast<CompilerCacheCallbackState*>(context);
    if (!state || !value || (!value->Value.pValue && value->Value.ValueSize))
        return;
    const auto* bytes = static_cast<const uint8_t*>(value->Value.pValue);
    state->values.emplace_back(
        value->Type,
        std::vector<uint8_t>(bytes, bytes + value->Value.ValueSize));
}

static void configure_exported_sdk() {
    std::string version_text = getenv_string("D3D12_METAL_SDK_AGILITY_VERSION");
    if (!version_text.empty()) {
        D3D12SDKVersion = static_cast<UINT>(std::strtoul(version_text.c_str(), nullptr, 10));
    }

    std::string sdk_path = getenv_string("D3D12_METAL_SDK_AGILITY_PATH");
    if (!sdk_path.empty()) {
        std::snprintf(D3D12SDKPath, sizeof(D3D12SDKPath), "%s", sdk_path.c_str());
    }
}

static std::string normalize_windows_path(std::string value) {
    for (char& ch : value) {
        if (ch == '/')
            ch = '\\';
    }
    return value;
}

static std::string join_windows_path(const std::string& base, const std::string& child) {
    if (base.empty())
        return child;
    if (child.empty())
        return base;
    if (base.back() == '\\' || base.back() == '/')
        return base + child;
    return base + "\\" + child;
}

static std::string module_path(HMODULE module) {
    char buffer[4096];
    DWORD written = GetModuleFileNameA(module, buffer, sizeof(buffer));
    if (written == 0)
        return "";
    if (written >= sizeof(buffer))
        written = sizeof(buffer) - 1;
    return std::string(buffer, written);
}

static std::string lower_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

static bool contains_ascii_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    return lower_ascii(haystack).find(lower_ascii(needle)) != std::string::npos;
}

static uint64_t fnv1a_file_hash(const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    uint64_t hash = 1469598103934665603ull;
    unsigned char buffer[64 * 1024];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        for (DWORD i = 0; i < read; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ull;
        }
    }
    CloseHandle(file);
    return hash;
}

static void inspect_module(ModuleInfo& module, const char* required_symbol = nullptr) {
    module.handle = LoadLibraryA(module.name.c_str());
    module.loaded = module.handle != nullptr;
    if (!module.loaded)
        return;

    module.path = module_path(module.handle);
    module.has_required_symbol =
        required_symbol == nullptr || GetProcAddress(module.handle, required_symbol) != nullptr;
    auto sdk_version = reinterpret_cast<const uint32_t*>(GetProcAddress(module.handle, "D3D12SDKVersion"));
    if (sdk_version) {
        module.exported_sdk_version = *sdk_version;
        module.has_exported_sdk_version = true;
    }
}

static void print_module_json(const ModuleInfo& module, bool last) {
    uint64_t hash = module.loaded ? fnv1a_file_hash(module.path) : 0;
    std::printf("    \"%s\": {\n", json_escape(module.name).c_str());
    std::printf("      \"loaded\": %s,\n", module.loaded ? "true" : "false");
    std::printf("      \"path\": \"%s\",\n", json_escape(module.path).c_str());
    std::printf("      \"fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(hash));
    std::printf("      \"has_required_symbol\": %s,\n", module.has_required_symbol ? "true" : "false");
    std::printf("      \"has_exported_sdk_version\": %s,\n", module.has_exported_sdk_version ? "true" : "false");
    std::printf("      \"exported_sdk_version\": %u\n", module.exported_sdk_version);
    std::printf("    }%s\n", last ? "" : ",");
}

static void print_interface_json(const InterfaceProbe& probe, bool last) {
    std::printf("    \"%s\": {\n", probe.name);
    std::printf("      \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(probe.hr)));
    std::printf("      \"supported\": %s,\n", probe.supported ? "true" : "false");
    std::printf("      \"classification\": \"%s\",\n", probe.supported ? "supported" : "safely_rejected");
    std::printf("      \"requires_contract_review\": %s\n", probe.requires_contract_review ? "true" : "false");
    std::printf("    }%s\n", last ? "" : ",");
}

static void print_hr_field(const char* key, HRESULT hr, bool last = false) {
    std::printf("    \"%s\": \"0x%08lx\"%s\n", key, static_cast<unsigned long>(static_cast<uint32_t>(hr)),
                last ? "" : ",");
}

int main() {
    const wchar_t* database_path = L"Z:\\tmp\\metalsharp-agility-cache.bin";
    DeleteFileW(database_path);
    configure_exported_sdk();
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");
    std::string expected_windows = getenv_string("D3D12_METAL_SDK_EXPECT_WINDOWS_SUBSTR");
    std::string sdk_module_dir = normalize_windows_path(D3D12SDKPath);

    std::vector<ModuleInfo> modules = {
        {join_windows_path(sdk_module_dir, "D3D12Core.dll"), nullptr, "", false, false, 0, false},
        {join_windows_path(sdk_module_dir, "d3d12SDKLayers.dll"), nullptr, "", false, false, 0, false},
        {join_windows_path(sdk_module_dir, "D3D12StateObjectCompiler.dll"), nullptr, "", false, false, 0, false},
        {join_windows_path(sdk_module_dir, "dxil.dll"), nullptr, "", false, false, 0, false},
        {"d3d12.dll", nullptr, "", false, false, 0, false},
    };

    inspect_module(modules[0]);
    inspect_module(modules[1]);
    inspect_module(modules[2]);
    inspect_module(modules[3]);
    inspect_module(modules[4], "D3D12CreateDevice");

    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using D3D12GetInterfaceFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    FARPROC create_device_proc = modules[4].loaded ? GetProcAddress(modules[4].handle, "D3D12CreateDevice") : nullptr;
    FARPROC get_interface_proc = modules[4].loaded ? GetProcAddress(modules[4].handle, "D3D12GetInterface") : nullptr;
    auto create_device = reinterpret_cast<CreateDeviceFn>(reinterpret_cast<void*>(create_device_proc));
    auto d3d12_get_interface = reinterpret_cast<D3D12GetInterfaceFn>(reinterpret_cast<void*>(get_interface_proc));

    IUnknown* device = nullptr;
    HRESULT create_hr = E_FAIL;
    if (create_device)
        create_hr =
            create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12Device0Probe, reinterpret_cast<void**>(&device));

    std::vector<InterfaceProbe> interfaces = {
        {"ID3D12Device", IID_D3D12Device0Probe, E_FAIL, false, false},
        {"ID3D12Device1", IID_D3D12Device1Probe, E_FAIL, false, false},
        {"ID3D12Device2", IID_D3D12Device2Probe, E_FAIL, false, false},
        {"ID3D12Device5", IID_D3D12Device5Probe, E_FAIL, false, false},
        {"ID3D12Device10", IID_D3D12Device10Probe, E_FAIL, false, false},
        {"ID3D12Device11", IID_D3D12Device11Probe, E_FAIL, false, false},
        {"ID3D12Device12", IID_D3D12Device12Probe, E_FAIL, false, false},
    };

    if (device) {
        for (auto& probe : interfaces) {
            void* queried = nullptr;
            probe.hr = device->QueryInterface(probe.iid, &queried);
            probe.supported = SUCCEEDED(probe.hr) && queried != nullptr;
            probe.requires_contract_review = probe.supported && std::strcmp(probe.name, "ID3D12Device") != 0 &&
                                             std::strcmp(probe.name, "ID3D12Device1") != 0 &&
                                             std::strcmp(probe.name, "ID3D12Device2") != 0;
            if (queried)
                reinterpret_cast<IUnknown*>(queried)->Release();
        }
    }

    ID3D12SDKConfiguration1Compat* sdk_config = nullptr;
    HRESULT get_sdk_config_hr =
        d3d12_get_interface ? d3d12_get_interface(CLSID_D3D12SDKConfigurationProbe, IID_ID3D12SDKConfiguration1Probe,
                                                  reinterpret_cast<void**>(&sdk_config))
                            : E_NOINTERFACE;
    HRESULT set_sdk_version_hr = sdk_config ? sdk_config->SetSDKVersion(D3D12SDKVersion, D3D12SDKPath) : E_NOINTERFACE;

    ID3D12DeviceConfiguration1Compat* device_config = nullptr;
    const GUID device_config_probe_clsid = {};
    HRESULT get_device_config_hr =
        d3d12_get_interface ? d3d12_get_interface(device_config_probe_clsid, IID_ID3D12DeviceConfiguration1Probe,
                                                  reinterpret_cast<void**>(&device_config))
                            : E_NOINTERFACE;
    D3D12DeviceConfigurationDescCompat device_config_desc = {};
    D3D12DeviceConfigurationDescCompat* device_config_desc_ptr =
        device_config ? device_config->GetDesc(&device_config_desc) : nullptr;

    D3D12_ROOT_SIGNATURE_DESC1 root_desc1 = {};
    root_desc1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned_root_desc = {};
    versioned_root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned_root_desc.Desc_1_1 = root_desc1;
    ID3DBlob* root_blob = nullptr;
    ID3DBlob* root_error = nullptr;
    HRESULT config_serialize_root_hr =
        device_config ? device_config->SerializeVersionedRootSignature(&versioned_root_desc, &root_blob, &root_error)
                      : E_NOINTERFACE;
    ID3D12RootSignatureDeserializer* config_deserializer = nullptr;
    HRESULT config_deserialize_root_hr =
        (device_config && root_blob)
            ? device_config->CreateVersionedRootSignatureDeserializer(
                  root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&config_deserializer))
            : E_NOINTERFACE;
    const D3D12_ROOT_SIGNATURE_DESC* config_deserialized_desc =
        config_deserializer ? config_deserializer->GetRootSignatureDesc() : nullptr;

    ID3D12ShaderCacheSession* shader_cache = nullptr;
    HRESULT create_shader_cache_hr = E_NOINTERFACE;
    HRESULT shader_cache_store_hr = E_NOINTERFACE;
    HRESULT shader_cache_size_hr = E_NOINTERFACE;
    HRESULT shader_cache_find_hr = E_NOINTERFACE;
    std::array<uint8_t, 4> shader_key = {0x73, 0x68, 0x64, 0x72};
    std::array<uint8_t, 4> shader_value = {0xde, 0xad, 0xbe, 0xef};
    std::array<uint8_t, 4> shader_readback = {};
    UINT shader_readback_size = static_cast<UINT>(shader_readback.size());
    if (device) {
        ID3D12Device9* device9 = nullptr;
        HRESULT device9_hr = device->QueryInterface(IID_PPV_ARGS(&device9));
        if (SUCCEEDED(device9_hr) && device9) {
            D3D12_SHADER_CACHE_SESSION_DESC cache_desc = {};
            const GUID shader_cache_identifier = {
                0x45524d85, 0xc0aa, 0x1d41, {0x84, 0x3f, 0x56, 0x8f, 0x50, 0x08, 0x44, 0x14}};
            cache_desc.Identifier = shader_cache_identifier;
            cache_desc.Mode = D3D12_SHADER_CACHE_MODE_MEMORY;
            cache_desc.MaximumInMemoryCacheSizeBytes = 4096;
            cache_desc.MaximumInMemoryCacheEntries = 4;
            create_shader_cache_hr = device9->CreateShaderCacheSession(&cache_desc, IID_PPV_ARGS(&shader_cache));
            device9->Release();
        } else {
            create_shader_cache_hr = device9_hr;
        }
    }
    if (shader_cache) {
        shader_cache_store_hr = shader_cache->StoreValue(shader_key.data(), static_cast<UINT>(shader_key.size()),
                                                         shader_value.data(), static_cast<UINT>(shader_value.size()));
        UINT size_query = 0;
        shader_cache_size_hr =
            shader_cache->FindValue(shader_key.data(), static_cast<UINT>(shader_key.size()), nullptr, &size_query);
        shader_cache_find_hr = shader_cache->FindValue(shader_key.data(), static_cast<UINT>(shader_key.size()),
                                                       shader_readback.data(), &shader_readback_size);
    }

    ID3D12StateObjectDatabaseFactoryCompat* database_factory = nullptr;
    HRESULT get_database_factory_hr =
        d3d12_get_interface
            ? d3d12_get_interface(CLSID_D3D12StateObjectFactoryProbe, IID_ID3D12StateObjectDatabaseFactoryProbe,
                                  reinterpret_cast<void**>(&database_factory))
            : E_NOINTERFACE;
    ID3D12StateObjectDatabaseCompat* database = nullptr;
    HRESULT create_database_hr =
        database_factory ? database_factory->CreateStateObjectDatabaseFromFile(
                               database_path, 0,
                               IID_ID3D12StateObjectDatabaseProbe,
                               reinterpret_cast<void**>(&database))
                         : E_NOINTERFACE;
    wchar_t application_exe[] = L"probe.exe";
    wchar_t application_name[] = L"MetalSharp State DB";
    wchar_t application_engine[] = L"DXMT";
    D3D12ApplicationDescCompat application_desc = {};
    application_desc.pExeFilename = application_exe;
    application_desc.pName = application_name;
    application_desc.Version.Version = 0x0001000200030004ull;
    application_desc.pEngineName = application_engine;
    application_desc.EngineVersion.Version = 0x0005000600070008ull;
    HRESULT set_application_desc_hr =
        database ? database->SetApplicationDesc(&application_desc)
                 : E_NOINTERFACE;
    application_name[0] = L'X';
    application_engine[0] = L'Y';
    ApplicationCallbackState application_callback = {};
    HRESULT get_application_desc_hr =
        database ? database->GetApplicationDesc(
                       reinterpret_cast<void*>(application_desc_callback),
                       &application_callback)
                 : E_NOINTERFACE;

    using CreateCompilerFactoryFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, void**);
    CreateCompilerFactoryFn create_compiler_factory = nullptr;
    FARPROC create_compiler_factory_proc =
        modules[2].loaded
            ? GetProcAddress(modules[2].handle, "D3D12CompilerCreateFactory")
            : nullptr;
    static_assert(sizeof(create_compiler_factory) ==
                      sizeof(create_compiler_factory_proc),
                  "compiler factory function pointer size mismatch");
    std::memcpy(&create_compiler_factory, &create_compiler_factory_proc,
                sizeof(create_compiler_factory));
    ID3D12CompilerFactoryProbe* compiler_factory = nullptr;
    HRESULT compiler_factory_hr =
        create_compiler_factory
            ? create_compiler_factory(L"d3d12.dll", IID_ID3D12CompilerFactoryProbe,
                                      reinterpret_cast<void**>(&compiler_factory))
            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    CompilerAdapterFamilyCompat compiler_family = {};
    HRESULT compiler_family_hr = compiler_factory
                                     ? compiler_factory->EnumerateAdapterFamilies(
                                           0, &compiler_family)
                                     : E_NOINTERFACE;
    CompilerAdapterFamilyCompat compiler_family_end = {};
    HRESULT compiler_family_end_hr = compiler_factory
                                         ? compiler_factory->EnumerateAdapterFamilies(
                                               1, &compiler_family_end)
                                         : E_NOINTERFACE;
    UINT compiler_abi_count = 0;
    HRESULT compiler_abi_size_hr =
        compiler_factory
            ? compiler_factory->EnumerateAdapterFamilyABIVersions(
                  0, &compiler_abi_count, nullptr)
            : E_NOINTERFACE;
    UINT64 compiler_abi_version = 0;
    UINT compiler_abi_capacity = compiler_abi_count;
    HRESULT compiler_abi_hr =
        compiler_factory
            ? compiler_factory->EnumerateAdapterFamilyABIVersions(
                  0, &compiler_abi_capacity, &compiler_abi_version)
            : E_NOINTERFACE;
    CompilerVersionNumberCompat compiler_version = {};
    HRESULT compiler_version_hr = compiler_factory
                                      ? compiler_factory->EnumerateAdapterFamilyCompilerVersion(
                                            0, &compiler_version)
                                      : E_NOINTERFACE;
    wchar_t compiler_exe[] = L"compiler-probe.exe";
    wchar_t compiler_name[] = L"MetalSharp Compiler Probe";
    wchar_t compiler_engine[] = L"DXMT";
    D3D12ApplicationDescCompat compiler_application = {};
    compiler_application.pExeFilename = compiler_exe;
    compiler_application.pName = compiler_name;
    compiler_application.Version.Version = 0x0001000200030004ull;
    compiler_application.pEngineName = compiler_engine;
    compiler_application.EngineVersion.Version = 0x0005000600070008ull;
    CompilerTargetCompat compiler_target = {0, 0};
    CompilerVersionNumberCompat compiler_profile_version = {};
    HRESULT compiler_profile_hr =
        compiler_factory
            ? compiler_factory->GetApplicationProfileVersion(
                  &compiler_target, &compiler_application,
                  &compiler_profile_version)
            : E_NOINTERFACE;
    const wchar_t* compiler_cache_path =
        L"Z:\\tmp\\metalsharp-compiler-session.psdb";
    DeleteFileW(compiler_cache_path);
    CompilerDatabasePathCompat compiler_database_path = {};
    compiler_database_path.Types = static_cast<CompilerValueTypeFlagsCompat>(
        CompilerValueTypeFlagsObjectCode | CompilerValueTypeFlagsMetadata);
    compiler_database_path.pPath = compiler_cache_path;
    ID3D12CompilerCacheSessionProbe* compiler_session = nullptr;
    HRESULT compiler_session_hr =
        compiler_factory
            ? compiler_factory->CreateCompilerCacheSession(
                  &compiler_database_path, 1, &compiler_target,
                  &compiler_application, IID_ID3D12CompilerCacheSessionProbe,
                  reinterpret_cast<void**>(&compiler_session))
            : E_NOINTERFACE;
    CompilerTargetCompat compiler_session_target = {};
    CompilerTargetCompat* compiler_session_target_ptr =
        compiler_session ? compiler_session->GetCompilerTarget(
                               &compiler_session_target)
                         : nullptr;
    const D3D12ApplicationDescCompat* compiler_session_application =
        compiler_session ? compiler_session->GetApplicationDesc() : nullptr;
    CompilerValueTypeFlagsCompat compiler_session_value_types =
        compiler_session ? compiler_session->GetValueTypes()
                         : CompilerValueTypeFlagsNone;
    std::array<uint8_t, 5> compiler_group_bytes = {
        'g', 'r', 'o', 'u', 'p'};
    std::array<uint8_t, 5> compiler_value_key_bytes = {
        'v', 'a', 'l', 'u', 'e'};
    std::array<uint8_t, 4> compiler_object_bytes = {
        0x10, 0x20, 0x30, 0x40};
    std::array<uint8_t, 4> compiler_metadata_bytes = {
        0x50, 0x60, 0x70, 0x80};
    CompilerCacheGroupKeyCompat compiler_group_key = {
        compiler_group_bytes.data(), static_cast<UINT>(compiler_group_bytes.size())};
    CompilerCacheValueKeyCompat compiler_value_key = {
        compiler_value_key_bytes.data(),
        static_cast<UINT>(compiler_value_key_bytes.size())};
    CompilerCacheTypedConstValueCompat compiler_typed_values[2] = {};
    compiler_typed_values[0].Type = CompilerValueTypeObjectCode;
    compiler_typed_values[0].Value.pValue = compiler_object_bytes.data();
    compiler_typed_values[0].Value.ValueSize =
        static_cast<UINT>(compiler_object_bytes.size());
    compiler_typed_values[1].Type = CompilerValueTypeMetadata;
    compiler_typed_values[1].Value.pValue = compiler_metadata_bytes.data();
    compiler_typed_values[1].Value.ValueSize =
        static_cast<UINT>(compiler_metadata_bytes.size());
    HRESULT compiler_store_value_hr =
        compiler_session ? compiler_session->StoreValue(
                               &compiler_value_key, compiler_typed_values, 2)
                         : E_NOINTERFACE;
    HRESULT compiler_store_group_hr =
        compiler_session ? compiler_session->StoreGroupValueKeys(
                               &compiler_group_key, 23, &compiler_value_key, 1)
                         : E_NOINTERFACE;
    UINT compiler_found_group_version = 0;
    HRESULT compiler_find_group_hr =
        compiler_session ? compiler_session->FindGroup(
                               &compiler_group_key, &compiler_found_group_version)
                         : E_NOINTERFACE;
    CompilerCacheCallbackState compiler_group_keys = {};
    HRESULT compiler_find_group_keys_hr =
        compiler_session ? compiler_session->FindGroupValueKeys(
                               &compiler_group_key, &compiler_found_group_version,
                               compiler_group_value_keys_callback,
                               &compiler_group_keys)
                         : E_NOINTERFACE;
    CompilerCacheCallbackState compiler_group_values = {};
    HRESULT compiler_find_group_values_hr =
        compiler_session ? compiler_session->FindGroupValues(
                               &compiler_group_key, &compiler_found_group_version,
                               static_cast<CompilerValueTypeFlagsCompat>(
                                   CompilerValueTypeFlagsObjectCode |
                                   CompilerValueTypeFlagsMetadata),
                               compiler_group_values_callback,
                               &compiler_group_values)
                         : E_NOINTERFACE;
    std::array<uint8_t, 4> compiler_object_readback = {};
    std::array<uint8_t, 4> compiler_metadata_readback = {};
    CompilerCacheTypedValueCompat compiler_typed_readback[2] = {};
    compiler_typed_readback[0].Type = CompilerValueTypeObjectCode;
    compiler_typed_readback[0].Value.pValue = compiler_object_readback.data();
    compiler_typed_readback[0].Value.ValueSize =
        static_cast<UINT>(compiler_object_readback.size());
    compiler_typed_readback[1].Type = CompilerValueTypeMetadata;
    compiler_typed_readback[1].Value.pValue = compiler_metadata_readback.data();
    compiler_typed_readback[1].Value.ValueSize =
        static_cast<UINT>(compiler_metadata_readback.size());
    HRESULT compiler_find_value_hr =
        compiler_session ? compiler_session->FindValue(
                               &compiler_value_key, compiler_typed_readback, 2,
                               nullptr, nullptr)
                         : E_NOINTERFACE;
    ID3D12CompilerProbe* compiler = nullptr;
    HRESULT compiler_create_hr =
        compiler_factory
            ? compiler_factory->CreateCompiler(
                  compiler_session, IID_ID3D12CompilerProbe,
                  reinterpret_cast<void**>(&compiler))
            : E_NOINTERFACE;
    void* compiler_factory_from_compiler = nullptr;
    HRESULT compiler_get_factory_hr =
        compiler ? compiler->GetFactory(IID_ID3D12CompilerFactoryProbe,
                                        &compiler_factory_from_compiler)
                 : E_NOINTERFACE;
    void* compiler_session_from_compiler = nullptr;
    HRESULT compiler_get_session_hr =
        compiler ? compiler->GetCacheSession(IID_ID3D12CompilerCacheSessionProbe,
                                             &compiler_session_from_compiler)
                 : E_NOINTERFACE;
    bool compiler_application_ok =
        compiler_session_application && compiler_session_application->pExeFilename &&
        std::wstring(compiler_session_application->pExeFilename) ==
            L"compiler-probe.exe" &&
        compiler_session_application->pName &&
        std::wstring(compiler_session_application->pName) ==
            L"MetalSharp Compiler Probe";
    ID3D12CompilerCacheSessionProbe* compiler_reopened_session = nullptr;
    HRESULT compiler_reopen_session_hr =
        compiler_factory
            ? compiler_factory->CreateCompilerCacheSession(
                  &compiler_database_path, 1, &compiler_target,
                  &compiler_application, IID_ID3D12CompilerCacheSessionProbe,
                  reinterpret_cast<void**>(&compiler_reopened_session))
            : E_NOINTERFACE;
    UINT compiler_reopened_group_version = 0;
    HRESULT compiler_reopened_find_group_hr =
        compiler_reopened_session
            ? compiler_reopened_session->FindGroup(
                  &compiler_group_key, &compiler_reopened_group_version)
            : E_NOINTERFACE;
    std::array<uint8_t, 4> compiler_reopened_object_readback = {};
    CompilerCacheTypedValueCompat compiler_reopened_typed_value = {};
    compiler_reopened_typed_value.Type = CompilerValueTypeObjectCode;
    compiler_reopened_typed_value.Value.pValue =
        compiler_reopened_object_readback.data();
    compiler_reopened_typed_value.Value.ValueSize =
        static_cast<UINT>(compiler_reopened_object_readback.size());
    HRESULT compiler_reopened_find_value_hr =
        compiler_reopened_session
            ? compiler_reopened_session->FindValue(
                  &compiler_value_key, &compiler_reopened_typed_value, 1,
                  nullptr, nullptr)
            : E_NOINTERFACE;
    bool compiler_persistence_ok =
        SUCCEEDED(compiler_reopen_session_hr) &&
        compiler_reopened_find_group_hr == S_OK &&
        compiler_reopened_group_version == 23 &&
        compiler_reopened_find_value_hr == S_OK &&
        compiler_reopened_object_readback == compiler_object_bytes;
    bool compiler_provider_fail_closed =
        compiler_create_hr == E_NOTIMPL && compiler == nullptr;
    bool compiler_cache_api_ok =
        SUCCEEDED(compiler_factory_hr) && compiler_family_hr == S_OK &&
        compiler_family_end_hr == DXGI_ERROR_NOT_FOUND &&
        std::wstring(compiler_family.szAdapterFamily) == L"Apple M4" &&
        compiler_abi_size_hr == S_OK && compiler_abi_count == 1 &&
        compiler_abi_hr == S_OK && compiler_abi_capacity == 1 &&
        compiler_abi_version == 1 && compiler_version_hr == S_OK &&
        compiler_version.Version == 1 && compiler_profile_hr == S_OK &&
        compiler_profile_version.Version == 1 &&
        SUCCEEDED(compiler_session_hr) && compiler_session_target_ptr &&
        compiler_session_target.AdapterFamilyIndex == 0 &&
        compiler_session_target.ABIVersion == 1 && compiler_session_value_types ==
            static_cast<CompilerValueTypeFlagsCompat>(
                CompilerValueTypeFlagsObjectCode |
                CompilerValueTypeFlagsMetadata) &&
        compiler_application_ok && compiler_store_value_hr == S_OK &&
        compiler_store_group_hr == S_OK && compiler_find_group_hr == S_OK &&
        compiler_found_group_version == 23 && compiler_find_group_keys_hr == S_OK &&
        compiler_group_keys.keys.size() == 1 &&
        compiler_group_keys.keys[0].size() == compiler_value_key_bytes.size() &&
        std::equal(compiler_group_keys.keys[0].begin(),
                   compiler_group_keys.keys[0].end(),
                   compiler_value_key_bytes.begin()) &&
        compiler_find_group_values_hr == S_OK &&
        compiler_group_values.values.size() == 2 && compiler_find_value_hr == S_OK &&
        compiler_typed_readback[0].Value.ValueSize == compiler_object_bytes.size() &&
        compiler_typed_readback[1].Value.ValueSize ==
            compiler_metadata_bytes.size() &&
        compiler_object_readback == compiler_object_bytes &&
        compiler_metadata_readback == compiler_metadata_bytes &&
        compiler_persistence_ok && compiler_provider_fail_closed;
    if (compiler_factory_from_compiler)
        reinterpret_cast<IUnknown*>(compiler_factory_from_compiler)->Release();
    if (compiler_session_from_compiler)
        reinterpret_cast<IUnknown*>(compiler_session_from_compiler)->Release();
    if (compiler)
        compiler->Release();
    if (compiler_reopened_session)
        compiler_reopened_session->Release();
    if (compiler_session)
        compiler_session->Release();
    DeleteFileW(compiler_cache_path);
    if (compiler_factory)
        compiler_factory->Release();

    struct PipelineStreamProbe {
        UINT type;
        ID3D12RootSignature* root_signature;
    } pipeline_stream = {0, nullptr};
    D3D12_PIPELINE_STATE_STREAM_DESC pipeline_desc = {};
    pipeline_desc.SizeInBytes = sizeof(pipeline_stream);
    pipeline_desc.pPipelineStateSubobjectStream = &pipeline_stream;
    std::array<uint8_t, 4> pso_key = {0x70, 0x73, 0x6f, 0x31};
    HRESULT store_pipeline_desc_hr =
        database
            ? database->StorePipelineStateDesc(pso_key.data(), static_cast<UINT>(pso_key.size()), 7, &pipeline_desc)
            : E_NOINTERFACE;
    PipelineCallbackState pipeline_callback = {};
    HRESULT find_pipeline_desc_hr =
        database ? database->FindPipelineStateDesc(pso_key.data(), static_cast<UINT>(pso_key.size()),
                                                   pipeline_state_callback, &pipeline_callback)
                 : E_NOINTERFACE;
    UINT found_pipeline_version = 0;
    HRESULT find_pipeline_version_hr =
        database
            ? database->FindObjectVersion(pso_key.data(), static_cast<UINT>(pso_key.size()), &found_pipeline_version)
            : E_NOINTERFACE;
    D3D12_STATE_OBJECT_CONFIG state_config = {};
    state_config.Flags = D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS;
    D3D12_NODE_MASK state_node_mask = {3};
    D3D12_RAYTRACING_SHADER_CONFIG state_shader_config = {32, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG state_pipeline_config = {2};
    D3D12_RAYTRACING_PIPELINE_CONFIG1 state_pipeline_config1 = {};
    state_pipeline_config1.MaxTraceRecursionDepth = 3;
    state_pipeline_config1.Flags = D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES;
    std::array<uint8_t, 8> state_dxil_bytes =
        {0x44, 0x58, 0x49, 0x4c, 0x01, 0x00, 0x00, 0x00};
    D3D12_EXPORT_DESC state_export = {};
    state_export.Name = L"RayGen";
    state_export.ExportToRename = L"RayGenRenamed";
    state_export.Flags = D3D12_EXPORT_FLAG_NONE;
    D3D12_DXIL_LIBRARY_DESC state_library = {};
    state_library.DXILLibrary.pShaderBytecode = state_dxil_bytes.data();
    state_library.DXILLibrary.BytecodeLength = state_dxil_bytes.size();
    state_library.NumExports = 1;
    state_library.pExports = &state_export;
    std::array<uint8_t, 4> state_key = {0x73, 0x6f, 0x31, 0x00};
    std::array<uint8_t, 4> state_parent_key = {0x70, 0x61, 0x72, 0x00};
    const auto expected_state_parent_key = state_parent_key;
    const auto expected_root_signature_first_byte =
        static_cast<const uint8_t*>(root_blob->GetBufferPointer())[0];
    GlobalSerializedRootSignatureCompat global_serialized_root = {};
    global_serialized_root.Desc.pSerializedBlob =
        root_blob ? root_blob->GetBufferPointer() : nullptr;
    global_serialized_root.Desc.SerializedBlobSizeInBytes =
        root_blob ? root_blob->GetBufferSize() : 0;
    LocalSerializedRootSignatureCompat local_serialized_root = {};
    local_serialized_root.Desc.pSerializedBlob =
        root_blob ? root_blob->GetBufferPointer() : nullptr;
    local_serialized_root.Desc.SerializedBlobSizeInBytes =
        root_blob ? root_blob->GetBufferSize() : 0;
    wchar_t by_key_export_name[] = L"ByKeyExport";
    wchar_t by_key_export_rename[] = L"ByKeyRenamed";
    D3D12_EXPORT_DESC by_key_export = {};
    by_key_export.Name = by_key_export_name;
    by_key_export.ExportToRename = by_key_export_rename;
    by_key_export.Flags = D3D12_EXPORT_FLAG_NONE;
    ExistingCollectionByKeyDescCompat existing_collection_by_key = {};
    existing_collection_by_key.pKey = state_parent_key.data();
    existing_collection_by_key.KeySize =
        static_cast<UINT>(state_parent_key.size());
    existing_collection_by_key.NumExports = 1;
    existing_collection_by_key.pExports = &by_key_export;
    D3D12_STATE_SUBOBJECT state_subobjects[12] = {};
    state_subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG;
    state_subobjects[0].pDesc = &state_config;
    state_subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK;
    state_subobjects[1].pDesc = &state_node_mask;
    state_subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    state_subobjects[2].pDesc = &state_shader_config;
    state_subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    state_subobjects[3].pDesc = &state_pipeline_config;
    state_subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
    state_subobjects[4].pDesc = &state_pipeline_config1;
    state_subobjects[5].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    state_subobjects[5].pDesc = &state_library;
    wchar_t hit_group_export[] = L"HitGroup";
    wchar_t hit_group_any_hit[] = L"AnyHit";
    wchar_t hit_group_closest_hit[] = L"ClosestHit";
    wchar_t hit_group_intersection[] = L"Intersection";
    D3D12_HIT_GROUP_DESC hit_group = {};
    hit_group.HitGroupExport = hit_group_export;
    hit_group.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hit_group.AnyHitShaderImport = hit_group_any_hit;
    hit_group.ClosestHitShaderImport = hit_group_closest_hit;
    hit_group.IntersectionShaderImport = hit_group_intersection;
    state_subobjects[6].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    state_subobjects[6].pDesc = &hit_group;
    wchar_t association_target[] = L"HitGroup";
    wchar_t association_export_name[] = L"RayGen";
    const wchar_t* association_exports[] = {association_export_name};
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
    association.SubobjectToAssociate = association_target;
    association.NumExports = 1;
    association.pExports = association_exports;
    state_subobjects[7].Type =
        D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    state_subobjects[7].pDesc = &association;
    wchar_t subobject_association_export_name[] = L"RayGen";
    const wchar_t* subobject_association_exports[] = {
        subobject_association_export_name};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION subobject_association = {};
    subobject_association.pSubobjectToAssociate = &state_subobjects[6];
    subobject_association.NumExports = 1;
    subobject_association.pExports = subobject_association_exports;
    state_subobjects[8].Type =
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    state_subobjects[8].pDesc = &subobject_association;
    state_subobjects[9].Type = static_cast<D3D12_STATE_SUBOBJECT_TYPE>(
        kGlobalSerializedRootSignatureType);
    state_subobjects[9].pDesc = &global_serialized_root;
    state_subobjects[10].Type = static_cast<D3D12_STATE_SUBOBJECT_TYPE>(
        kLocalSerializedRootSignatureType);
    state_subobjects[10].pDesc = &local_serialized_root;
    state_subobjects[11].Type = static_cast<D3D12_STATE_SUBOBJECT_TYPE>(
        kExistingCollectionByKeyType);
    state_subobjects[11].pDesc = &existing_collection_by_key;
    D3D12_STATE_OBJECT_DESC state_desc = {};
    state_desc.Type = D3D12_STATE_OBJECT_TYPE_COLLECTION;
    state_desc.NumSubobjects = 12;
    state_desc.pSubobjects = state_subobjects;
    HRESULT store_state_object_hr =
        database ? database->StoreStateObjectDesc(
                       state_key.data(), static_cast<UINT>(state_key.size()), 11,
                       &state_desc, state_parent_key.data(),
                       static_cast<UINT>(state_parent_key.size()))
                 : E_NOINTERFACE;
    state_dxil_bytes[0] = 0;
    static_cast<uint8_t*>(root_blob->GetBufferPointer())[0] = 0xff;
    state_parent_key[0] = 0xee;
    by_key_export_name[0] = L'X';
    by_key_export_rename[0] = L'Y';
    hit_group_export[0] = L'X';
    hit_group_any_hit[0] = L'Y';
    hit_group_closest_hit[0] = L'Z';
    hit_group_intersection[0] = L'W';
    association_target[0] = L'X';
    association_export_name[0] = L'Y';
    subobject_association_export_name[0] = L'Z';
    StateObjectCallbackState state_callback = {};
    HRESULT find_state_object_hr =
        database ? database->FindStateObjectDesc(
                       state_key.data(), static_cast<UINT>(state_key.size()),
                       state_object_callback, &state_callback)
                 : E_NOINTERFACE;
    UINT found_state_version = 0;
    HRESULT find_state_version_hr =
        database ? database->FindObjectVersion(
                       state_key.data(), static_cast<UINT>(state_key.size()),
                       &found_state_version)
                 : E_NOINTERFACE;
    D3D12_NODE_MASK unsupported_payload = {};
    D3D12_STATE_SUBOBJECT unsupported_subobject = {};
    unsupported_subobject.Type = static_cast<D3D12_STATE_SUBOBJECT_TYPE>(0x7f);
    unsupported_subobject.pDesc = &unsupported_payload;
    D3D12_STATE_OBJECT_DESC unsupported_state_desc = {};
    unsupported_state_desc.Type = D3D12_STATE_OBJECT_TYPE_COLLECTION;
    unsupported_state_desc.NumSubobjects = 1;
    unsupported_state_desc.pSubobjects = &unsupported_subobject;
    HRESULT store_unsupported_state_object_hr =
        database ? database->StoreStateObjectDesc(
                       "bad", 3, 1, &unsupported_state_desc, nullptr, 0)
                 : E_NOINTERFACE;

    ID3D12StateObjectDatabaseCompat* reopened_database = nullptr;
    HRESULT reopen_database_hr =
        database_factory ? database_factory->CreateStateObjectDatabaseFromFile(
                               database_path, 0,
                               IID_ID3D12StateObjectDatabaseProbe,
                               reinterpret_cast<void**>(&reopened_database))
                         : E_NOINTERFACE;
    ApplicationCallbackState reopened_application_callback = {};
    HRESULT reopened_application_hr =
        reopened_database
            ? reopened_database->GetApplicationDesc(
                  reinterpret_cast<void*>(application_desc_callback),
                  &reopened_application_callback)
            : E_NOINTERFACE;
    PipelineCallbackState reopened_pipeline_callback = {};
    HRESULT reopened_pipeline_hr =
        reopened_database
            ? reopened_database->FindPipelineStateDesc(
                  pso_key.data(), static_cast<UINT>(pso_key.size()),
                  pipeline_state_callback, &reopened_pipeline_callback)
            : E_NOINTERFACE;
    StateObjectCallbackState reopened_state_callback = {};
    HRESULT reopened_state_hr =
        reopened_database
            ? reopened_database->FindStateObjectDesc(
                  state_key.data(), static_cast<UINT>(state_key.size()),
                  state_object_callback, &reopened_state_callback)
            : E_NOINTERFACE;
    ID3D12StateObjectDatabaseCompat* readonly_database = nullptr;
    HRESULT readonly_database_hr =
        database_factory ? database_factory->CreateStateObjectDatabaseFromFile(
                               database_path, 1,
                               IID_ID3D12StateObjectDatabaseProbe,
                               reinterpret_cast<void**>(&readonly_database))
                         : E_NOINTERFACE;
    HRESULT readonly_store_hr =
        readonly_database
            ? readonly_database->StorePipelineStateDesc(
                  pso_key.data(), static_cast<UINT>(pso_key.size()), 8,
                  &pipeline_desc)
            : E_NOINTERFACE;

    const wchar_t* malformed_database_path =
        L"Z:\\tmp\\metalsharp-agility-cache-malformed.bin";
    DeleteFileW(malformed_database_path);
    HANDLE malformed_file = CreateFileW(
        malformed_database_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const uint8_t malformed_bytes[] = {0x53, 0x44};
    DWORD malformed_written = 0;
    const BOOL malformed_write_ok =
        malformed_file != INVALID_HANDLE_VALUE &&
        WriteFile(malformed_file, malformed_bytes, sizeof(malformed_bytes),
                  &malformed_written, nullptr) &&
        malformed_written == sizeof(malformed_bytes);
    if (malformed_file != INVALID_HANDLE_VALUE)
        CloseHandle(malformed_file);
    ID3D12StateObjectDatabaseCompat* malformed_database = nullptr;
    HRESULT malformed_database_hr =
        database_factory
            ? database_factory->CreateStateObjectDatabaseFromFile(
                  malformed_database_path, 0,
                  IID_ID3D12StateObjectDatabaseProbe,
                  reinterpret_cast<void**>(&malformed_database))
            : E_NOINTERFACE;
    const BOOL malformed_file_removed = DeleteFileW(malformed_database_path);

    bool d3d12_expected_path = expected_windows.empty() || contains_ascii_ci(modules[4].path, expected_windows);
    bool payload_version_matches = (modules[0].loaded && modules[0].has_exported_sdk_version &&
                                    modules[0].exported_sdk_version == D3D12SDKVersion) ||
                                   (modules[1].loaded && modules[1].has_exported_sdk_version &&
                                    modules[1].exported_sdk_version == D3D12SDKVersion);
    bool device_configuration_ok = SUCCEEDED(get_sdk_config_hr) && SUCCEEDED(set_sdk_version_hr) &&
                                   SUCCEEDED(get_device_config_hr) && device_config_desc_ptr != nullptr &&
                                   device_config_desc.SDKVersion == D3D12SDKVersion &&
                                   SUCCEEDED(config_serialize_root_hr) && root_blob && root_blob->GetBufferSize() > 0 &&
                                   SUCCEEDED(config_deserialize_root_hr) && config_deserialized_desc &&
                                   config_deserialized_desc->NumParameters == 0;
    bool shader_cache_ok = SUCCEEDED(create_shader_cache_hr) && SUCCEEDED(shader_cache_store_hr) &&
                           SUCCEEDED(shader_cache_size_hr) && SUCCEEDED(shader_cache_find_hr) &&
                           shader_readback == shader_value;
    bool pipeline_desc_cache_ok =
        SUCCEEDED(get_database_factory_hr) && SUCCEEDED(create_database_hr) &&
        SUCCEEDED(set_application_desc_hr) && SUCCEEDED(get_application_desc_hr) &&
        application_callback.called && application_callback.exe == L"probe.exe" &&
        application_callback.name == L"MetalSharp State DB" &&
        application_callback.engine == L"DXMT" &&
        application_callback.version == 0x0001000200030004ull &&
        application_callback.engine_version == 0x0005000600070008ull &&
        SUCCEEDED(store_pipeline_desc_hr) &&
        SUCCEEDED(find_pipeline_desc_hr) && pipeline_callback.called && pipeline_callback.version == 7 &&
        pipeline_callback.size == sizeof(pipeline_stream) && SUCCEEDED(find_pipeline_version_hr) &&
        found_pipeline_version == 7 && SUCCEEDED(store_state_object_hr) &&
        SUCCEEDED(find_state_object_hr) && state_callback.called &&
        state_callback.version == 11 &&
        state_callback.type == D3D12_STATE_OBJECT_TYPE_COLLECTION &&
        state_callback.subobject_count == 12 &&
        state_callback.first_subobject_type ==
            D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG &&
        state_callback.config_flags == state_config.Flags &&
        state_callback.node_mask == state_node_mask.NodeMask &&
        state_callback.max_payload_size ==
            state_shader_config.MaxPayloadSizeInBytes &&
        state_callback.max_attribute_size ==
            state_shader_config.MaxAttributeSizeInBytes &&
        state_callback.max_recursion_depth ==
            state_pipeline_config1.MaxTraceRecursionDepth &&
        state_callback.pipeline_flags == static_cast<UINT>(state_pipeline_config1.Flags) &&
        state_callback.dxil_library_present &&
        state_callback.dxil_library_size == state_dxil_bytes.size() &&
        state_callback.dxil_first_byte == 0x44 &&
        state_callback.dxil_export_count == 1 &&
        state_callback.dxil_first_export == L"RayGen" &&
        state_callback.dxil_first_rename == L"RayGenRenamed" &&
        state_callback.dxil_first_export_flags == 0 &&
        state_callback.hit_group_present &&
        state_callback.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE &&
        state_callback.hit_group_export == L"HitGroup" &&
        state_callback.hit_group_any_hit == L"AnyHit" &&
        state_callback.hit_group_closest_hit == L"ClosestHit" &&
        state_callback.hit_group_intersection == L"Intersection" &&
        state_callback.dxil_association_present &&
        state_callback.dxil_association_target == L"HitGroup" &&
        state_callback.dxil_association_export_count == 1 &&
        state_callback.dxil_association_first_export == L"RayGen" &&
        state_callback.subobject_association_present &&
        state_callback.subobject_association_target_type ==
            D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP &&
        state_callback.subobject_association_export_count == 1 &&
        state_callback.subobject_association_first_export == L"RayGen" &&
        state_callback.serialized_root_signature_present &&
        state_callback.serialized_root_signature_type_mask == 3 &&
        state_callback.serialized_root_signature_size ==
            root_blob->GetBufferSize() &&
        state_callback.serialized_root_signature_first_byte ==
            expected_root_signature_first_byte &&
        state_callback.existing_collection_by_key_present &&
        state_callback.existing_collection_by_key_size ==
            expected_state_parent_key.size() &&
        state_callback.existing_collection_by_key_first_byte ==
            expected_state_parent_key[0] &&
        state_callback.existing_collection_by_key_export_count == 1 &&
        state_callback.existing_collection_by_key_first_export ==
            L"ByKeyExport" &&
        state_callback.existing_collection_by_key_first_rename ==
            L"ByKeyRenamed" &&
        state_callback.parent_key_size == expected_state_parent_key.size() &&
        state_callback.parent_key == expected_state_parent_key &&
        SUCCEEDED(find_state_version_hr) && found_state_version == 11 &&
        store_unsupported_state_object_hr == E_NOTIMPL &&
        SUCCEEDED(reopen_database_hr) && SUCCEEDED(reopened_application_hr) &&
        reopened_application_callback.called &&
        reopened_application_callback.exe == L"probe.exe" &&
        reopened_application_callback.name == L"MetalSharp State DB" &&
        reopened_application_callback.engine == L"DXMT" &&
        reopened_application_callback.version == 0x0001000200030004ull &&
        reopened_application_callback.engine_version == 0x0005000600070008ull &&
        SUCCEEDED(reopened_pipeline_hr) && reopened_pipeline_callback.called &&
        reopened_pipeline_callback.version == 7 &&
        reopened_pipeline_callback.size == sizeof(pipeline_stream) &&
        SUCCEEDED(reopened_state_hr) && reopened_state_callback.called &&
        reopened_state_callback.version == 11 &&
        reopened_state_callback.subobject_count == 12 &&
        reopened_state_callback.config_flags == state_config.Flags &&
        reopened_state_callback.node_mask == state_node_mask.NodeMask &&
        reopened_state_callback.max_payload_size ==
            state_shader_config.MaxPayloadSizeInBytes &&
        reopened_state_callback.max_attribute_size ==
            state_shader_config.MaxAttributeSizeInBytes &&
        reopened_state_callback.max_recursion_depth ==
            state_pipeline_config1.MaxTraceRecursionDepth &&
        reopened_state_callback.pipeline_flags == static_cast<UINT>(state_pipeline_config1.Flags) &&
        reopened_state_callback.dxil_library_present &&
        reopened_state_callback.dxil_library_size == state_dxil_bytes.size() &&
        reopened_state_callback.dxil_first_byte == 0x44 &&
        reopened_state_callback.dxil_export_count == 1 &&
        reopened_state_callback.dxil_first_export == L"RayGen" &&
        reopened_state_callback.dxil_first_rename == L"RayGenRenamed" &&
        reopened_state_callback.dxil_first_export_flags == 0 &&
        reopened_state_callback.hit_group_present &&
        reopened_state_callback.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE &&
        reopened_state_callback.hit_group_export == L"HitGroup" &&
        reopened_state_callback.hit_group_any_hit == L"AnyHit" &&
        reopened_state_callback.hit_group_closest_hit == L"ClosestHit" &&
        reopened_state_callback.hit_group_intersection == L"Intersection" &&
        reopened_state_callback.dxil_association_present &&
        reopened_state_callback.dxil_association_target == L"HitGroup" &&
        reopened_state_callback.dxil_association_export_count == 1 &&
        reopened_state_callback.dxil_association_first_export == L"RayGen" &&
        reopened_state_callback.subobject_association_present &&
        reopened_state_callback.subobject_association_target_type ==
            D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP &&
        reopened_state_callback.subobject_association_export_count == 1 &&
        reopened_state_callback.subobject_association_first_export == L"RayGen" &&
        reopened_state_callback.serialized_root_signature_present &&
        reopened_state_callback.serialized_root_signature_type_mask == 3 &&
        reopened_state_callback.serialized_root_signature_size ==
            root_blob->GetBufferSize() &&
        reopened_state_callback.serialized_root_signature_first_byte ==
            expected_root_signature_first_byte &&
        reopened_state_callback.existing_collection_by_key_present &&
        reopened_state_callback.existing_collection_by_key_size ==
            expected_state_parent_key.size() &&
        reopened_state_callback.existing_collection_by_key_first_byte ==
            expected_state_parent_key[0] &&
        reopened_state_callback.existing_collection_by_key_export_count == 1 &&
        reopened_state_callback.existing_collection_by_key_first_export ==
            L"ByKeyExport" &&
        reopened_state_callback.existing_collection_by_key_first_rename ==
            L"ByKeyRenamed" &&
        reopened_state_callback.parent_key == expected_state_parent_key &&
        SUCCEEDED(readonly_database_hr) && readonly_store_hr == E_ACCESSDENIED &&
        malformed_write_ok && malformed_database_hr ==
            HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) && malformed_database == nullptr;
    const BOOL database_file_removed = DeleteFileW(database_path);
    bool pass = modules[0].loaded && modules[1].loaded && modules[4].loaded && modules[4].has_required_symbol &&
                payload_version_matches && d3d12_expected_path && SUCCEEDED(create_hr) && device != nullptr &&
                interfaces[0].supported && device_configuration_ok && shader_cache_ok && pipeline_desc_cache_ok &&
                compiler_cache_api_ok;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-agility-ue5.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"sdk\": {\n");
    std::printf("    \"D3D12SDKVersion\": %u,\n", D3D12SDKVersion);
    std::printf("    \"D3D12SDKPath\": \"%s\"\n", json_escape(D3D12SDKPath).c_str());
    std::printf("  },\n");
    std::printf("  \"agility_match\": {\n");
    std::printf("    \"d3d12core_loaded\": %s,\n", modules[0].loaded ? "true" : "false");
    std::printf("    \"d3d12core_exported_sdk_version\": %u,\n", modules[0].exported_sdk_version);
    std::printf("    \"sdk_layers_exported_sdk_version\": %u,\n", modules[1].exported_sdk_version);
    std::printf("    \"payload_version_matches_probe\": %s\n", payload_version_matches ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"device_create\": {\n");
    std::printf("    \"minimum_feature_level\": \"11_0\",\n");
    std::printf("    \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(create_hr)));
    std::printf("    \"succeeded\": %s\n", SUCCEEDED(create_hr) ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"routing\": {\n");
    std::printf("    \"expected_d3d12_path_substring\": \"%s\",\n", json_escape(expected_windows).c_str());
    std::printf("    \"d3d12_expected_path_match\": %s\n", d3d12_expected_path ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"device_configuration\": {\n");
    print_hr_field("get_sdk_configuration", get_sdk_config_hr);
    print_hr_field("set_sdk_version", set_sdk_version_hr);
    print_hr_field("get_device_configuration", get_device_config_hr);
    std::printf("    \"desc_sdk_version\": %u,\n", device_config_desc.SDKVersion);
    print_hr_field("serialize_versioned_root_signature", config_serialize_root_hr);
    print_hr_field("create_versioned_root_signature_deserializer", config_deserialize_root_hr);
    std::printf("    \"deserialized_parameter_count\": %u,\n",
                config_deserialized_desc ? config_deserialized_desc->NumParameters : UINT_MAX);
    std::printf("    \"verified\": %s\n", device_configuration_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"compiler_cache\": {\n");
    print_hr_field("create_shader_cache_session", create_shader_cache_hr);
    print_hr_field("shader_cache_store", shader_cache_store_hr);
    print_hr_field("shader_cache_size_query", shader_cache_size_hr);
    print_hr_field("shader_cache_find", shader_cache_find_hr);
    print_hr_field("get_state_object_database_factory", get_database_factory_hr);
    print_hr_field("create_state_object_database", create_database_hr);
    print_hr_field("set_application_desc", set_application_desc_hr);
    print_hr_field("get_application_desc", get_application_desc_hr);
    print_hr_field("store_pipeline_state_desc", store_pipeline_desc_hr);
    print_hr_field("find_pipeline_state_desc", find_pipeline_desc_hr);
    print_hr_field("find_pipeline_version", find_pipeline_version_hr);
    print_hr_field("store_state_object_desc", store_state_object_hr);
    print_hr_field("find_state_object_desc", find_state_object_hr);
    print_hr_field("compiler_factory", compiler_factory_hr);
    print_hr_field("compiler_family", compiler_family_hr);
    print_hr_field("compiler_family_end", compiler_family_end_hr);
    print_hr_field("compiler_abi_size", compiler_abi_size_hr);
    print_hr_field("compiler_abi", compiler_abi_hr);
    print_hr_field("compiler_version", compiler_version_hr);
    print_hr_field("compiler_profile", compiler_profile_hr);
    print_hr_field("compiler_session", compiler_session_hr);
    print_hr_field("compiler_store_value", compiler_store_value_hr);
    print_hr_field("compiler_store_group", compiler_store_group_hr);
    print_hr_field("compiler_find_group", compiler_find_group_hr);
    print_hr_field("compiler_find_group_keys", compiler_find_group_keys_hr);
    print_hr_field("compiler_find_group_values", compiler_find_group_values_hr);
    print_hr_field("compiler_find_value", compiler_find_value_hr);
    print_hr_field("compiler_create", compiler_create_hr);
    print_hr_field("compiler_get_factory", compiler_get_factory_hr);
    print_hr_field("compiler_get_session", compiler_get_session_hr);
    print_hr_field("compiler_reopen_session", compiler_reopen_session_hr);
    print_hr_field("compiler_reopened_find_group", compiler_reopened_find_group_hr);
    print_hr_field("compiler_reopened_find_value", compiler_reopened_find_value_hr);
    print_hr_field("find_state_object_version", find_state_version_hr);
    print_hr_field("store_unsupported_state_object_desc", store_unsupported_state_object_hr);
    print_hr_field("reopen_database", reopen_database_hr);
    print_hr_field("reopened_application_desc", reopened_application_hr);
    print_hr_field("reopened_pipeline_desc", reopened_pipeline_hr);
    print_hr_field("reopened_state_object_desc", reopened_state_hr);
    print_hr_field("readonly_database", readonly_database_hr);
    print_hr_field("readonly_store", readonly_store_hr);
    print_hr_field("malformed_database", malformed_database_hr);
    std::printf("    \"malformed_file_rejected\": %s,\n",
                (malformed_write_ok && malformed_database_hr ==
                     HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) && malformed_database == nullptr) ? "true" : "false");
    std::printf("    \"malformed_file_removed\": %s,\n",
                malformed_file_removed ? "true" : "false");
    std::printf("    \"application_callback_called\": %s,\n",
                application_callback.called ? "true" : "false");
    std::printf("    \"application_callback_name_deep_copy_verified\": %s,\n",
                application_callback.name == L"MetalSharp State DB" ? "true" : "false");
    std::printf("    \"application_callback_engine_deep_copy_verified\": %s,\n",
                application_callback.engine == L"DXMT" ? "true" : "false");
    std::printf("    \"pipeline_callback_called\": %s,\n", pipeline_callback.called ? "true" : "false");
    std::printf("    \"pipeline_callback_version\": %u,\n", pipeline_callback.version);
    std::printf("    \"pipeline_callback_size\": %llu,\n", static_cast<unsigned long long>(pipeline_callback.size));
    std::printf("    \"state_object_callback_called\": %s,\n", state_callback.called ? "true" : "false");
    std::printf("    \"state_object_callback_version\": %u,\n", state_callback.version);
    std::printf("    \"state_object_callback_subobject_count\": %u,\n", state_callback.subobject_count);
    std::printf("    \"state_object_callback_first_subobject_type\": %u,\n",
                static_cast<UINT>(state_callback.first_subobject_type));
    std::printf("    \"state_object_callback_config_flags\": %u,\n",
                static_cast<UINT>(state_callback.config_flags));
    std::printf("    \"state_object_callback_node_mask\": %u,\n",
                state_callback.node_mask);
    std::printf("    \"state_object_callback_max_payload_size\": %u,\n",
                state_callback.max_payload_size);
    std::printf("    \"state_object_callback_max_attribute_size\": %u,\n",
                state_callback.max_attribute_size);
    std::printf("    \"state_object_callback_max_recursion_depth\": %u,\n",
                state_callback.max_recursion_depth);
    std::printf("    \"state_object_callback_pipeline_flags\": %u,\n",
                state_callback.pipeline_flags);
    std::printf("    \"state_object_callback_dxil_library_size\": %u,\n",
                state_callback.dxil_library_size);
    std::printf("    \"state_object_callback_dxil_first_byte\": %u,\n",
                state_callback.dxil_first_byte);
    std::printf("    \"state_object_callback_dxil_export_count\": %u,\n",
                state_callback.dxil_export_count);
    std::printf("    \"state_object_callback_dxil_first_export_flags\": %u,\n",
                state_callback.dxil_first_export_flags);
    std::printf("    \"state_object_callback_hit_group_type\": %u,\n",
                state_callback.hit_group_type);
    std::printf("    \"state_object_callback_dxil_association_export_count\": %u,\n",
                state_callback.dxil_association_export_count);
    std::printf("    \"state_object_callback_subobject_association_target_type\": %u,\n",
                state_callback.subobject_association_target_type);
    std::printf("    \"state_object_serialized_root_signature_type_mask\": %u,\n",
                state_callback.serialized_root_signature_type_mask);
    std::printf("    \"state_object_serialized_root_signature_size\": %llu,\n",
                static_cast<unsigned long long>(state_callback.serialized_root_signature_size));
    std::printf("    \"state_object_serialized_root_signature_first_byte\": %u,\n",
                state_callback.serialized_root_signature_first_byte);
    std::printf("    \"state_object_existing_collection_by_key_size\": %u,\n",
                state_callback.existing_collection_by_key_size);
    std::printf("    \"state_object_existing_collection_by_key_export_count\": %u,\n",
                state_callback.existing_collection_by_key_export_count);
    std::printf("    \"state_object_existing_collection_by_key_first_export\": \"%s\",\n",
                json_escape(std::string(state_callback.existing_collection_by_key_first_export.begin(),
                                        state_callback.existing_collection_by_key_first_export.end())).c_str());
    std::printf("    \"state_object_parent_key_size\": %u,\n", state_callback.parent_key_size);
    std::printf("    \"state_object_file_persistence_verified\": %s,\n",
                (SUCCEEDED(reopen_database_hr) && SUCCEEDED(reopened_application_hr) &&
                 SUCCEEDED(reopened_pipeline_hr) && SUCCEEDED(reopened_state_hr)) ? "true" : "false");
    std::printf("    \"readonly_store_rejected\": %s,\n",
                readonly_store_hr == E_ACCESSDENIED ? "true" : "false");
    std::printf("    \"database_file_removed\": %s,\n",
                database_file_removed ? "true" : "false");
    std::printf("    \"shader_cache_verified\": %s,\n", shader_cache_ok ? "true" : "false");
    std::printf("    \"pipeline_desc_cache_verified\": %s,\n", pipeline_desc_cache_ok ? "true" : "false");
    std::printf("    \"state_object_desc_cache_verified\": %s,\n",
                state_callback.called && found_state_version == 11 ? "true" : "false");
    std::printf("    \"compiler_cache_api_verified\": %s,\n",
                compiler_cache_api_ok ? "true" : "false");
    std::printf("    \"compiler_provider_fail_closed\": %s,\n",
                compiler_provider_fail_closed ? "true" : "false");
    std::printf("    \"compiler_family\": \"%s\",\n",
                json_escape(narrow_ascii(compiler_family.szAdapterFamily)).c_str());
    std::printf("    \"compiler_abi_version\": %llu,\n",
                static_cast<unsigned long long>(compiler_abi_version));
    std::printf("    \"compiler_group_value_key_count\": %zu,\n",
                compiler_group_keys.keys.size());
    std::printf("    \"compiler_group_value_count\": %zu,\n",
                compiler_group_values.values.size());
    std::printf("    \"compiler_object_readback_match\": %s,\n",
                compiler_object_readback == compiler_object_bytes ? "true" : "false");
    std::printf("    \"compiler_metadata_readback_match\": %s,\n",
                compiler_metadata_readback == compiler_metadata_bytes ? "true" : "false");
    std::printf("    \"compiler_persistence_verified\": %s,\n",
                compiler_persistence_ok ? "true" : "false");
    std::printf("    \"unsupported_state_object_rejected\": %s\n",
                store_unsupported_state_object_hr == E_NOTIMPL ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"modules\": {\n");
    for (size_t i = 0; i < modules.size(); ++i)
        print_module_json(modules[i], i + 1 == modules.size());
    std::printf("  },\n");
    std::printf("  \"device_interfaces\": {\n");
    for (size_t i = 0; i < interfaces.size(); ++i)
        print_interface_json(interfaces[i], i + 1 == interfaces.size());
    std::printf("  }\n");
    std::printf("}\n");

    // DXMT may keep Wine-hosted worker synchronization objects alive at process
    // teardown. This probe is diagnostic, so flush JSON and terminate hard
    // before COM/module cleanup can mask successful evidence with CRT asserts.
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
