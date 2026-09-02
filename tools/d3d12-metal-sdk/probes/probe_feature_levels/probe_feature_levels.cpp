#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <d3d12.h>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

static std::string getenv_string(const char* key) {
    DWORD needed = GetEnvironmentVariableA(key, nullptr, 0);
    if (!needed)
        return "";
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (!written)
        return "";
    value.resize(written);
    return value;
}

static std::string json_escape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
                output += escaped;
            } else {
                output += c;
            }
        }
    }
    return output;
}

static void configure_exported_sdk() {
    std::string version = getenv_string("D3D12_METAL_SDK_AGILITY_VERSION");
    if (!version.empty())
        D3D12SDKVersion = static_cast<UINT>(std::strtoul(version.c_str(), nullptr, 10));
    std::string path = getenv_string("D3D12_METAL_SDK_AGILITY_PATH");
    if (!path.empty())
        std::snprintf(D3D12SDKPath, sizeof(D3D12SDKPath), "%s", path.c_str());
}

static const char* feature_level_name(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_11_0:
        return "11_0";
    case D3D_FEATURE_LEVEL_11_1:
        return "11_1";
    case D3D_FEATURE_LEVEL_12_0:
        return "12_0";
    case D3D_FEATURE_LEVEL_12_1:
        return "12_1";
    case D3D_FEATURE_LEVEL_12_2:
        return "12_2";
    default:
        return "unknown";
    }
}

static const char* shader_model_name(D3D_SHADER_MODEL model) {
    switch (model) {
    case D3D_SHADER_MODEL_6_5:
        return "6_5";
    case D3D_SHADER_MODEL_6_6:
        return "6_6";
    case D3D_SHADER_MODEL_6_7:
        return "6_7";
    default:
        return "other";
    }
}

struct CreateResult {
    D3D_FEATURE_LEVEL level;
    HRESULT hr;
    bool object_returned;
};

int main() {
    configure_exported_sdk();
    const std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));

    constexpr std::array<D3D_FEATURE_LEVEL, 5> required_levels = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                                                  D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
                                                                  D3D_FEATURE_LEVEL_12_2};
    std::array<CreateResult, required_levels.size()> create_results = {};

    ID3D12Device* device = nullptr;
    D3D_FEATURE_LEVEL device_level = D3D_FEATURE_LEVEL_11_0;
    for (size_t i = 0; i < required_levels.size(); ++i) {
        ID3D12Device* candidate = nullptr;
        HRESULT hr = create_device ? create_device(nullptr, required_levels[i], IID_D3D12DeviceProbe,
                                                   reinterpret_cast<void**>(&candidate))
                                   : E_NOINTERFACE;
        create_results[i] = {required_levels[i], hr, candidate != nullptr};
        if (candidate) {
            if (!device || required_levels[i] >= device_level) {
                if (device)
                    device->Release();
                device = candidate;
                device_level = required_levels[i];
            } else {
                candidate->Release();
            }
        }
    }

    ID3D12Device* invalid_device = nullptr;
    HRESULT invalid_level_hr = create_device
                                   ? create_device(nullptr, static_cast<D3D_FEATURE_LEVEL>(0xffff),
                                                   IID_D3D12DeviceProbe, reinterpret_cast<void**>(&invalid_device))
                                   : E_NOINTERFACE;
    if (invalid_device)
        invalid_device->Release();

    D3D_FEATURE_LEVEL requested_levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {static_cast<UINT>(std::size(requested_levels)), requested_levels,
                                                D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model = {D3D_SHADER_MODEL_6_7};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS10 options10 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14 = {};
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature = {D3D_ROOT_SIGNATURE_VERSION_1_1};
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gpu_va = {};

    HRESULT levels_hr = E_NOINTERFACE;
    HRESULT shader_model_hr = E_NOINTERFACE;
    HRESULT options_hr = E_NOINTERFACE;
    HRESULT options1_hr = E_NOINTERFACE;
    HRESULT options2_hr = E_NOINTERFACE;
    HRESULT options3_hr = E_NOINTERFACE;
    HRESULT options4_hr = E_NOINTERFACE;
    HRESULT options5_hr = E_NOINTERFACE;
    HRESULT options6_hr = E_NOINTERFACE;
    HRESULT options7_hr = E_NOINTERFACE;
    HRESULT options8_hr = E_NOINTERFACE;
    HRESULT options10_hr = E_NOINTERFACE;
    HRESULT options12_hr = E_NOINTERFACE;
    HRESULT options14_hr = E_NOINTERFACE;
    HRESULT root_signature_hr = E_NOINTERFACE;
    HRESULT gpu_va_hr = E_NOINTERFACE;

#define CHECK_FEATURE(name, value) name##_hr = device->CheckFeatureSupport(D3D12_FEATURE_##value, &name, sizeof(name))
    if (device) {
        CHECK_FEATURE(levels, FEATURE_LEVELS);
        CHECK_FEATURE(shader_model, SHADER_MODEL);
        CHECK_FEATURE(options, D3D12_OPTIONS);
        CHECK_FEATURE(options1, D3D12_OPTIONS1);
        CHECK_FEATURE(options2, D3D12_OPTIONS2);
        CHECK_FEATURE(options3, D3D12_OPTIONS3);
        CHECK_FEATURE(options4, D3D12_OPTIONS4);
        CHECK_FEATURE(options5, D3D12_OPTIONS5);
        CHECK_FEATURE(options6, D3D12_OPTIONS6);
        CHECK_FEATURE(options7, D3D12_OPTIONS7);
        CHECK_FEATURE(options8, D3D12_OPTIONS8);
        CHECK_FEATURE(options10, D3D12_OPTIONS10);
        CHECK_FEATURE(options12, D3D12_OPTIONS12);
        CHECK_FEATURE(options14, D3D12_OPTIONS14);
        CHECK_FEATURE(root_signature, ROOT_SIGNATURE);
        CHECK_FEATURE(gpu_va, GPU_VIRTUAL_ADDRESS_SUPPORT);
    }
#undef CHECK_FEATURE

    bool all_levels_create = true;
    for (const auto& result : create_results)
        all_levels_create = all_levels_create && SUCCEEDED(result.hr) && result.object_returned;

    const D3D12_COMMAND_LIST_SUPPORT_FLAGS required_write_immediate = static_cast<D3D12_COMMAND_LIST_SUPPORT_FLAGS>(
        D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
        D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE);

    const bool fl12_2_caps =
        SUCCEEDED(levels_hr) && levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_2 && SUCCEEDED(options_hr) &&
        options.OutputMergerLogicOp && options.DoublePrecisionFloatShaderOps &&
        options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_3 &&
        options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 &&
        options.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_3 &&
        options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation &&
        options.MaxGPUVirtualAddressBitsPerResource >= 40 && SUCCEEDED(options1_hr) && options1.WaveOps &&
        options1.Int64ShaderOps && SUCCEEDED(options2_hr) && options2.DepthBoundsTestSupported &&
        SUCCEEDED(options3_hr) && options3.CopyQueueTimestampQueriesSupported &&
        SUCCEEDED(options4_hr) && options4.Native16BitShaderOpsSupported &&
        options3.CastingFullyTypedFormatSupported &&
        (options3.WriteBufferImmediateSupportFlags & required_write_immediate) == required_write_immediate &&
        SUCCEEDED(options5_hr) && options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 && SUCCEEDED(options6_hr) &&
        options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2 && SUCCEEDED(options7_hr) &&
        options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 &&
        options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9 && SUCCEEDED(options8_hr) &&
        options8.UnalignedBlockTexturesSupported && SUCCEEDED(options10_hr) && SUCCEEDED(root_signature_hr) &&
        root_signature.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1 && SUCCEEDED(gpu_va_hr) &&
        gpu_va.MaxGPUVirtualAddressBitsPerResource >= 40 && gpu_va.MaxGPUVirtualAddressBitsPerProcess >= 40;

    const bool sm67_caps = SUCCEEDED(shader_model_hr) && shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_7 &&
                           SUCCEEDED(options12_hr) && options12.EnhancedBarriersSupported &&
                           options12.RelaxedFormatCastingSupported && SUCCEEDED(options14_hr) &&
                           options14.AdvancedTextureOpsSupported && options14.WriteableMSAATexturesSupported;

    const bool invalid_level_rejected = FAILED(invalid_level_hr);
    const bool pass = create_device && all_levels_create && fl12_2_caps && sm67_caps && invalid_level_rejected;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-feature-levels.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"sdk\": {\"version\": %u, \"path\": \"%s\"},\n", D3D12SDKVersion,
                json_escape(D3D12SDKPath).c_str());
    std::printf("  \"device_creation\": [\n");
    for (size_t i = 0; i < create_results.size(); ++i) {
        const auto& result = create_results[i];
        std::printf("    {\"level\": \"%s\", \"hr\": \"0x%08lx\", \"object_returned\": %s}%s\n",
                    feature_level_name(result.level), static_cast<unsigned long>(static_cast<uint32_t>(result.hr)),
                    result.object_returned ? "true" : "false", i + 1 == create_results.size() ? "" : ",");
    }
    std::printf("  ],\n");
    std::printf("  \"invalid_level\": {\"hr\": \"0x%08lx\", \"rejected\": %s},\n",
                static_cast<unsigned long>(static_cast<uint32_t>(invalid_level_hr)),
                invalid_level_rejected ? "true" : "false");
    std::printf("  \"check_results\": {\n");
    std::printf("    \"feature_levels\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(levels_hr)));
    std::printf("    \"shader_model\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(shader_model_hr)));
    std::printf("    \"options\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options_hr)));
    std::printf("    \"options1\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options1_hr)));
    std::printf("    \"options2\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options2_hr)));
    std::printf("    \"options3\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options3_hr)));
    std::printf("    \"options4\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options4_hr)));
    std::printf("    \"options5\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options5_hr)));
    std::printf("    \"options6\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options6_hr)));
    std::printf("    \"options7\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options7_hr)));
    std::printf("    \"options8\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options8_hr)));
    std::printf("    \"options10\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options10_hr)));
    std::printf("    \"options12\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options12_hr)));
    std::printf("    \"options14\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options14_hr)));
    std::printf("    \"root_signature\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(root_signature_hr)));
    std::printf("    \"gpu_virtual_address_support\": \"0x%08lx\"\n",
                static_cast<unsigned long>(static_cast<uint32_t>(gpu_va_hr)));
    std::printf("  },\n");
    std::printf("  \"reported\": {\n");
    std::printf("    \"feature_level\": \"%s\",\n", feature_level_name(levels.MaxSupportedFeatureLevel));
    std::printf("    \"shader_model\": \"%s\",\n", shader_model_name(shader_model.HighestShaderModel));
    std::printf("    \"resource_binding_tier\": %u,\n", static_cast<unsigned>(options.ResourceBindingTier));
    std::printf("    \"rovs_supported\": %s,\n", options.ROVsSupported ? "true" : "false");
    std::printf("    \"output_merger_logic_op\": %s,\n", options.OutputMergerLogicOp ? "true" : "false");
    std::printf("    \"double_precision_float_shader_ops\": %s,\n",
                options.DoublePrecisionFloatShaderOps ? "true" : "false");
    std::printf("    \"native16_bit_shader_ops\": %s,\n",
                options4.Native16BitShaderOpsSupported ? "true" : "false");
    std::printf("    \"raytracing_tier\": %u,\n", static_cast<unsigned>(options5.RaytracingTier));
    std::printf("    \"srv_only_tiled_resource_tier3\": %s,\n", options5.SRVOnlyTiledResourceTier3 ? "true" : "false");
    std::printf("    \"vrs_tier\": %u,\n", static_cast<unsigned>(options6.VariableShadingRateTier));
    std::printf("    \"additional_shading_rates\": %s,\n", options6.AdditionalShadingRatesSupported ? "true" : "false");
    std::printf("    \"per_primitive_shading_rate_with_viewport_indexing\": %s,\n",
                options6.PerPrimitiveShadingRateSupportedWithViewportIndexing ? "true" : "false");
    std::printf("    \"shading_rate_image_tile_size\": %u,\n",
                static_cast<unsigned>(options6.ShadingRateImageTileSize));
    std::printf("    \"mesh_shader_tier\": %u,\n", static_cast<unsigned>(options7.MeshShaderTier));
    std::printf("    \"sampler_feedback_tier\": %u,\n", static_cast<unsigned>(options7.SamplerFeedbackTier));
    std::printf("    \"vrs_sum_combiner_supported\": %s,\n",
                options10.VariableRateShadingSumCombinerSupported ? "true" : "false");
    std::printf("    \"mesh_shader_per_primitive_shading_rate_supported\": %s,\n",
                options10.MeshShaderPerPrimitiveShadingRateSupported ? "true" : "false");
    std::printf("    \"tiled_resources_tier\": %u,\n", static_cast<unsigned>(options.TiledResourcesTier));
    std::printf("    \"conservative_rasterization_tier\": %u,\n",
                static_cast<unsigned>(options.ConservativeRasterizationTier));
    std::printf("    \"vp_array_index_without_gs\": %s,\n",
                options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation ? "true" : "false");
    std::printf("    \"max_gpu_virtual_address_bits_per_resource\": %u,\n",
                static_cast<unsigned>(options.MaxGPUVirtualAddressBitsPerResource));
    std::printf("    \"wave_ops\": %s,\n", options1.WaveOps ? "true" : "false");
    std::printf("    \"wave_lane_count_min\": %u,\n", static_cast<unsigned>(options1.WaveLaneCountMin));
    std::printf("    \"wave_lane_count_max\": %u,\n", static_cast<unsigned>(options1.WaveLaneCountMax));
    std::printf("    \"int64_shader_ops\": %s,\n", options1.Int64ShaderOps ? "true" : "false");
    std::printf("    \"depth_bounds\": %s,\n", options2.DepthBoundsTestSupported ? "true" : "false");
    std::printf("    \"copy_queue_timestamps\": %s,\n", options3.CopyQueueTimestampQueriesSupported ? "true" : "false");
    std::printf("    \"casting_fully_typed_format\": %s,\n",
                options3.CastingFullyTypedFormatSupported ? "true" : "false");
    std::printf("    \"write_buffer_immediate_flags\": %u,\n",
                static_cast<unsigned>(options3.WriteBufferImmediateSupportFlags));
    std::printf("    \"unaligned_block_textures\": %s,\n", options8.UnalignedBlockTexturesSupported ? "true" : "false");
    std::printf("    \"root_signature_1_1\": %s,\n",
                root_signature.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1 ? "true" : "false");
    std::printf("    \"gpu_virtual_address_bits_per_resource\": %u,\n",
                static_cast<unsigned>(gpu_va.MaxGPUVirtualAddressBitsPerResource));
    std::printf("    \"gpu_virtual_address_bits_per_process\": %u,\n",
                static_cast<unsigned>(gpu_va.MaxGPUVirtualAddressBitsPerProcess));
    std::printf("    \"options12_enhanced_barriers\": %s,\n", options12.EnhancedBarriersSupported ? "true" : "false");
    std::printf("    \"options12_relaxed_format_casting\": %s,\n",
                options12.RelaxedFormatCastingSupported ? "true" : "false");
    std::printf("    \"enhanced_barriers\": %s,\n", options12.EnhancedBarriersSupported ? "true" : "false");
    std::printf("    \"relaxed_format_casting\": %s,\n", options12.RelaxedFormatCastingSupported ? "true" : "false");
    std::printf("    \"advanced_texture_ops\": %s,\n", options14.AdvancedTextureOpsSupported ? "true" : "false");
    std::printf("    \"writeable_msaa_textures\": %s\n", options14.WriteableMSAATexturesSupported ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"requirements\": {\n");
    std::printf("    \"all_levels_create\": %s,\n", all_levels_create ? "true" : "false");
    std::printf("    \"feature_level_12_2_caps\": %s,\n", fl12_2_caps ? "true" : "false");
    std::printf("    \"shader_model_6_7_caps\": %s,\n", sm67_caps ? "true" : "false");
    std::printf("    \"invalid_level_rejected\": %s\n", invalid_level_rejected ? "true" : "false");
    std::printf("  }\n");
    std::printf("}\n");
    std::fflush(stdout);

    if (device)
        device->Release();
    if (d3d12)
        FreeLibrary(d3d12);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
