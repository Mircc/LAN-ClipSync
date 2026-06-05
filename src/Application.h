#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "ClipboardManager.h"
#include "NetworkManager.h"

#include <memory>
#include <string>

namespace clipsync {

class Application {
public:
    Application();
    ~Application();

    int run(HINSTANCE instance);

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool createHiddenWindow(HINSTANCE instance);
    bool createTrayIcon();
    void destroyTrayIcon();
    void updateTrayTooltip();
    void showFirewallHint();
    void showPeerSettings();
    void showQrDialog();
    void updateConnectionStatus();

    void onLocalClipboardChanged(const std::wstring& text);
    void onNetworkTextReceived(const std::string& utf8Text);
    void onPeersChanged(size_t count);

    static constexpr UINT kTrayCallbackMessage = WM_APP + 1;
    static constexpr UINT kWmPeerStatus = WM_APP + 2;
    static constexpr UINT kTrayIconId = 1;
    static constexpr UINT kMenuPeerSettings = 1;
    static constexpr UINT kMenuQrCode = 2;
    static constexpr UINT kMenuRefreshStatus = 3;
    static constexpr UINT kMenuExit = 4;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW tray_{};

    std::unique_ptr<ClipboardManager> clipboard_;
    std::unique_ptr<NetworkManager> network_;

    std::wstring statusText_ = L"LAN-ClipSync 已启动";
};

}  // namespace clipsync
