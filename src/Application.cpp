#include "Application.h"

#include "JsonUtil.h"
#include "Logger.h"
#include "PeerConfig.h"
#include "PeerSettingsDialog.h"
#include "QrDialog.h"
#include "UiTheme.h"
#include "SingleInstance.h"
#include "resource.h"

#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shellapi.h>
#include <windows.h>

namespace clipsync {

namespace {

constexpr wchar_t kWindowClassName[] = L"LANClipSyncHiddenWindow";

Application* g_app = nullptr;

HICON loadAppIcon(HINSTANCE instance, int sizePx = 0) {
    const int cx = sizePx > 0 ? sizePx : GetSystemMetrics(SM_CXICON);
    const int cy = sizePx > 0 ? sizePx : GetSystemMetrics(SM_CYICON);
    return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, cx,
                                         cy, LR_SHARED));
}

HICON loadTrayIcon(HINSTANCE instance) {
    const int size = GetSystemMetrics(SM_CXSMICON);
    return loadAppIcon(instance, size > 0 ? size : 16);
}

}  // namespace

Application::Application() = default;
Application::~Application() { destroyTrayIcon(); }

bool Application::createHiddenWindow(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Application::windowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = loadAppIcon(instance_);
    if (!RegisterClassExW(&wc)) {
        Logger::error(L"RegisterClassEx 失败");
        return false;
    }

    hwnd_ = CreateWindowExW(0, kWindowClassName, L"LAN-ClipSync", 0, 0, 0, 0, 0, HWND_MESSAGE,
                              nullptr, instance_, nullptr);
    if (!hwnd_) {
        Logger::error(L"CreateWindowEx 失败");
        return false;
    }
    return true;
}

bool Application::createTrayIcon() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd_;
    tray_.uID = kTrayIconId;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = kTrayCallbackMessage;
    tray_.hIcon = loadTrayIcon(instance_);
    wcscpy_s(tray_.szTip, statusText_.c_str());

    if (!Shell_NotifyIconW(NIM_ADD, &tray_)) {
        Logger::error(L"创建托盘图标失败");
        return false;
    }
    return true;
}

void Application::destroyTrayIcon() {
    if (hwnd_) {
        tray_.cbSize = sizeof(tray_);
        Shell_NotifyIconW(NIM_DELETE, &tray_);
    }
}

void Application::updateTrayTooltip() {
    tray_.cbSize = sizeof(tray_);
    tray_.uFlags = NIF_TIP | NIF_ICON;
    tray_.hIcon = loadTrayIcon(instance_);
    wcscpy_s(tray_.szTip, statusText_.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

void Application::showFirewallHint() {
    constexpr wchar_t kRegKey[] = L"Software\\LAN-ClipSync";
    constexpr wchar_t kRegValue[] = L"FirewallHintShown";

    HKEY key = nullptr;
    DWORD shown = 0;
    DWORD size = sizeof(shown);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, kRegValue, nullptr, nullptr, reinterpret_cast<LPBYTE>(&shown), &size);
        RegCloseKey(key);
        if (shown != 0) {
            return;
        }
    }

    MessageBoxW(hwnd_,
                L"LAN-ClipSync 使用 TCP 点对点同步剪切板（无广播）。\n"
                L"请在防火墙中允许本程序的入站/出站 TCP 连接（专用网络）。\n"
                L"跨 VLAN 时由双方手动填写对方 IP:端口。\n\n"
                L"本程序不会访问互联网。",
                L"防火墙提示", MB_OK | MB_ICONINFORMATION);

    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        shown = 1;
        RegSetValueExW(key, kRegValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&shown),
                       sizeof(shown));
        RegCloseKey(key);
    }
}

void Application::showPeerSettings() {
    if (network_) {
        PeerSettingsDialog::show(hwnd_, network_.get());
        updateConnectionStatus();
    }
}

void Application::showQrDialog() {
    QrDialog::show(hwnd_);
}

void Application::onLocalClipboardChanged(const std::wstring& text) {
    if (!network_) {
        return;
    }
    const std::string utf8 = wideToUtf8(text);
    network_->broadcastText(utf8);
}

void Application::onNetworkTextReceived(const std::string& utf8Text) {
    if (!clipboard_) {
        return;
    }
    clipboard_->setFromNetwork(utf8ToWide(utf8Text));
}

void Application::onPeersChanged(size_t) { updateConnectionStatus(); }

void Application::updateConnectionStatus() {
    if (!network_) {
        statusText_ = L"LAN-ClipSync";
    } else {
        const size_t total = network_->peerCount();
        const size_t online = network_->onlinePeerCount();
        if (total == 0) {
            statusText_ = L"LAN-ClipSync — 未配置对端";
        } else if (online == total) {
            statusText_ = L"LAN-ClipSync — ● " + std::to_wstring(online) + L"/" +
                          std::to_wstring(total) + L" 已连接";
        } else if (online == 0) {
            statusText_ = L"LAN-ClipSync — ○ 未连接 (" + std::to_wstring(total) + L" 个对端)";
        } else {
            statusText_ = L"LAN-ClipSync — ● " + std::to_wstring(online) + L"/" +
                          std::to_wstring(total) + L" 在线";
        }
    }
    updateTrayTooltip();
}

int Application::run(HINSTANCE instance) {
    g_app = this;
    Logger::init();

    if (!createHiddenWindow(instance)) {
        return 1;
    }

    clipboard_ = std::make_unique<ClipboardManager>(hwnd_);
    network_ = std::make_unique<NetworkManager>();

    clipboard_->setOnTextChanged([this](const std::wstring& text) {
        onLocalClipboardChanged(text);
    });
    network_->setOnTextReceived([this](const std::string& utf8) {
        onNetworkTextReceived(utf8);
    });
    network_->setOnPeersChanged([this](size_t count) { onPeersChanged(count); });
    network_->setOnPeerStatusChanged([this]() {
        if (hwnd_) {
            PostMessageW(hwnd_, kWmPeerStatus, 0, 0);
        }
    });

    if (!network_->start() || !clipboard_->start()) {
        MessageBoxW(hwnd_, L"LAN-ClipSync 启动失败，请查看日志文件。", L"错误",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!createTrayIcon()) {
        return 1;
    }

    network_->runProbeAndNotify();
    updateConnectionStatus();
    showFirewallHint();

    if (network_->peerCount() > 0) {
        if (peerConfigFileExists() || isPeerSetupCompleted()) {
            markPeerSetupCompleted();
        }
        std::wstringstream ss;
        ss << L"已加载 " << network_->peerCount() << L" 个对端";
        if (network_->listenPortReusedFromConfig()) {
            ss << L"，复用监听端口 " << network_->listenPort();
        }
        Logger::info(ss.str());
    } else if (!isPeerSetupCompleted() && !peerConfigFileExists()) {
        MessageBoxW(hwnd_,
                    L"首次使用请配置对端。\n\n"
                    L"配置将保存到程序目录，下次启动会自动加载，无需重复添加。\n"
                    L"监听端口若未被占用也会自动复用。",
                    L"配置对端", MB_OK | MB_ICONINFORMATION);
        showPeerSettings();
    }

    if (network_->listenPortChangedFromSaved()) {
        const std::wstring addr = formatLocalListenAddresses(
            network_->localIPv4(), network_->localIPv6(), localComputerNameUtf8(),
            network_->listenPort());
        std::wstring msg = L"上次保存的端口 " + std::to_wstring(network_->savedListenPort()) +
                           L" 已被占用。\n\n本机现监听地址为：\n" + addr +
                           L"\n\n请把新地址告知对方，或在对方更新配置。";
        MessageBoxW(hwnd_, msg.c_str(), L"监听端口已变更", MB_OK | MB_ICONWARNING);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    network_->stop();
    clipboard_->stop();
    destroyTrayIcon();
    UiTheme::shutdown();
    g_app = nullptr;
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Application::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Application* app = g_app;
    if (!app) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            if (app->clipboard_) {
                app->clipboard_->onClipboardUpdate();
            }
            return 0;

        case kWmPeerStatus:
            app->updateConnectionStatus();
            return 0;

        case kSingleInstanceActivateMsg:
            FlashWindow(hwnd, TRUE);
            return 0;

        case kTrayCallbackMessage:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, kMenuPeerSettings, L"对端设置");
                AppendMenuW(menu, MF_STRING, kMenuQrCode, L"生成二维码");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kMenuRefreshStatus, L"刷新连接状态");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
                SetForegroundWindow(hwnd);
                const UINT cmd =
                    TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (cmd == kMenuPeerSettings) {
                    app->showPeerSettings();
                } else if (cmd == kMenuQrCode) {
                    app->showQrDialog();
                } else if (cmd == kMenuRefreshStatus) {
                    app->network_->runProbeAndNotify();
                    app->updateConnectionStatus();
                } else if (cmd == kMenuExit) {
                    PostQuitMessage(0);
                }
            } else if (lParam == WM_LBUTTONDBLCLK) {
                app->showPeerSettings();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace clipsync
