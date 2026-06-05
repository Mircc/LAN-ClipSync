#include "ClipboardManager.h"

#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

ClipboardManager::ClipboardManager(HWND hwnd) : hwnd_(hwnd) {}

bool ClipboardManager::start() {
    if (!AddClipboardFormatListener(hwnd_)) {
        Logger::error(L"AddClipboardFormatListener 失败");
        return false;
    }
    Logger::info(L"剪切板监听已启动");
    return true;
}

void ClipboardManager::stop() {
    RemoveClipboardFormatListener(hwnd_);
}

void ClipboardManager::setOnTextChanged(TextChangedCallback cb) {
    onTextChanged_ = std::move(cb);
}

bool ClipboardManager::openClipboardWithRetry() {
    for (int i = 0; i < 3; ++i) {
        if (OpenClipboard(hwnd_)) {
            return true;
        }
        Sleep(50);
    }
    Logger::warn(L"OpenClipboard 失败（已重试 3 次）");
    return false;
}

bool ClipboardManager::isTextOnlyClipboard() const {
    if (IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIBV5)) {
        return false;
    }
    return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);
}

std::wstring ClipboardManager::readUnicodeText() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (!IsClipboardFormatAvailable(CF_TEXT)) {
            return {};
        }
        HANDLE data = GetClipboardData(CF_TEXT);
        if (!data) {
            return {};
        }
        const char* ansi = static_cast<const char*>(GlobalLock(data));
        if (!ansi) {
            return {};
        }
        const int wideLen = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(wideLen > 0 ? wideLen - 1 : 0), L'\0');
        if (wideLen > 0) {
            MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide.data(), wideLen);
        }
        GlobalUnlock(data);
        return wide;
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        return {};
    }
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) {
        return {};
    }
    std::wstring result(text);
    GlobalUnlock(data);
    return result;
}

bool ClipboardManager::writeUnicodeText(const std::wstring& text) {
    if (!EmptyClipboard()) {
        Logger::warn(L"EmptyClipboard 失败");
        return false;
    }

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        Logger::error(L"GlobalAlloc 失败");
        return false;
    }

    void* locked = GlobalLock(mem);
    if (!locked) {
        GlobalFree(mem);
        Logger::error(L"GlobalLock 失败");
        return false;
    }
    memcpy(locked, text.c_str(), bytes);
    GlobalUnlock(mem);

    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        Logger::error(L"SetClipboardData 失败");
        return false;
    }
    return true;
}

void ClipboardManager::onClipboardUpdate() {
    if (suppressEcho_) {
        suppressEcho_ = false;
        return;
    }

    if (!openClipboardWithRetry()) {
        return;
    }

    if (!isTextOnlyClipboard()) {
        CloseClipboard();
        Logger::info(L"忽略非纯文本剪切板内容");
        return;
    }

    std::wstring text = readUnicodeText();
    CloseClipboard();

    if (text.empty()) {
        return;
    }

    if (text == lastNetworkText_) {
        Logger::info(L"忽略与网络回显相同的剪切板变更");
        return;
    }

    if (onTextChanged_) {
        onTextChanged_(text);
    }
}

bool ClipboardManager::setFromNetwork(const std::wstring& text) {
    if (text == lastNetworkText_) {
        return true;
    }

    lastNetworkText_ = text;
    suppressEcho_ = true;

    if (!openClipboardWithRetry()) {
        suppressEcho_ = false;
        return false;
    }

    const bool ok = writeUnicodeText(text);
    CloseClipboard();

    if (!ok) {
        suppressEcho_ = false;
        Logger::error(L"写入网络剪切板失败");
        return false;
    }

    Logger::info(L"已写入来自网络的剪切板文本");
    return true;
}

}  // namespace clipsync
