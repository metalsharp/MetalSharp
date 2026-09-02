#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <d3d12.h>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

struct GPUUploadFeatureProbe {
    BOOL DynamicDepthBiasSupported = FALSE;
    BOOL GPUUploadHeapSupported = FALSE;
};

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

static std::string feature_level_name(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_12_2:
        return "12_2";
    case D3D_FEATURE_LEVEL_12_1:
        return "12_1";
    case D3D_FEATURE_LEVEL_12_0:
        return "12_0";
    case D3D_FEATURE_LEVEL_11_1:
        return "11_1";
    case D3D_FEATURE_LEVEL_11_0:
        return "11_0";
    default:
        return "unknown";
    }
}

static std::string shader_model_name(D3D_SHADER_MODEL model) {
    switch (model) {
    case D3D_SHADER_MODEL_6_7:
        return "6_7";
    case D3D_SHADER_MODEL_6_6:
        return "6_6";
    case D3D_SHADER_MODEL_6_5:
        return "6_5";
    case D3D_SHADER_MODEL_6_4:
        return "6_4";
    case D3D_SHADER_MODEL_6_3:
        return "6_3";
    case D3D_SHADER_MODEL_6_2:
        return "6_2";
    case D3D_SHADER_MODEL_6_1:
        return "6_1";
    case D3D_SHADER_MODEL_6_0:
        return "6_0";
    case D3D_SHADER_MODEL_5_1:
        return "5_1";
    default:
        return "unknown";
    }
}

static void print_hr(const char* key, HRESULT hr) {
    std::printf("    \"%s_hr\": \"0x%08lx\",\n", key, static_cast<unsigned long>(static_cast<uint32_t>(hr)));
}

int main() {
    configure_exported_sdk();
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    FARPROC create_device_proc = d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    auto create_device = reinterpret_cast<CreateDeviceFn>(reinterpret_cast<void*>(create_device_proc));

    ID3D12Device* device = nullptr;
    HRESULT create_hr = E_FAIL;
    if (create_device)
        create_hr =
            create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe, reinterpret_cast<void**>(&device));

    D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels = {
        static_cast<UINT>(sizeof(requested_levels) / sizeof(requested_levels[0])), requested_levels,
        D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model = {D3D_SHADER_MODEL_6_7};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14 = {};
    GPUUploadFeatureProbe options16 = {};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT stream_output_format = {};
    stream_output_format.Format = DXGI_FORMAT_R32_FLOAT;

    HRESULT fl_hr = E_FAIL;
    HRESULT sm_hr = E_FAIL;
    HRESULT options_hr = E_FAIL;
    HRESULT options1_hr = E_FAIL;
    HRESULT options2_hr = E_FAIL;
    HRESULT options3_hr = E_FAIL;
    HRESULT options4_hr = E_FAIL;
    HRESULT options5_hr = E_FAIL;
    HRESULT options7_hr = E_FAIL;
    HRESULT options9_hr = E_FAIL;
    HRESULT options11_hr = E_FAIL;
    HRESULT options14_hr = E_FAIL;
    HRESULT options16_hr = E_FAIL;
    HRESULT stream_output_format_hr = E_FAIL;
    HRESULT create_reserved_resource_hr = E_FAIL;
    HRESULT query_device5_hr = E_NOINTERFACE;
    HRESULT create_state_object_hr = E_NOINTERFACE;
    HRESULT invalid_feature_hr = E_FAIL;
    HRESULT zero_size_feature_hr = E_FAIL;
    HRESULT null_data_feature_hr = E_FAIL;
    HRESULT null_feature_level_list_hr = E_FAIL;

    if (device) {
        fl_hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &feature_levels, sizeof(feature_levels));
        sm_hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model));
        options_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        options1_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1));
        options2_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2));
        options3_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3));
        options4_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4));
        options5_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        options7_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
        options9_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9));
        options11_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11, sizeof(options11));
        options14_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options14, sizeof(options14));
        options16_hr = device->CheckFeatureSupport(
            static_cast<D3D12_FEATURE>(45), &options16, sizeof(options16));
        stream_output_format_hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &stream_output_format,
                                                              sizeof(stream_output_format));
        UINT invalid_feature_payload = 0xdeadbeefu;
        invalid_feature_hr = device->CheckFeatureSupport(static_cast<D3D12_FEATURE>(0xffffffffu),
                                                         &invalid_feature_payload, sizeof(invalid_feature_payload));
        zero_size_feature_hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &invalid_feature_payload, 0);
        null_data_feature_hr =
            device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, nullptr, sizeof(invalid_feature_payload));
        D3D12_FEATURE_DATA_FEATURE_LEVELS null_feature_level_list = {};
        null_feature_level_list.NumFeatureLevels = 1;
        null_feature_level_list_hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &null_feature_level_list,
                                                                 sizeof(null_feature_level_list));

        D3D12_RESOURCE_DESC reserved_desc = {};
        reserved_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        reserved_desc.Width = 4096;
        reserved_desc.Height = 1;
        reserved_desc.DepthOrArraySize = 1;
        reserved_desc.MipLevels = 1;
        reserved_desc.SampleDesc.Count = 1;
        reserved_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* reserved_resource = nullptr;
        create_reserved_resource_hr = device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COMMON,
                                                                     nullptr, IID_PPV_ARGS(&reserved_resource));
        if (reserved_resource)
            reserved_resource->Release();

        ID3D12Device5* device5 = nullptr;
        query_device5_hr = device->QueryInterface(IID_PPV_ARGS(&device5));
        if (device5) {
            D3D12_STATE_OBJECT_DESC state_object_desc = {};
            state_object_desc.Type = D3D12_STATE_OBJECT_TYPE_COLLECTION;
            ID3D12StateObject* state_object = nullptr;
            create_state_object_hr = device5->CreateStateObject(&state_object_desc, IID_PPV_ARGS(&state_object));
            if (state_object)
                state_object->Release();
            device5->Release();
        }
    }

    bool feature_level_ok = SUCCEEDED(fl_hr) && feature_levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_0;
    bool shader_model_target_ok = SUCCEEDED(sm_hr) && shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    bool shader_model_6_6_or_better = SUCCEEDED(sm_hr) && shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
    bool binding_tier_ok = SUCCEEDED(options_hr) && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
    bool double_precision_reported = SUCCEEDED(options_hr) && options.DoublePrecisionFloatShaderOps;
    bool wave_ops_proven_reported = SUCCEEDED(options1_hr) && options1.WaveOps && options1.WaveLaneCountMin == 32 &&
                                    options1.WaveLaneCountMax == 32;
    bool atomic64_conservative = SUCCEEDED(options9_hr) && options9.AtomicInt64OnTypedResourceSupported &&
                                 options9.AtomicInt64OnGroupSharedSupported && SUCCEEDED(options11_hr) &&
                                 options11.AtomicInt64OnDescriptorHeapResourceSupported;
    bool native16_reported = SUCCEEDED(options4_hr) && options4.Native16BitShaderOpsSupported;
    bool view_instancing_reported = SUCCEEDED(options3_hr) &&
                                     options3.ViewInstancingTier >= D3D12_VIEW_INSTANCING_TIER_1;
    bool barycentrics_reported = SUCCEEDED(options3_hr) &&
                                 options3.BarycentricsSupported;
    bool programmable_sample_positions_reported =
        SUCCEEDED(options2_hr) &&
        options2.ProgrammableSamplePositionsTier >=
            D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1;
    bool advanced_features_reported =
        SUCCEEDED(options5_hr) && options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 && SUCCEEDED(options7_hr) &&
        options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 &&
        options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9 &&
        (!SUCCEEDED(options9_hr) || options9.WaveMMATier == D3D12_WAVE_MMA_TIER_NOT_SUPPORTED) &&
        SUCCEEDED(options14_hr) && options14.AdvancedTextureOpsSupported && options14.WriteableMSAATexturesSupported;
    bool stream_output_conservative =
        SUCCEEDED(stream_output_format_hr) && !(stream_output_format.Support1 & D3D12_FORMAT_SUPPORT1_SO_BUFFER);
    bool gpu_upload_supported =
        SUCCEEDED(options16_hr) && options16.GPUUploadHeapSupported;
    bool reserved_resources_unsupported = FAILED(create_reserved_resource_hr);
    bool state_objects_unsupported = FAILED(query_device5_hr) || FAILED(create_state_object_hr);
    bool feature_query_validation = invalid_feature_hr == E_INVALIDARG && zero_size_feature_hr == E_INVALIDARG &&
                                    null_data_feature_hr == E_POINTER && null_feature_level_list_hr == E_INVALIDARG;
    bool pass = SUCCEEDED(create_hr) && feature_level_ok && shader_model_target_ok && binding_tier_ok &&
                double_precision_reported && native16_reported && view_instancing_reported &&
                barycentrics_reported && programmable_sample_positions_reported &&
                wave_ops_proven_reported &&
                atomic64_conservative && advanced_features_reported &&
                gpu_upload_supported && stream_output_conservative && reserved_resources_unsupported &&
                state_objects_unsupported &&
                feature_query_validation;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-device-caps.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"sdk\": {\"D3D12SDKVersion\": %u, \"D3D12SDKPath\": \"%s\"},\n", D3D12SDKVersion,
                json_escape(D3D12SDKPath).c_str());
    std::printf("  \"device_create\": {\n");
    std::printf("    \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(create_hr)));
    std::printf("    \"succeeded\": %s\n", SUCCEEDED(create_hr) ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"feature_levels\": {\n");
    print_hr("check", fl_hr);
    std::printf("    \"max_supported\": \"%s\",\n",
                feature_level_name(feature_levels.MaxSupportedFeatureLevel).c_str());
    std::printf("    \"meets_12_0\": %s\n", feature_level_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"shader_model\": {\n");
    print_hr("check", sm_hr);
    std::printf("    \"highest\": \"%s\",\n", shader_model_name(shader_model.HighestShaderModel).c_str());
    std::printf("    \"meets_6_5\": %s,\n", shader_model_target_ok ? "true" : "false");
    std::printf("    \"meets_6_6\": %s\n", shader_model_6_6_or_better ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"options\": {\n");
    print_hr("check", options_hr);
    std::printf("    \"resource_binding_tier\": %u,\n", static_cast<unsigned>(options.ResourceBindingTier));
    std::printf("    \"resource_heap_tier\": %u,\n", static_cast<unsigned>(options.ResourceHeapTier));
    std::printf("    \"output_merger_logic_op\": %s,\n", options.OutputMergerLogicOp ? "true" : "false");
    std::printf("    \"double_precision_float_shader_ops\": %s,\n",
                options.DoublePrecisionFloatShaderOps ? "true" : "false");
    std::printf("    \"rovs_supported\": %s,\n", options.ROVsSupported ? "true" : "false");
    std::printf("    \"conservative_rasterization_tier\": %u\n",
                static_cast<unsigned>(options.ConservativeRasterizationTier));
    std::printf("  },\n");
    std::printf("  \"options1\": {\n");
    print_hr("check", options1_hr);
    std::printf("    \"wave_ops\": %s,\n", options1.WaveOps ? "true" : "false");
    std::printf("    \"wave_lane_count_min\": %u,\n", options1.WaveLaneCountMin);
    std::printf("    \"wave_lane_count_max\": %u,\n", options1.WaveLaneCountMax);
    std::printf("    \"int64_shader_ops\": %s,\n", options1.Int64ShaderOps ? "true" : "false");
    std::printf("    \"wave_ops_proven_reported\": %s\n", wave_ops_proven_reported ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"advanced_features\": {\n");
    print_hr("options2", options2_hr);
    std::printf("    \"programmable_sample_positions_tier\": %u,\n",
                static_cast<unsigned>(options2.ProgrammableSamplePositionsTier));
    print_hr("options3", options3_hr);
    std::printf("    \"view_instancing_tier\": %u,\n",
                static_cast<unsigned>(options3.ViewInstancingTier));
    std::printf("    \"barycentrics_supported\": %s,\n",
                options3.BarycentricsSupported ? "true" : "false");
    print_hr("options4", options4_hr);
    std::printf("    \"native16_bit_shader_ops\": %s,\n",
                options4.Native16BitShaderOpsSupported ? "true" : "false");
    std::printf("    \"msaa64kb_aligned_texture\": %s,\n",
                options4.MSAA64KBAlignedTextureSupported ? "true" : "false");
    print_hr("options5", options5_hr);
    std::printf("    \"raytracing_tier\": %u,\n", static_cast<unsigned>(options5.RaytracingTier));
    print_hr("options7", options7_hr);
    std::printf("    \"mesh_shader_tier\": %u,\n", static_cast<unsigned>(options7.MeshShaderTier));
    std::printf("    \"sampler_feedback_tier\": %u,\n", static_cast<unsigned>(options7.SamplerFeedbackTier));
    print_hr("options9", options9_hr);
    std::printf("    \"mesh_shader_pipeline_stats_supported\": %s,\n",
                options9.MeshShaderPipelineStatsSupported ? "true" : "false");
    std::printf("    \"atomic64_typed_resource\": %s,\n",
                options9.AtomicInt64OnTypedResourceSupported ? "true" : "false");
    std::printf("    \"atomic64_group_shared\": %s,\n", options9.AtomicInt64OnGroupSharedSupported ? "true" : "false");
    std::printf("    \"wave_mma_tier\": %u,\n", static_cast<unsigned>(options9.WaveMMATier));
    print_hr("options11", options11_hr);
    std::printf("    \"atomic64_descriptor_heap_resource\": %s\n",
                options11.AtomicInt64OnDescriptorHeapResourceSupported ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"options14\": {\n");
    print_hr("check", options14_hr);
    std::printf("    \"advanced_texture_ops\": %s,\n", options14.AdvancedTextureOpsSupported ? "true" : "false");
    std::printf("    \"writeable_msaa_textures\": %s\n", options14.WriteableMSAATexturesSupported ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"options16\": {\n");
    print_hr("check", options16_hr);
    std::printf("    \"gpu_upload_heap_supported\": %s\n", options16.GPUUploadHeapSupported ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"unsupported_policy\": {\n");
    print_hr("stream_output_format", stream_output_format_hr);
    std::printf("    \"stream_output_so_buffer_advertised\": %s,\n",
                (stream_output_format.Support1 & D3D12_FORMAT_SUPPORT1_SO_BUFFER) ? "true" : "false");
    print_hr("create_reserved_resource", create_reserved_resource_hr);
    print_hr("query_device5", query_device5_hr);
    print_hr("create_state_object", create_state_object_hr);
    print_hr("invalid_feature", invalid_feature_hr);
    print_hr("zero_size_feature", zero_size_feature_hr);
    print_hr("null_data_feature", null_data_feature_hr);
    print_hr("null_feature_level_list", null_feature_level_list_hr);
    std::printf("    \"stream_output_conservative\": %s,\n", stream_output_conservative ? "true" : "false");
    std::printf("    \"reserved_resources_unsupported\": %s,\n", reserved_resources_unsupported ? "true" : "false");
    std::printf("    \"state_objects_unsupported\": %s,\n", state_objects_unsupported ? "true" : "false");
    std::printf("    \"feature_query_validation\": %s\n", feature_query_validation ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"requirements\": {\n");
    std::printf("    \"feature_level_12_0_or_better\": %s,\n", feature_level_ok ? "true" : "false");
    std::printf("    \"shader_model_6_5_or_better\": %s,\n", shader_model_target_ok ? "true" : "false");
    std::printf("    \"shader_model_6_6_or_better\": %s,\n", shader_model_6_6_or_better ? "true" : "false");
    std::printf("    \"binding_tier_3\": %s,\n", binding_tier_ok ? "true" : "false");
    std::printf("    \"double_precision_reported\": %s,\n", double_precision_reported ? "true" : "false");
    std::printf("    \"native16_reported\": %s,\n", native16_reported ? "true" : "false");
    std::printf("    \"view_instancing_reported\": %s,\n", view_instancing_reported ? "true" : "false");
    std::printf("    \"barycentrics_reported\": %s,\n", barycentrics_reported ? "true" : "false");
    std::printf("    \"programmable_sample_positions_reported\": %s,\n",
                programmable_sample_positions_reported ? "true" : "false");
    std::printf("    \"wave_ops_proven_reported\": %s,\n", wave_ops_proven_reported ? "true" : "false");
    std::printf("    \"atomic64_conservative\": %s,\n", atomic64_conservative ? "true" : "false");
    std::printf("    \"advanced_features_reported\": %s,\n", advanced_features_reported ? "true" : "false");
    std::printf("    \"gpu_upload_supported\": %s,\n", gpu_upload_supported ? "true" : "false");
    std::printf("    \"mesh_shader_pipeline_stats_supported\": %s,\n",
                options9.MeshShaderPipelineStatsSupported ? "true" : "false");
    std::printf("    \"stream_output_conservative\": %s,\n", stream_output_conservative ? "true" : "false");
    std::printf("    \"reserved_resources_unsupported\": %s,\n", reserved_resources_unsupported ? "true" : "false");
    std::printf("    \"state_objects_unsupported\": %s,\n", state_objects_unsupported ? "true" : "false");
    std::printf("    \"feature_query_validation\": %s\n", feature_query_validation ? "true" : "false");
    std::printf("  }\n");
    std::printf("}\n");

    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
