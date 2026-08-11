/// @file DRMShims.cpp
/// @brief Anti-DRM detection shims for volume serial, MAC address, and timing.
///
/// Spoofs volume serial numbers, MAC addresses, and system timing to bypass DRM checks that validate hardware
/// fingerprints. Also provides anti-debug detection shims to hide the Wine environment from copy protection.
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <metalsharp/Logger.h>
#include <metalsharp/PELoader.h>
#include <metalsharp/Win32Types.h>

#ifdef __APPLE__
#include <ifaddrs.h>
#include <mach/mach_time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#endif

namespace metalsharp {
namespace win32 {

static const uint8_t STABLE_MAC[6] = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
static const uint32_t STABLE_DISK_SERIAL = 0xABCDEF12;
static char STABLE_ADAPTER_NAME[] = "eth0";
static uint16_t STABLE_ADAPTER_DESCRIPTION[] = {
    'M', 'e', 't', 'a', 'l', 'S', 'h', 'a', 'r', 'p', ' ', 'V', 'i', 'r',
    't', 'u', 'a', 'l', ' ', 'E', 't', 'h', 'e', 'r', 'n', 'e', 't', 0,
};

// IP_ADAPTER_ADDRESSES_LH's x86_64 prefix through Ipv6IfIndex.  Keep this
// layout explicit because the guest reads these fields through Windows ABI
// pointers, not as an inline buffer.  The remaining linked-list fields are
// intentionally null for this single synthetic adapter.
struct IP_ADAPTER_ADDRESSES_LAYOUT {
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

static_assert(sizeof(IP_ADAPTER_ADDRESSES_LAYOUT) == 112);
static_assert(offsetof(IP_ADAPTER_ADDRESSES_LAYOUT, AdapterName) == 16);
static_assert(offsetof(IP_ADAPTER_ADDRESSES_LAYOUT, PhysicalAddress) == 80);
static_assert(offsetof(IP_ADAPTER_ADDRESSES_LAYOUT, PhysicalAddressLength) == 88);
static_assert(offsetof(IP_ADAPTER_ADDRESSES_LAYOUT, IfType) == 100);
static_assert(offsetof(IP_ADAPTER_ADDRESSES_LAYOUT, OperStatus) == 104);
static_assert(offsetof(IP_ADAPTER_ADDRESSES_LAYOUT, OperStatus) + sizeof(uint32_t) <=
              sizeof(IP_ADAPTER_ADDRESSES_LAYOUT));

static BOOL MSABI shim_GetSystemFirmwareTable(DWORD FirmwareTableProviderSignature, DWORD FirmwareTableID,
                                              void* pFirmwareTableBuffer, DWORD BufferSize) {

    if (BufferSize == 0 && pFirmwareTableBuffer == nullptr) {
        return 0;
    }

    if (FirmwareTableProviderSignature == 0x52534D42) {
        if (FirmwareTableID == 0) {
            return 0;
        }

        struct RawSMBIOSData {
            uint8_t Used20CallingMethod;
            uint8_t SMBIOSMajorVersion;
            uint8_t SMBIOSMinorVersion;
            uint8_t DmiRevision;
            uint32_t Length;
            uint8_t SMBIOSTableData[1];
        };

        static const uint8_t fakeSMBIOS[] = {
            0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x02, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x17, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 'A',  'p',  'p',  'l',  'e',  ' ',  'I',  'n',  'c',  '.',  '\0',
            'M',  'a',  'c',  'B',  'o',  'o',  'k',  ' ',  'P',  'r',  'o',  '\0', '\0',
        };

        uint32_t totalSize = sizeof(RawSMBIOSData) - 1 + sizeof(fakeSMBIOS);

        if (BufferSize == 0 || pFirmwareTableBuffer == nullptr) {
            return totalSize;
        }

        DWORD copySize = (BufferSize < totalSize) ? BufferSize : totalSize;

        auto* out = static_cast<uint8_t*>(pFirmwareTableBuffer);
        out[0] = 0;
        out[1] = 3;
        out[2] = 4;
        out[3] = 0;
        uint32_t dataLen = sizeof(fakeSMBIOS);
        memcpy(out + 4, &dataLen, 4);
        memcpy(out + 8, fakeSMBIOS, copySize > 8 ? copySize - 8 : 0);

        return copySize;
    }

    return 0;
}

static uint32_t MSABI shim_GetAdaptersInfo(void* pAdapterInfo, uint32_t* pOutBufLen) {
    if (!pOutBufLen)
        return 111;

    struct IP_ADAPTER_INFO_LAYOUT {
        void* Next;
        uint32_t ComboIndex;
        char AdapterName[260];
        char Description[132];
        uint32_t AddressLength;
        uint8_t Address[8];
        uint32_t Index;
        uint32_t Type;
        uint32_t DhcpEnabled;
        void* CurrentIpAddress;
        uint8_t IpAddressList[64];
        uint8_t GatewayList[64];
        uint8_t DhcpServer[64];
        uint8_t HaveWins;
        uint8_t PrimaryWinsServer[64];
        uint8_t SecondaryWinsServer[64];
        uint32_t LeaseObtained;
        uint32_t LeaseExpires;
    };

    uint32_t needed = sizeof(IP_ADAPTER_INFO_LAYOUT);
    if (!pAdapterInfo || *pOutBufLen < needed) {
        *pOutBufLen = needed;
        return 111;
    }

    memset(pAdapterInfo, 0, needed);
    auto* adapter = static_cast<IP_ADAPTER_INFO_LAYOUT*>(pAdapterInfo);
    adapter->ComboIndex = 0;
    memcpy(adapter->AdapterName, "eth0\0", 5);
    memcpy(adapter->Description, "MetalSharp Virtual Ethernet\0", 28);
    adapter->AddressLength = 6;
    memcpy(adapter->Address, STABLE_MAC, 6);
    adapter->Index = 1;
    adapter->Type = 6;

    return 0;
}

static uint32_t MSABI shim_GetAdaptersAddresses(uint32_t Family, uint32_t Flags, void* Reserved,
                                                void* pAdapterAddresses, uint32_t* pOutBufLen) {
    (void)Family;
    (void)Flags;

    if (!pOutBufLen)
        return 111;
    if (Reserved)
        return 87;

    constexpr uint32_t needed = static_cast<uint32_t>(sizeof(IP_ADAPTER_ADDRESSES_LAYOUT));
    if (!pAdapterAddresses || *pOutBufLen < needed) {
        *pOutBufLen = needed;
        return 111;
    }

    assert(needed <= *pOutBufLen);
    memset(pAdapterAddresses, 0, needed);
    auto* adapter = static_cast<IP_ADAPTER_ADDRESSES_LAYOUT*>(pAdapterAddresses);

    adapter->Length = needed;
    adapter->IfIndex = 1;
    adapter->AdapterName = STABLE_ADAPTER_NAME;
    adapter->Description = STABLE_ADAPTER_DESCRIPTION;
    adapter->FriendlyName = STABLE_ADAPTER_DESCRIPTION;
    memcpy(adapter->PhysicalAddress, STABLE_MAC, sizeof(STABLE_MAC));
    adapter->PhysicalAddressLength = sizeof(STABLE_MAC);
    adapter->Mtu = 1500;
    adapter->IfType = 6;
    adapter->OperStatus = 1;

    return 0;
}

static void MSABI shim_GetLocalTime(void* lpSystemTime) {
    if (!lpSystemTime)
        return;

    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm localTime;
    localtime_r(&t, &localTime);

    auto* st = reinterpret_cast<uint16_t*>(lpSystemTime);
    st[0] = static_cast<uint16_t>(localTime.tm_year + 1900);
    st[1] = static_cast<uint16_t>(localTime.tm_mon + 1);
    st[2] = static_cast<uint16_t>(localTime.tm_wday);
    st[3] = static_cast<uint16_t>(localTime.tm_mday);
    st[4] = static_cast<uint16_t>(localTime.tm_hour);
    st[5] = static_cast<uint16_t>(localTime.tm_min);
    st[6] = static_cast<uint16_t>(localTime.tm_sec);
    st[7] = 0;
}

static DWORD MSABI shim_timeGetTime() {
    static auto startTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
    return static_cast<DWORD>(ms.count());
}

static DWORD MSABI shim_timeBeginPeriod(UINT) {
    return 0;
}
static DWORD MSABI shim_timeEndPeriod(UINT) {
    return 0;
}

static BOOL MSABI shim_DeviceIoControl_DRM(HANDLE hDevice, DWORD dwIoControlCode, void* lpInBuffer, DWORD nInBufferSize,
                                           void* lpOutBuffer, DWORD nOutBufferSize, DWORD* lpBytesReturned,
                                           void* lpOverlapped) {

    if (lpBytesReturned)
        *lpBytesReturned = 0;

    switch (dwIoControlCode) {
    case 0x00074008:
    case 0x0007C028:
    case 0x0004D014: {
        if (lpOutBuffer && nOutBufferSize >= 4) {
            uint32_t serial = STABLE_DISK_SERIAL;
            memcpy(lpOutBuffer, &serial, 4);
            if (lpBytesReturned)
                *lpBytesReturned = 4;
        }
        return 1;
    }
    case 0x0007404C:
    case 0x00074060: {
        if (lpOutBuffer && nOutBufferSize >= 512) {
            memset(lpOutBuffer, 0, 512);
            auto* out = static_cast<uint8_t*>(lpOutBuffer);
            memcpy(out, "MetalSharp", 10);
            out[20] = 0x01;
            out[21] = 0x00;
            out[22] = 0x01;
            out[23] = 0x00;
            if (lpBytesReturned)
                *lpBytesReturned = 512;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static void MSABI shim_RaiseException_Fixed(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
                                            void* lpArguments) {
    (void)dwExceptionFlags;
    (void)nNumberOfArguments;
    (void)lpArguments;
    MS_INFO("PELoader: RaiseException(0x%08X) — dispatching to VEH chain", dwExceptionCode);

    struct FakeExceptionRecord {
        uint32_t ExceptionCode;
        uint32_t ExceptionFlags;
        void* ExceptionRecord;
        void* ExceptionAddress;
        uint32_t NumberParameters;
        void* ExceptionInformation[15];
    } record = {};

    record.ExceptionCode = dwExceptionCode;
    record.ExceptionFlags = dwExceptionFlags;
    record.NumberParameters = nNumberOfArguments > 15 ? 15 : nNumberOfArguments;

    extern std::vector<std::pair<void*, bool>> s_vehHandlers;
    extern std::mutex s_vehMutex;
    extern void* s_unhandledExceptionFilter;

    {
        std::lock_guard<std::mutex> lock(s_vehMutex);
        for (auto& [handler, isFirst] : s_vehHandlers) {
            if (handler) {
                struct FakePointers {
                    FakeExceptionRecord* ExceptionRecord;
                    void* ContextRecord;
                } pointers = {&record, nullptr};

                typedef int32_t (*VEHHandler)(void*);
                auto veh = reinterpret_cast<VEHHandler>(handler);
                int32_t result = veh(&pointers);
                if (result == -1) {
                    MS_INFO("PELoader: VEH handler %p handled exception 0x%08X", handler, dwExceptionCode);
                    return;
                }
            }
        }
    }

    if (s_unhandledExceptionFilter) {
        struct FakePointers {
            void* ExceptionRecord;
            void* ContextRecord;
        } pointers = {&record, nullptr};

        typedef void* (*FilterFunc)(void*);
        auto filter = reinterpret_cast<FilterFunc>(s_unhandledExceptionFilter);
        void* action = filter(&pointers);
        (void)action;
        return;
    }

    MS_WARN("PELoader: Unhandled exception 0x%08X — no VEH or filter handled it", dwExceptionCode);
}

void addDRMShims(ShimLibrary& kernel32, ShimLibrary& winmm) {
    auto fn = [](void* ptr) -> ExportedFunction { return [ptr]() -> void* { return ptr; }; };

    kernel32.functions["GetSystemFirmwareTable"] = fn((void*)shim_GetSystemFirmwareTable);
    kernel32.functions["GetAdaptersInfo"] = fn((void*)shim_GetAdaptersInfo);
    kernel32.functions["GetAdaptersAddresses"] = fn((void*)shim_GetAdaptersAddresses);
    kernel32.functions["GetLocalTime"] = fn((void*)shim_GetLocalTime);
    kernel32.functions["DeviceIoControl"] = fn((void*)shim_DeviceIoControl_DRM);

    winmm.name = "winmm.dll";
    winmm.functions["timeGetTime"] = fn((void*)shim_timeGetTime);
    winmm.functions["timeBeginPeriod"] = fn((void*)shim_timeBeginPeriod);
    winmm.functions["timeEndPeriod"] = fn((void*)shim_timeEndPeriod);
    winmm.functions["timeGetDevCaps"] = fn((void*)nullptr);
    winmm.functions["joyGetNumDevs"] = fn((void*)nullptr);
    winmm.functions["PlaySoundA"] = fn((void*)nullptr);
    winmm.functions["PlaySoundW"] = fn((void*)nullptr);
    winmm.functions["sndPlaySoundA"] = fn((void*)nullptr);
    winmm.functions["sndPlaySoundW"] = fn((void*)nullptr);
    winmm.functions["mciSendCommandA"] = fn((void*)nullptr);
    winmm.functions["mciSendStringA"] = fn((void*)nullptr);
    winmm.functions["midiOutOpen"] = fn((void*)nullptr);
    winmm.functions["midiOutClose"] = fn((void*)nullptr);
    winmm.functions["midiOutShortMsg"] = fn((void*)nullptr);
    winmm.functions["waveOutOpen"] = fn((void*)nullptr);
    winmm.functions["waveOutClose"] = fn((void*)nullptr);
    winmm.functions["waveOutWrite"] = fn((void*)nullptr);
    winmm.functions["waveOutPrepareHeader"] = fn((void*)nullptr);
    winmm.functions["waveOutUnprepareHeader"] = fn((void*)nullptr);
}

} // namespace win32
} // namespace metalsharp
