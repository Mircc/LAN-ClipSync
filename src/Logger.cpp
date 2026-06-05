#include "Logger.h"

#include <cstdio>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

namespace {

std::mutex g_logMutex;
wchar_t g_logPath[MAX_PATH]{};
bool g_initialized = false;

void ensureLogPath() {
    if (g_initialized) {
        return;
    }
    if (GetModuleFileNameW(nullptr, g_logPath, MAX_PATH) == 0) {
        wcscpy_s(g_logPath, L"LAN-ClipSync.log");
    } else {
        wchar_t* slash = wcsrchr(g_logPath, L'\\');
        if (!slash) {
            slash = wcsrchr(g_logPath, L'/');
        }
        if (slash) {
            slash[1] = L'\0';
            wcscat_s(g_logPath, L"LAN-ClipSync.log");
        } else {
            wcscpy_s(g_logPath, L"LAN-ClipSync.log");
        }
    }
    g_initialized = true;
}

}  // namespace

void Logger::init() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    ensureLogPath();
}

void Logger::write(const wchar_t* level, const std::wstring& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    ensureLogPath();

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t timeBuf[32]{};
    swprintf_s(timeBuf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath, L"a, ccs=UTF-8") == 0 && file) {
        fwprintf(file, L"%s [%s] %s\n", timeBuf, level, msg.c_str());
        fclose(file);
    }

    wchar_t debug[512]{};
    swprintf_s(debug, L"%s %s\n", level, msg.c_str());
    OutputDebugStringW(debug);
}

void Logger::info(const std::wstring& msg) { write(L"INFO", msg); }
void Logger::warn(const std::wstring& msg) { write(L"WARN", msg); }
void Logger::error(const std::wstring& msg) { write(L"ERROR", msg); }

}  // namespace clipsync
