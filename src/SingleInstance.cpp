#include "SingleInstance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

namespace {

constexpr wchar_t kInstanceMutexName[] = L"Local\\LAN-ClipSync.SingleInstance";
constexpr wchar_t kMainWindowClass[] = L"LANClipSyncHiddenWindow";
constexpr wchar_t kMainWindowTitle[] = L"LAN-ClipSync";

HANDLE g_instanceMutex = nullptr;

}  // namespace

bool acquireSingleInstance() {
    g_instanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (!g_instanceMutex) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return false;
    }
    return true;
}

void activateExistingInstance() {
    if (HWND hwnd = FindWindowW(kMainWindowClass, kMainWindowTitle)) {
        PostMessageW(hwnd, kSingleInstanceActivateMsg, 0, 0);
    }
}

}  // namespace clipsync
