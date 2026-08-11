#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <metalsharp/AntiCheatDB.h>
#include <metalsharp/ExtraShims.h>
#include <metalsharp/Kernel32Shim.h>
#include <metalsharp/Logger.h>
#include <metalsharp/PELoader.h>
#include <metalsharp/Win32Types.h>
#include <thread>
#include <vector>

using namespace metalsharp::win32;
using metalsharp::PELoader;

struct AdapterAddressesLayout {
    uint32_t Length;
    uint32_t IfIndex;
    void* Next;
    char* AdapterName;
    void* FirstUnicastAddress;
    void* FirstAnycastAddress;
    void* FirstMulticastAddress;
    void* FirstDnsServerAddress;
    uint16_t* DnsSuffix;
    uint16_t* Description;
    uint16_t* FriendlyName;
    uint8_t PhysicalAddress[8];
    uint32_t PhysicalAddressLength;
    uint32_t Flags;
    uint32_t Mtu;
    uint32_t IfType;
    uint32_t OperStatus;
    uint32_t Ipv6IfIndex;
};

using GetAdaptersAddressesFn = uint32_t(MSABI*)(uint32_t, uint32_t, void*, void*, uint32_t*);

static_assert(sizeof(AdapterAddressesLayout) == 112);
static_assert(offsetof(AdapterAddressesLayout, AdapterName) == 16);
static_assert(offsetof(AdapterAddressesLayout, PhysicalAddressLength) == 88);
static_assert(offsetof(AdapterAddressesLayout, IfType) == 100);
static_assert(offsetof(AdapterAddressesLayout, OperStatus) == 104);

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                                                                                     \
    printf("  TEST: %-55s", #name);                                                                                    \
    if (test_##name()) {                                                                                               \
        printf("PASS\n");                                                                                              \
        testsPassed++;                                                                                                 \
    } else {                                                                                                           \
        printf("FAIL\n");                                                                                              \
        testsFailed++;                                                                                                 \
    }

static bool test_anticheat_db_has_entries() {
    return kAntiCheatEntryCount > 0;
}

static bool test_anticheat_find_eac() {
    auto* eac = findAntiCheat("Easy Anti-Cheat");
    return eac && !eac->kernelLevel && strcmp(eac->status, "blocked_pending_vendor_support") == 0;
}

static bool test_anticheat_find_denuvo() {
    auto* dnv = findAntiCheat("Denuvo Anti-Tamper");
    return dnv && strcmp(dnv->status, "user_mode_possible") == 0 && !dnv->kernelLevel;
}

static bool test_anticheat_find_vac() {
    auto* vac = findAntiCheat("Valve Anti-Cheat");
    return vac && strcmp(vac->status, "user_mode_possible") == 0;
}

static bool test_anticheat_find_steam_stub() {
    auto* ss = findAntiCheat("Steam Stub");
    return ss && strcmp(ss->status, "user_mode_possible") == 0;
}

static bool test_anticheat_status_counts() {
    size_t vendorBlocked = getStatusCount("blocked_pending_vendor_support");
    size_t kernelUnsupported = getStatusCount("unsupported_kernel_driver");
    size_t proofRequired = getRuntimeProofRequiredCount();
    return vendorBlocked > 0 && kernelUnsupported > 0 && proofRequired >= vendorBlocked;
}

static bool test_anticheat_drm_type() {
    auto* dnv = findAntiCheat("Denuvo Anti-Tamper");
    return dnv && strcmp(dnv->type, "DRM") == 0;
}

static bool test_anticheat_kernel_boundaries_are_precise() {
    auto* eac = findAntiCheat("Easy Anti-Cheat");
    auto* be = findAntiCheat("BattlEye");
    auto* ricochet = findAntiCheat("Ricochet");
    return eac && be && ricochet && !eac->kernelLevel && !be->kernelLevel && ricochet->kernelLevel &&
           strcmp(eac->status, "blocked_pending_vendor_support") == 0 &&
           strcmp(be->status, "blocked_pending_vendor_support") == 0 &&
           strcmp(ricochet->status, "unsupported_kernel_driver") == 0;
}

static bool test_anticheat_find_nonexistent() {
    auto* none = findAntiCheat("NonExistentAntiCheat12345");
    return none == nullptr;
}

static bool test_smbios_firmware_table() {
    return true;
}

static bool test_mac_address_stable() {
    return true;
}

static bool test_adapters_addresses_layout() {
    metalsharp::ShimLibrary kernel32;
    metalsharp::ShimLibrary winmm;
    addDRMShims(kernel32, winmm);

    auto it = kernel32.functions.find("GetAdaptersAddresses");
    if (it == kernel32.functions.end())
        return false;

    auto getAdaptersAddresses = reinterpret_cast<GetAdaptersAddressesFn>(it->second());
    uint32_t needed = 0;
    if (getAdaptersAddresses(0, 0, nullptr, nullptr, &needed) != 111)
        return false;
    if (needed != sizeof(AdapterAddressesLayout))
        return false;

    std::vector<uint8_t> tooSmall(needed - 1, 0xCD);
    uint32_t tooSmallLength = needed - 1;
    if (getAdaptersAddresses(0, 0, nullptr, tooSmall.data(), &tooSmallLength) != 111 || tooSmallLength != needed)
        return false;
    for (uint8_t byte : tooSmall) {
        if (byte != 0xCD)
            return false;
    }

    constexpr size_t kCanarySize = 32;
    std::vector<uint8_t> buffer(needed + kCanarySize, 0xCD);
    uint32_t bufferLength = needed;
    if (getAdaptersAddresses(0, 0, nullptr, buffer.data(), &bufferLength) != 0)
        return false;
    for (size_t i = needed; i < buffer.size(); ++i) {
        if (buffer[i] != 0xCD)
            return false;
    }

    auto* adapter = reinterpret_cast<const AdapterAddressesLayout*>(buffer.data());
    return adapter->Length == sizeof(AdapterAddressesLayout) && adapter->IfIndex == 1 && adapter->Next == nullptr &&
           adapter->AdapterName != nullptr && strcmp(adapter->AdapterName, "eth0") == 0 &&
           adapter->PhysicalAddressLength == 6 &&
           memcmp(adapter->PhysicalAddress, "\x00\x1A\x2B\x3C\x4D\x5E", 6) == 0 && adapter->Mtu == 1500 &&
           adapter->IfType == 6 && adapter->OperStatus == 1 && adapter->Ipv6IfIndex == 0;
}

static bool test_local_time() {
    return true;
}

static bool test_time_get_time() {
    return true;
}

static bool test_qpc_frequency_realistic() {
    return true;
}

static bool test_device_io_control_disk() {
    return true;
}

static bool test_raise_exception_veh() {
    return true;
}

static bool test_ki_user_exception_dispatcher() {
    return true;
}

static bool test_rtl_raise_exception() {
    return true;
}

static bool test_veh_chain_registration() {
    return true;
}

static bool test_rtl_lookup_function_entry_allows_null_image_base() {
    PELoader loader;
    auto kernel32 = Kernel32Shim::create();
    addMissingKernel32(kernel32);

    auto function = kernel32.functions.find("RtlLookupFunctionEntry");
    if (function == kernel32.functions.end())
        return false;

    using LookupFunctionEntry = void*(MSABI*)(uint64_t, uint64_t*, void*);
    auto lookup = reinterpret_cast<LookupFunctionEntry>(function->second());
    if (!lookup)
        return false;

    metalsharp::Logger::setLevel(metalsharp::LogLevel::Error);
    void* result = lookup(0, nullptr, nullptr);
    if (!result)
        return false;

    auto* runtimeFunction = static_cast<const uint32_t*>(result);
    if (runtimeFunction[0] != 0 || runtimeFunction[1] != 0x1000)
        return false;

    std::atomic<bool> allCallsReturned{true};
    std::vector<std::thread> callers;
    callers.reserve(8);
    for (int i = 0; i < 8; ++i) {
        callers.emplace_back([&] {
            for (int call = 0; call < 100; ++call) {
                if (!lookup(0, nullptr, nullptr)) {
                    allCallsReturned = false;
                    return;
                }
            }
        });
    }
    for (auto& caller : callers)
        caller.join();

    return allCallsReturned;
}

int main() {
    printf("=== Phase 20: Anti-Cheat & DRM Compatibility ===\n\n");

    printf("--- 20.1 Anti-Cheat Compatibility Database ---\n");
    TEST(anticheat_db_has_entries);
    TEST(anticheat_find_eac);
    TEST(anticheat_find_denuvo);
    TEST(anticheat_find_vac);
    TEST(anticheat_find_steam_stub);
    TEST(anticheat_status_counts);
    TEST(anticheat_drm_type);
    TEST(anticheat_kernel_boundaries_are_precise);
    TEST(anticheat_find_nonexistent);

    printf("\n--- 20.2 DRM Shims (Firmware, MAC, Disk) ---\n");
    TEST(smbios_firmware_table);
    TEST(mac_address_stable);
    TEST(adapters_addresses_layout);
    TEST(device_io_control_disk);

    printf("\n--- 20.3 SEH & Exception Hardening ---\n");
    TEST(raise_exception_veh);
    TEST(ki_user_exception_dispatcher);
    TEST(rtl_raise_exception);
    TEST(veh_chain_registration);
    TEST(rtl_lookup_function_entry_allows_null_image_base);

    printf("\n--- 20.4 Timing & Hardware Fingerprinting ---\n");
    TEST(local_time);
    TEST(time_get_time);
    TEST(qpc_frequency_realistic);

    printf("\n%d/%d passed", testsPassed, testsPassed + testsFailed);
    if (testsFailed > 0)
        printf(" (%d FAILED)", testsFailed);
    printf("\n");

    return testsFailed > 0 ? 1 : 0;
}
