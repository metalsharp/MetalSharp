#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t DWORD;
typedef int32_t BOOL;
typedef uint16_t WORD;
typedef void* HANDLE;
typedef const char* LPCSTR;

BOOL ShowWindow(HANDLE hWnd, int nCmdShow) {
    (void)hWnd;
    (void)nCmdShow;
    return 1;
}

int MessageBoxA(HANDLE hWnd, LPCSTR lpText, LPCSTR lpCaption, DWORD uType) {
    (void)hWnd;
    (void)uType;
    fprintf(stderr, "[%s] %s\n", lpCaption ? lpCaption : "MetalSharp", lpText ? lpText : "");
    return 1;
}

int MessageBoxW(HANDLE hWnd, const void* lpText, const void* lpCaption, DWORD uType) {
    (void)hWnd;
    (void)lpText;
    (void)lpCaption;
    (void)uType;
    return 1;
}

int GetWindowTextA(HANDLE hWnd, char* lpString, int nMaxCount) {
    (void)hWnd;
    if (lpString && nMaxCount > 0)
        lpString[0] = '\0';
    return 0;
}

int GetWindowTextLengthA(HANDLE hWnd) {
    (void)hWnd;
    return 0;
}

BOOL SetWindowTextA(HANDLE hWnd, LPCSTR lpString) {
    (void)hWnd;
    (void)lpString;
    return 1;
}

HANDLE GetDC(HANDLE hWnd) {
    (void)hWnd;
    return (HANDLE)(intptr_t)1;
}

int ReleaseDC(HANDLE hWnd, HANDLE hDC) {
    (void)hWnd;
    (void)hDC;
    return 1;
}

HANDLE GetForegroundWindow(void) {
    return (HANDLE)(intptr_t)1;
}

BOOL SetForegroundWindow(HANDLE hWnd) {
    (void)hWnd;
    return 1;
}

HANDLE FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName) {
    (void)lpClassName;
    (void)lpWindowName;
    return NULL;
}

BOOL PostMessageA(HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return 1;
}

void* SendMessageA(HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return NULL;
}

int16_t GetKeyState(int nVirtKey) {
    (void)nVirtKey;
    return 0;
}

int16_t GetAsyncKeyState(int vKey) {
    (void)vKey;
    return 0;
}

BOOL SetRect(void* lprc, int xLeft, int yTop, int xRight, int yBottom) {
    (void)lprc;
    (void)xLeft;
    (void)yTop;
    (void)xRight;
    (void)yBottom;
    return 1;
}

BOOL AdjustWindowRect(void* lpRect, DWORD dwStyle, BOOL bMenu) {
    (void)lpRect;
    (void)dwStyle;
    (void)bMenu;
    return 1;
}

BOOL SetWindowPos(HANDLE hWnd, HANDLE hWndInsertAfter, int X, int Y, int cx, int cy, DWORD uFlags) {
    (void)hWnd;
    (void)hWndInsertAfter;
    (void)X;
    (void)Y;
    (void)cx;
    (void)cy;
    (void)uFlags;
    return 1;
}

BOOL GetWindowRect(HANDLE hWnd, void* lpRect) {
    (void)hWnd;
    (void)lpRect;
    return 1;
}

BOOL GetClientRect(HANDLE hWnd, void* lpRect) {
    (void)hWnd;
    (void)lpRect;
    return 1;
}

BOOL MoveWindow(HANDLE hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint) {
    (void)hWnd;
    (void)X;
    (void)Y;
    (void)nWidth;
    (void)nHeight;
    (void)bRepaint;
    return 1;
}

BOOL IsWindow(HANDLE hWnd) {
    (void)hWnd;
    return 0;
}

BOOL IsWindowVisible(HANDLE hWnd) {
    (void)hWnd;
    return 1;
}

BOOL EnableWindow(HANDLE hWnd, BOOL bEnable) {
    (void)hWnd;
    (void)bEnable;
    return 1;
}

HANDLE SetFocus(HANDLE hWnd) {
    (void)hWnd;
    return NULL;
}

HANDLE GetFocus(void) {
    return NULL;
}

BOOL PeekMessageA(void* lpMsg, HANDLE hWnd, DWORD wMsgFilterMin, DWORD wMsgFilterMax, DWORD wRemoveMsg) {
    (void)lpMsg;
    (void)hWnd;
    (void)wMsgFilterMin;
    (void)wMsgFilterMax;
    (void)wRemoveMsg;
    return 0;
}

BOOL GetMessageA(void* lpMsg, HANDLE hWnd, DWORD wMsgFilterMin, DWORD wMsgFilterMax) {
    (void)lpMsg;
    (void)hWnd;
    (void)wMsgFilterMin;
    (void)wMsgFilterMax;
    return 0;
}

void TranslateMessage(const void* lpMsg) {
    (void)lpMsg;
}

void* DispatchMessageA(const void* lpMsg) {
    (void)lpMsg;
    return NULL;
}

void* DefWindowProcA(HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return NULL;
}

// Mono resolves the unsuffixed symbol on macOS dllmap targets (Terraria's
// ReLogic WindowService takes a DefWindowProc delegate via
// Marshal.GetFunctionPointerForDelegate — EntryPointNotFoundException
// "DefWindowProc" without the A/W variants present).
void* DefWindowProc(HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return NULL;
}

void* DefWindowProcW(HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return NULL;
}

// Terraria's ReLogic NativeMethods surface (merged into Terraria.exe):
// SetWindowLong / GetWindowLong / CallWindowProc / GetParent — with the
// Ptr and A/W variants mono may resolve depending on the DllImport charset.
void* CallWindowProc(void* prevWndFunc, HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)prevWndFunc;
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return NULL;
}

void* CallWindowProcA(void* prevWndFunc, HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)prevWndFunc;
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return NULL;
}

void* CallWindowProcW(void* prevWndFunc, HANDLE hWnd, DWORD Msg, void* wParam, void* lParam) {
    (void)prevWndFunc;
    (void)hWnd;
    (void)Msg;
    (void)wParam;
    (void)lParam;
    return NULL;
}

void* SetWindowLong(HANDLE hWnd, int nIndex, void* dwNewLong) {
    (void)hWnd;
    (void)nIndex;
    (void)dwNewLong;
    return NULL;
}

void* SetWindowLongA(HANDLE hWnd, int nIndex, void* dwNewLong) {
    (void)hWnd;
    (void)nIndex;
    (void)dwNewLong;
    return NULL;
}

void* SetWindowLongW(HANDLE hWnd, int nIndex, void* dwNewLong) {
    (void)hWnd;
    (void)nIndex;
    (void)dwNewLong;
    return NULL;
}

void* SetWindowLongPtr(HANDLE hWnd, int nIndex, void* dwNewLong) {
    (void)hWnd;
    (void)nIndex;
    (void)dwNewLong;
    return NULL;
}

void* SetWindowLongPtrA(HANDLE hWnd, int nIndex, void* dwNewLong) {
    (void)hWnd;
    (void)nIndex;
    (void)dwNewLong;
    return NULL;
}

void* SetWindowLongPtrW(HANDLE hWnd, int nIndex, void* dwNewLong) {
    (void)hWnd;
    (void)nIndex;
    (void)dwNewLong;
    return NULL;
}

void* GetWindowLong(HANDLE hWnd, int nIndex) {
    (void)hWnd;
    (void)nIndex;
    return NULL;
}

void* GetWindowLongA(HANDLE hWnd, int nIndex) {
    (void)hWnd;
    (void)nIndex;
    return NULL;
}

void* GetWindowLongW(HANDLE hWnd, int nIndex) {
    (void)hWnd;
    (void)nIndex;
    return NULL;
}

void* GetWindowLongPtr(HANDLE hWnd, int nIndex) {
    (void)hWnd;
    (void)nIndex;
    return NULL;
}

void* GetWindowLongPtrA(HANDLE hWnd, int nIndex) {
    (void)hWnd;
    (void)nIndex;
    return NULL;
}

void* GetWindowLongPtrW(HANDLE hWnd, int nIndex) {
    (void)hWnd;
    (void)nIndex;
    return NULL;
}

HANDLE GetParent(HANDLE hWnd) {
    (void)hWnd;
    return NULL;
}
