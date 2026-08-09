#include <windows.h>
#include <stdio.h>
#include <string.h>

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR cmd, int show) {
    char ep[MAX_PATH];
    GetModuleFileNameA(NULL, ep, MAX_PATH);
    char *sl = strrchr(ep, '\\');
    if (!sl) return 1;
    *sl = 0;
    char re[MAX_PATH];
    snprintf(re, MAX_PATH, "%s\\steamwebhelper_real.exe", ep);
    if (GetFileAttributesA(re) == INVALID_FILE_ATTRIBUTES) return 1;
    char cl[8192];
    if (strlen(cmd) > 0)
        snprintf(cl, sizeof(cl), "\"%s\" %s --disable-gpu", re, cmd);
    else
        snprintf(cl, sizeof(cl), "\"%s\" --disable-gpu", re);
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(re, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)ec;
}
