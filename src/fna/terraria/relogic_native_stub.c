/// @file relogic_native_stub.c
/// @brief Terraria's ReLogic.Native.dll stub for macOS.
///
/// Terraria's managed code (ReLogic.Localization.IME.Windows.NativeMethods)
/// P/Invokes the classic Windows IME UI API (ImeUi_*) out of ReLogic.Native.dll.
/// The game ships that DLL as a Windows PE, which mono cannot dlopen — the
/// terraria-mono.config dllmaps it to `libReLogic.Native.dylib`. This stub
/// exports the full ImeUi_* surface as inert no-ops: the IME is optional, and
/// returning "no IME" lets Terraria run without it (previously the game died
/// at Main.Initialize with DllNotFoundException: ReLogic.Native.dll).
#include <stddef.h>
#include <stdint.h>

typedef void* HWND;
typedef int32_t BOOL;

int32_t ImeUi_Initialize(HWND hwnd, BOOL bMultiThreaded) {
    (void)hwnd;
    (void)bMultiThreaded;
    return 0;
}

void ImeUi_Uninitialize(void) {}

void ImeUi_Enable(BOOL bEnable) {
    (void)bEnable;
}

BOOL ImeUi_IsEnabled(void) {
    return 0;
}

int32_t ImeUi_ProcessMessage(HWND hwnd, int32_t uMsg, void* wParam, void* lParam, void** plResult) {
    (void)hwnd;
    (void)uMsg;
    (void)wParam;
    (void)lParam;
    if (plResult) {
        *plResult = NULL;
    }
    return 0;
}

int32_t ImeUi_GetCompositionString(int32_t index, void* buf, int32_t bufLen) {
    (void)index;
    (void)buf;
    (void)bufLen;
    return 0;
}

int32_t ImeUi_GetCandidateCount(void) {
    return 0;
}

int32_t ImeUi_GetCandidate(int32_t index, void* buf, int32_t bufLen) {
    (void)index;
    (void)buf;
    (void)bufLen;
    return 0;
}

int32_t ImeUi_GetCandidateSelection(void) {
    return 0;
}

void ImeUi_SetCandidateSelection(int32_t selection) {
    (void)selection;
}

int32_t ImeUi_GetCandidatePageSize(void) {
    return 0;
}

BOOL ImeUi_IsCandidateListVisible(void) {
    return 0;
}

BOOL ImeUi_IsShowCandListWindow(void) {
    return 0;
}

void ImeUi_FinalizeString(void) {}

int32_t ImeUi_GetPrimaryLanguage(void) {
    return 0;
}

BOOL ImeUi_IgnoreHotKey(void) {
    return 0;
}

BOOL ImeUi_ShouldIgnoreHotKey(void) {
    return 0;
}

BOOL ImeUi_EnableIme(BOOL bEnable) {
    (void)bEnable;
    return 0;
}
