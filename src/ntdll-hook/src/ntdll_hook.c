#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* === MetalFX Spatial Upscaling live toggle (both arches) ===========
 *
 * The overlay writes <METALSHARP_HOME>/etc/metalfx.overlay.json
 * ("{"enabled":bool,"factor":num,"ts":u64}") via the MetalSharp backend.
 * This poller, resident in the Wine game process, applies the on/off to the
 * process environment with SetEnvironmentVariableW so DXMT picks it up at the
 * next CreateSwapChain (game-initiated swapchain recreate / alt-enter).
 * The factor is not live-reloadable (DXMT Config is a cached singleton); it
 * is written to dxmt.conf by the backend for the next launch.
 */

static volatile LONG g_metalfx_last_ts = 0;
static volatile LONG g_metalfx_stop = 0;

static void metalfx_build_state_path(LPWSTR out, int cap) {
    out[0] = L'\0';
    wchar_t home[1024];
    DWORD n = GetEnvironmentVariableW(L"METALSHARP_HOME", home, 1024);
    if (n == 0 || n >= 1024)
        return;
    /* METALSHARP_HOME is a unix path (/Users/x/.metalsharp); map to Z:\... */
    int wrote = _snwprintf(out, cap, L"Z:");
    for (DWORD i = 0; i < n && wrote < cap - 24; i++) {
        wchar_t c = home[i];
        if (c == L'/')
            c = L'\\';
        out[wrote++] = c;
    }
    if (wrote < cap - 24) {
        out[wrote] = L'\0';
        wcscat_s(out, cap, L"\\etc\\metalfx.overlay.json");
    } else {
        out[0] = L'\0';
    }
}

static int metalfx_read_file(const wchar_t* path, char* buf, int bufsz) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, bufsz - 1, &got, NULL);
    CloseHandle(h);
    if (!ok)
        return -1;
    buf[got] = '\0';
    return (int)got;
}

/* Minimal JSON field extractors (no parser dep). */
static int metalfx_json_bool(const char* s, const char* key, int def) {
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(s, pat);
    if (!p)
        return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (!strncmp(p, "true", 4))
        return 1;
    if (!strncmp(p, "false", 5))
        return 0;
    return def;
}
static unsigned long metalfx_json_u64(const char* s, const char* key, unsigned long def) {
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(s, pat);
    if (!p)
        return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    return strtoul(p, NULL, 10);
}

static DWORD WINAPI metalfx_poller_thread(LPVOID unused) {
    (void)unused;
    char buf[512];
    wchar_t path[1200];
    /* backoff poll: 500ms while a game is likely running; cheap file stat. */
    for (; !g_metalfx_stop; Sleep(500)) {
        metalfx_build_state_path(path, 1200);
        if (!path[0])
            continue;
        if (metalfx_read_file(path, buf, sizeof(buf)) < 0)
            continue;
        unsigned long ts = metalfx_json_u64(buf, "ts", 0);
        if (ts == 0)
            continue;
        if ((LONG)ts == g_metalfx_last_ts)
            continue; /* unchanged */
        int enabled = metalfx_json_bool(buf, "enabled", -1);
        if (enabled < 0)
            continue;
        SetEnvironmentVariableW(L"DXMT_METALFX_SPATIAL_SWAPCHAIN", enabled ? L"1" : L"0");
        g_metalfx_last_ts = (LONG)ts;
        OutputDebugStringA(enabled ? "[metalsharp_ntdll_hook] MetalFX spatial enabled (next swapchain recreate)"
                                   : "[metalsharp_ntdll_hook] MetalFX spatial disabled (next swapchain recreate)");
    }
    return 0;
}

static void metalfx_init(void) {
    HANDLE t = CreateThread(NULL, 0, metalfx_poller_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

/* Both arches: the MetalFX poller runs so the overlay can toggle upscaling
 * live (applies on swapchain recreate). The DLL installs no Nt* hooks and
 * opens no sockets. */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        metalfx_init();
    }
    return TRUE;
}
