#include <stdio.h>
#include <windows.h>

typedef char *(__cdecl *wine_get_unix_file_name_fn)(const wchar_t *path);

int main(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC raw = ntdll != NULL ? GetProcAddress(ntdll, "wine_get_unix_file_name") : NULL;
    FARPROC kernel_export = kernel32 != NULL ? GetProcAddress(kernel32, "wine_get_unix_file_name") : NULL;
    printf("ntdll=%p export=%p kernel32=%p kernel_export=%p\n", (void *)ntdll, (void *)raw, (void *)kernel32, (void *)kernel_export);
    if (raw == NULL && kernel_export != NULL) raw = kernel_export;
    if (raw == NULL) return 2;
    char *(*get_name)(const wchar_t *) = (wine_get_unix_file_name_fn)raw;
    char *name = get_name(L"/proc/40/maps");
    printf("maps=%s\n", name != NULL ? name : "<null>");
    if (name != NULL) HeapFree(GetProcessHeap(), 0, name);
    return name != NULL ? 0 : 3;
}
