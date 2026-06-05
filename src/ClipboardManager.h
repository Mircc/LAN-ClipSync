#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <string>

namespace clipsync {

class ClipboardManager {
public:
    using TextChangedCallback = std::function<void(const std::wstring& text)>;

    explicit ClipboardManager(HWND hwnd);

    bool start();
    void stop();

    void onClipboardUpdate();

    bool setFromNetwork(const std::wstring& text);

    void setOnTextChanged(TextChangedCallback cb);

private:
    bool openClipboardWithRetry();
    bool isTextOnlyClipboard() const;
    std::wstring readUnicodeText();
    bool writeUnicodeText(const std::wstring& text);

    HWND hwnd_;
    TextChangedCallback onTextChanged_;

    std::wstring lastNetworkText_;
    bool suppressEcho_ = false;
};

}  // namespace clipsync
