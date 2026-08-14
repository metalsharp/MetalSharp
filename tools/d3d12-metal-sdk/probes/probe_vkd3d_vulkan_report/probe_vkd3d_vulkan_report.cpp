#include <dlfcn.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using VkBool32 = uint32_t;
using VkFlags = uint32_t;
using VkInstance = struct VkInstance_T*;
using VkPhysicalDevice = struct VkPhysicalDevice_T*;
using VkResult = int32_t;
using VkStructureType = int32_t;

constexpr VkResult VK_SUCCESS = 0;
constexpr VkStructureType VK_STRUCTURE_TYPE_APPLICATION_INFO = 0;
constexpr VkStructureType VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1;
constexpr uint32_t VK_MAX_EXTENSION_NAME_SIZE = 256;
constexpr uint32_t VK_MAX_PHYSICAL_DEVICE_NAME_SIZE = 256;
constexpr VkFlags VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR = 0x00000001u;
constexpr const char* VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME = "VK_KHR_portability_enumeration";

#define VK_MAKE_API_VERSION(variant, major, minor, patch)                                                              \
    ((((uint32_t)(variant)) << 29U) | (((uint32_t)(major)) << 22U) | (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))

struct VkAllocationCallbacks;

struct VkApplicationInfo {
    VkStructureType sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

struct VkExtensionProperties {
    char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
};

struct alignas(8) VkPhysicalDevicePropertiesBuffer {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t pipelineCacheUUID[16];
    uint8_t reserved_for_limits_and_sparse_properties[4096];
};
static_assert(alignof(VkPhysicalDevicePropertiesBuffer) >= 8, "Vulkan properties buffer must be 8-byte aligned");
static_assert(offsetof(VkPhysicalDevicePropertiesBuffer, apiVersion) == 0, "unexpected apiVersion offset");
static_assert(offsetof(VkPhysicalDevicePropertiesBuffer, deviceName) == 20, "unexpected deviceName offset");
static_assert(sizeof(VkPhysicalDevicePropertiesBuffer) % 8 == 0,
              "Vulkan properties buffer size must preserve 8-byte stride");

using PFN_vkVoidFunction = void (*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction (*)(VkInstance, const char*);
using PFN_vkEnumerateInstanceExtensionProperties = VkResult (*)(const char*, uint32_t*, VkExtensionProperties*);
using PFN_vkCreateInstance = VkResult (*)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
using PFN_vkEnumeratePhysicalDevices = VkResult (*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties = void (*)(VkPhysicalDevice, VkPhysicalDevicePropertiesBuffer*);
using PFN_vkDestroyInstance = void (*)(VkInstance, const VkAllocationCallbacks*);

static std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (char c : value) {
        switch (c) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << c;
            break;
        }
    }
    return out.str();
}

static bool file_exists(const std::string& path) {
    struct stat st = {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static long long file_size(const std::string& path) {
    struct stat st = {};
    if (path.empty() || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return static_cast<long long>(st.st_size);
}

static std::string arg_value(int argc, const char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0)
            return argv[i + 1];
    }
    return "";
}

static const char* device_type_name(uint32_t type) {
    switch (type) {
    case 0:
        return "other";
    case 1:
        return "integrated_gpu";
    case 2:
        return "discrete_gpu";
    case 3:
        return "virtual_gpu";
    case 4:
        return "cpu";
    default:
        return "unknown";
    }
}

int main(int argc, const char** argv) {
    const std::string loader_path = arg_value(argc, argv, "--loader");
    const std::string icd_path = arg_value(argc, argv, "--icd");
    const std::string moltenvk_path = arg_value(argc, argv, "--moltenvk");
    const std::string winevulkan_so_path = arg_value(argc, argv, "--winevulkan-so");
    const std::string vulkan_dll_path = arg_value(argc, argv, "--vulkan-dll");
    const std::string winevulkan_dll_path = arg_value(argc, argv, "--winevulkan-dll");

    if (!icd_path.empty()) {
        setenv("VK_ICD_FILENAMES", icd_path.c_str(), 1);
        setenv("VK_DRIVER_FILES", icd_path.c_str(), 1);
    }

    bool loader_exists = file_exists(loader_path);
    bool icd_exists = file_exists(icd_path);
    bool moltenvk_exists = file_exists(moltenvk_path);
    bool winevulkan_so_exists = file_exists(winevulkan_so_path);
    bool vulkan_dll_exists = file_exists(vulkan_dll_path);
    bool winevulkan_dll_exists = file_exists(winevulkan_dll_path);

    std::string winevulkan_so_error;
    void* winevulkan_so = winevulkan_so_exists ? dlopen(winevulkan_so_path.c_str(), RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!winevulkan_so) {
        const char* err = dlerror();
        winevulkan_so_error = err ? err : "dlopen_failed";
    }
    bool winevulkan_unix_call_funcs_ok = winevulkan_so && dlsym(winevulkan_so, "__wine_unix_call_funcs") != nullptr;
    bool winevulkan_unix_call_wow64_funcs_ok =
        winevulkan_so && dlsym(winevulkan_so, "__wine_unix_call_wow64_funcs") != nullptr;

    std::string dlopen_error;
    void* loader = loader_exists ? dlopen(loader_path.c_str(), RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!loader) {
        const char* err = dlerror();
        dlopen_error = err ? err : "dlopen_failed";
    }

    auto get_instance_proc_addr =
        loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
    bool gipa_ok = get_instance_proc_addr != nullptr;

    auto enumerate_instance_extensions =
        gipa_ok ? reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                      get_instance_proc_addr(nullptr, "vkEnumerateInstanceExtensionProperties"))
                : nullptr;
    auto create_instance =
        gipa_ok ? reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc_addr(nullptr, "vkCreateInstance")) : nullptr;

    uint32_t extension_count = 0;
    VkResult enumerate_extensions_result = -1;
    std::vector<VkExtensionProperties> extensions;
    bool portability_supported = false;
    bool moltenvk_extension_supported = false;
    if (enumerate_instance_extensions) {
        enumerate_extensions_result = enumerate_instance_extensions(nullptr, &extension_count, nullptr);
        if (enumerate_extensions_result == VK_SUCCESS && extension_count > 0) {
            extensions.resize(extension_count);
            enumerate_extensions_result = enumerate_instance_extensions(nullptr, &extension_count, extensions.data());
            extensions.resize(extension_count);
            for (const auto& ext : extensions) {
                if (std::strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
                    portability_supported = true;
                if (std::strcmp(ext.extensionName, "VK_MVK_moltenvk") == 0)
                    moltenvk_extension_supported = true;
            }
        }
    }

    VkInstance instance = nullptr;
    VkResult create_instance_result = -1;
    std::vector<const char*> enabled_extensions;
    if (portability_supported)
        enabled_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (create_instance) {
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "metalsharp-vkd3d-vulkan-report";
        app_info.applicationVersion = 1;
        app_info.pEngineName = "metalsharp-fresh-proof";
        app_info.engineVersion = 1;
        app_info.apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);

        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.flags = portability_supported ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
        create_info.ppEnabledExtensionNames = enabled_extensions.empty() ? nullptr : enabled_extensions.data();
        create_instance_result = create_instance(&create_info, nullptr, &instance);
    }

    auto enumerate_physical_devices = instance && gipa_ok
                                          ? reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
                                                get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices"))
                                          : nullptr;
    auto get_physical_device_properties = instance && gipa_ok
                                              ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                                                    get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties"))
                                              : nullptr;
    auto destroy_instance =
        instance && gipa_ok
            ? reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc_addr(instance, "vkDestroyInstance"))
            : nullptr;

    uint32_t physical_device_count = 0;
    VkResult enumerate_devices_result = -1;
    std::vector<VkPhysicalDevice> physical_devices;
    std::vector<VkPhysicalDevicePropertiesBuffer> properties;
    if (instance && enumerate_physical_devices && get_physical_device_properties) {
        enumerate_devices_result = enumerate_physical_devices(instance, &physical_device_count, nullptr);
        if (enumerate_devices_result == VK_SUCCESS && physical_device_count > 0) {
            physical_devices.resize(physical_device_count);
            enumerate_devices_result =
                enumerate_physical_devices(instance, &physical_device_count, physical_devices.data());
            physical_devices.resize(physical_device_count);
            properties.resize(physical_device_count);
            for (uint32_t i = 0; i < physical_device_count; ++i) {
                std::memset(&properties[i], 0, sizeof(properties[i]));
                get_physical_device_properties(physical_devices[i], &properties[i]);
            }
        }
    }

    if (instance && destroy_instance)
        destroy_instance(instance, nullptr);
    if (loader)
        dlclose(loader);

    bool ok = loader_exists && icd_exists && moltenvk_exists && winevulkan_so_exists && vulkan_dll_exists &&
              winevulkan_dll_exists && winevulkan_so && winevulkan_unix_call_funcs_ok &&
              winevulkan_unix_call_wow64_funcs_ok && loader && gipa_ok && enumerate_instance_extensions &&
              create_instance && enumerate_extensions_result == VK_SUCCESS && moltenvk_extension_supported &&
              create_instance_result == VK_SUCCESS && instance != nullptr && enumerate_physical_devices &&
              get_physical_device_properties && enumerate_devices_result == VK_SUCCESS && physical_device_count > 0;

    std::cout << "{\n";
    std::cout << "  \"schema\": \"metalsharp.vkd3d.fresh.vulkan-report.v1\",\n";
    std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    std::cout << "  \"process_arch\": \"x86_64\",\n";
    std::cout << "  \"vk_icd_filenames\": \""
              << json_escape(getenv("VK_ICD_FILENAMES") ? getenv("VK_ICD_FILENAMES") : "") << "\",\n";
    std::cout << "  \"vk_driver_files\": \"" << json_escape(getenv("VK_DRIVER_FILES") ? getenv("VK_DRIVER_FILES") : "")
              << "\",\n";
    std::cout << "  \"paths\": {\n";
    std::cout << "    \"loader\": {\"path\": \"" << json_escape(loader_path)
              << "\", \"exists\": " << (loader_exists ? "true" : "false") << ", \"size\": " << file_size(loader_path)
              << "},\n";
    std::cout << "    \"icd\": {\"path\": \"" << json_escape(icd_path)
              << "\", \"exists\": " << (icd_exists ? "true" : "false") << ", \"size\": " << file_size(icd_path)
              << "},\n";
    std::cout << "    \"moltenvk\": {\"path\": \"" << json_escape(moltenvk_path)
              << "\", \"exists\": " << (moltenvk_exists ? "true" : "false")
              << ", \"size\": " << file_size(moltenvk_path) << "},\n";
    std::cout << "    \"winevulkan_so\": {\"path\": \"" << json_escape(winevulkan_so_path)
              << "\", \"exists\": " << (winevulkan_so_exists ? "true" : "false")
              << ", \"size\": " << file_size(winevulkan_so_path) << "},\n";
    std::cout << "    \"vulkan_dll\": {\"path\": \"" << json_escape(vulkan_dll_path)
              << "\", \"exists\": " << (vulkan_dll_exists ? "true" : "false")
              << ", \"size\": " << file_size(vulkan_dll_path) << "},\n";
    std::cout << "    \"winevulkan_dll\": {\"path\": \"" << json_escape(winevulkan_dll_path)
              << "\", \"exists\": " << (winevulkan_dll_exists ? "true" : "false")
              << ", \"size\": " << file_size(winevulkan_dll_path) << "}\n";
    std::cout << "  },\n";
    std::cout << "  \"winevulkan_so_open_ok\": " << (winevulkan_so != nullptr ? "true" : "false") << ",\n";
    std::cout << "  \"winevulkan_so_error\": \"" << json_escape(winevulkan_so_error) << "\",\n";
    std::cout << "  \"winevulkan_unix_call_funcs_ok\": " << (winevulkan_unix_call_funcs_ok ? "true" : "false") << ",\n";
    std::cout << "  \"winevulkan_unix_call_wow64_funcs_ok\": "
              << (winevulkan_unix_call_wow64_funcs_ok ? "true" : "false") << ",\n";
    std::cout << "  \"loader_open_ok\": " << (loader != nullptr ? "true" : "false") << ",\n";
    std::cout << "  \"dlopen_error\": \"" << json_escape(dlopen_error) << "\",\n";
    std::cout << "  \"vk_get_instance_proc_addr_ok\": " << (gipa_ok ? "true" : "false") << ",\n";
    std::cout << "  \"enumerate_instance_extensions_result\": " << enumerate_extensions_result << ",\n";
    std::cout << "  \"instance_extension_count\": " << extension_count << ",\n";
    std::cout << "  \"portability_enumeration_supported\": " << (portability_supported ? "true" : "false") << ",\n";
    std::cout << "  \"moltenvk_extension_supported\": " << (moltenvk_extension_supported ? "true" : "false") << ",\n";
    std::cout << "  \"create_instance_result\": " << create_instance_result << ",\n";
    std::cout << "  \"enumerate_physical_devices_result\": " << enumerate_devices_result << ",\n";
    std::cout << "  \"physical_device_count\": " << physical_device_count << ",\n";
    std::cout << "  \"physical_devices\": [";
    for (size_t i = 0; i < properties.size(); ++i) {
        const auto& prop = properties[i];
        if (i)
            std::cout << ", ";
        std::cout << "{\"name\": \"" << json_escape(prop.deviceName) << "\", \"api_version\": " << prop.apiVersion
                  << ", \"driver_version\": " << prop.driverVersion << ", \"vendor_id\": " << prop.vendorID
                  << ", \"device_id\": " << prop.deviceID << ", \"device_type\": " << prop.deviceType
                  << ", \"device_type_name\": \"" << device_type_name(prop.deviceType) << "\"}";
    }
    std::cout << "],\n";
    std::cout << "  \"instance_extensions\": [";
    for (size_t i = 0; i < extensions.size(); ++i) {
        if (i)
            std::cout << ", ";
        std::cout << "{\"name\": \"" << json_escape(extensions[i].extensionName)
                  << "\", \"spec_version\": " << extensions[i].specVersion << "}";
    }
    std::cout << "]\n";
    std::cout << "}\n";
    return ok ? 0 : 5;
}
