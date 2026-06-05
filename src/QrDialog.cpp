#include "QrDialog.h"

#include "QrRender.h"
#include "JsonUtil.h"
#include "Logger.h"
#include "UiTheme.h"
#include "resource.h"

#include <algorithm>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

namespace clipsync {

namespace {

constexpr wchar_t kDialogClass[] = L"LANClipSyncQrDialog";

constexpr int IDC_TEXT_INPUT = 200;
constexpr int IDC_QR_DISPLAY = 201;

constexpr UINT kGenTimerId = 1;
constexpr UINT kGenTimerMs = 350;
constexpr int kDefaultInputH = 72;

HWND g_dialog = nullptr;
HBITMAP g_qrBitmap = nullptr;
int g_qrBmpW = 0;
int g_qrBmpH = 0;
int g_inputW = 0;
int g_inputH = 0;
bool g_generating = false;
bool g_resizingInput = false;
int g_resizeStartX = 0;
int g_resizeStartY = 0;
int g_resizeStartInputW = 0;
int g_resizeStartInputH = 0;
WNDPROC g_origEditProc = nullptr;

struct QrLayoutMetrics {
    int availW = 0;
    int availH = 0;
    int targetDisplaySz = 0;
    int minDisplayPx = 0;
};

void layoutControls(HWND hwnd);
LRESULT CALLBACK editSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

HMENU ctrlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

int contentMargin(HWND hwnd) {
    return UiTheme::scale(hwnd, 12);
}

int layoutGap(HWND hwnd) {
    return UiTheme::scale(hwnd, 8);
}

int defaultInputHeight(HWND hwnd) {
    return UiTheme::scale(hwnd, kDefaultInputH);
}

int minInputHeight(HWND hwnd) {
    return UiTheme::scale(hwnd, 48);
}

int minInputWidth(HWND hwnd) {
    return UiTheme::scale(hwnd, 120);
}

int minQrDisplayPx(HWND hwnd) {
    return MulDiv(kQrMinUiPx, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

int contentWidth(HWND hwnd) {
    RECT cr{};
    GetClientRect(hwnd, &cr);
    return cr.right - contentMargin(hwnd) * 2;
}

int currentInputWidth(HWND hwnd) {
    if (g_inputW > 0) return g_inputW;
    return contentWidth(hwnd);
}

int currentInputHeight(HWND hwnd) {
    if (g_inputH > 0) return g_inputH;
    return defaultInputHeight(hwnd);
}

int maxInputWidth(HWND hwnd) {
    return std::max(minInputWidth(hwnd), contentWidth(hwnd));
}

int maxInputHeight(HWND hwnd) {
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int m = contentMargin(hwnd);
    const int gap = layoutGap(hwnd);
    const int qrNeedH = g_qrBmpH > 0 ? g_qrBmpH : minQrDisplayPx(hwnd);
    const int maxH = cr.bottom - m - gap - qrNeedH - m;
    return std::max(minInputHeight(hwnd), maxH);
}

int gripSize(HWND hwnd) {
    return UiTheme::scale(hwnd, 16);
}

bool pointInEditGrip(HWND hEdit, int clientX, int clientY) {
    RECT rc{};
    GetClientRect(hEdit, &rc);
    const int gs = gripSize(GetParent(hEdit));
    return clientX >= rc.right - gs && clientY >= rc.bottom - gs;
}

POINT parentClientPointFromEditLParam(HWND hEdit, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ClientToScreen(hEdit, &pt);
    ScreenToClient(GetParent(hEdit), &pt);
    return pt;
}

void drawEditGrip(HWND hEdit, HDC hdc) {
    RECT rc{};
    GetClientRect(hEdit, &rc);
    const int gs = gripSize(GetParent(hEdit));
    RECT grip{rc.right - gs, rc.bottom - gs, rc.right, rc.bottom};
    DrawFrameControl(hdc, &grip, DFC_SCROLL, DFCS_SCROLLSIZEGRIP);
}

void applyInputSizeResize(HWND hwnd, int parentClientX, int parentClientY) {
    const int dx = parentClientX - g_resizeStartX;
    const int dy = parentClientY - g_resizeStartY;
    const int newW = std::clamp(g_resizeStartInputW + dx,
                                minInputWidth(hwnd), maxInputWidth(hwnd));
    const int newH = std::clamp(g_resizeStartInputH + dy,
                                minInputHeight(hwnd), maxInputHeight(hwnd));
    if (newW != currentInputWidth(hwnd) || newH != currentInputHeight(hwnd)) {
        g_inputW = newW;
        g_inputH = newH;
        layoutControls(hwnd);
    }
}

void clampInputSize(HWND hwnd) {
    g_inputW = std::clamp(currentInputWidth(hwnd), minInputWidth(hwnd), maxInputWidth(hwnd));
    g_inputH = std::clamp(currentInputHeight(hwnd), minInputHeight(hwnd), maxInputHeight(hwnd));
}

QrLayoutMetrics computeQrLayoutMetrics(HWND hwnd) {
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int cw = cr.right;
    const int ch = cr.bottom;
    const int m = contentMargin(hwnd);
    const int gap = layoutGap(hwnd);
    const int inputH = currentInputHeight(hwnd);

    const int availW = cw - m * 2;
    const int availH = ch - m - inputH - gap - m;
    const int minDisplayPx = minQrDisplayPx(hwnd);

    const int displayW = std::min(availW, std::max(minDisplayPx, availW));
    const int displayH = std::min(availH, std::max(minDisplayPx, availH));
    const int displaySz = std::min(displayW, displayH);

    return {availW, availH, std::max(0, displaySz), minDisplayPx};
}

void setFont(HWND hwnd, HFONT font) {
    if (hwnd && font) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

std::wstring readClipboardText() {
    std::wstring result;
    if (!OpenClipboard(nullptr)) return result;

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data) {
            const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
            if (text) {
                const size_t maxLen = 8000;
                result.assign(text, wcsnlen(text,
                    std::min(static_cast<size_t>(wcslen(text)), maxLen)));
            }
            GlobalUnlock(data);
        }
    } else if (IsClipboardFormatAvailable(CF_TEXT)) {
        HANDLE data = GetClipboardData(CF_TEXT);
        if (data) {
            const char* ansi = static_cast<const char*>(GlobalLock(data));
            if (ansi) {
                const int len = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
                if (len > 0) {
                    result.resize(static_cast<size_t>(len - 1));
                    MultiByteToWideChar(CP_ACP, 0, ansi, -1, result.data(), len);
                }
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
}

std::wstring readTextFromEdit() {
    if (!g_dialog) return {};
    HWND hEdit = GetDlgItem(g_dialog, IDC_TEXT_INPUT);
    if (!hEdit) return {};
    const int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len + 1), L'\0');
    const int copied = GetWindowTextW(hEdit, text.data(), len + 1);
    if (copied <= 0) return {};
    text.resize(static_cast<size_t>(copied));
    return text;
}

void detachQrBitmapFromDisplay(HWND hDisplay) {
    if (!hDisplay) return;
    SendMessageW(hDisplay, STM_SETIMAGE, IMAGE_BITMAP, 0);
}

void setQrBitmapOnDisplay(HWND hDisplay, HBITMAP bmp) {
    if (!hDisplay) return;
    detachQrBitmapFromDisplay(hDisplay);
    if (bmp) {
        SendMessageW(hDisplay, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bmp));
    }
}

void updateQrDisplay() {
    if (!g_dialog || g_generating) return;
    g_generating = true;

    HWND hDisplay = GetDlgItem(g_dialog, IDC_QR_DISPLAY);
    detachQrBitmapFromDisplay(hDisplay);
    if (g_qrBitmap) {
        DeleteObject(g_qrBitmap);
        g_qrBitmap = nullptr;
    }

    const std::wstring text = readTextFromEdit();
    if (text.empty()) {
        g_qrBmpW = 0;
        g_qrBmpH = 0;
        layoutControls(g_dialog);
        g_generating = false;
        return;
    }

    const std::string utf8 = wideToUtf8(text);
    const int qrModules = qrModuleCountForText(utf8);
    if (qrModules <= 0) {
        g_qrBmpW = 0;
        g_qrBmpH = 0;
        layoutControls(g_dialog);
        Logger::error(L"二维码生成失败");
        g_generating = false;
        return;
    }

    const QrLayoutMetrics metrics = computeQrLayoutMetrics(g_dialog);
    const int modulePx = modulePxForTargetDisplay(qrModules, metrics.targetDisplaySz);
    g_qrBitmap = renderQrToBitmap(utf8, modulePx);
    if (g_qrBitmap) {
        BITMAP bm{};
        GetObject(g_qrBitmap, sizeof(bm), &bm);
        g_qrBmpW = bm.bmWidth;
        g_qrBmpH = bm.bmHeight;

        setQrBitmapOnDisplay(hDisplay, g_qrBitmap);
        layoutControls(g_dialog);
        Logger::info(L"二维码已生成");
    } else {
        g_qrBmpW = 0;
        g_qrBmpH = 0;
        layoutControls(g_dialog);
        Logger::error(L"二维码生成失败");
    }

    g_generating = false;
}

void scheduleGenerate(HWND hwnd) {
    SetTimer(hwnd, kGenTimerId, kGenTimerMs, nullptr);
}

void layoutControls(HWND hwnd) {
    const int m = contentMargin(hwnd);
    const int gap = layoutGap(hwnd);
    const int inputW = currentInputWidth(hwnd);
    const int inputH = currentInputHeight(hwnd);
    const int minDisplayPx = minQrDisplayPx(hwnd);

    const int qrW = g_qrBmpW > 0 ? g_qrBmpW : minDisplayPx;
    const int qrH = g_qrBmpH > 0 ? g_qrBmpH : minDisplayPx;

    HWND hEdit = GetDlgItem(hwnd, IDC_TEXT_INPUT);
    HWND hQr = GetDlgItem(hwnd, IDC_QR_DISPLAY);
    if (hEdit) {
        SetWindowPos(hEdit, nullptr, m, m, inputW, inputH, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hEdit, nullptr, FALSE);
    }
    if (hQr) {
        const int contentW = contentWidth(hwnd);
        const int x = m + (contentW - qrW) / 2;
        const int y = m + inputH + gap;
        SetWindowPos(hQr, nullptr, x, y, qrW, qrH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

int minClientHeight(HWND hwnd) {
    const int m = contentMargin(hwnd);
    const int gap = layoutGap(hwnd);
    const int minQrH = minQrDisplayPx(hwnd);
    return m + minInputHeight(hwnd) + gap + minQrH + m;
}

int minClientWidth(HWND hwnd) {
    const int m = contentMargin(hwnd);
    return std::max(UiTheme::scale(hwnd, 440), minQrDisplayPx(hwnd) + m * 2);
}

LRESULT CALLBACK editSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (pointInEditGrip(hwnd, pt.x, pt.y) || g_resizingInput) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (pointInEditGrip(hwnd, x, y)) {
            HWND parent = GetParent(hwnd);
            const POINT pt = parentClientPointFromEditLParam(hwnd, lParam);
            g_resizingInput = true;
            g_resizeStartX = pt.x;
            g_resizeStartY = pt.y;
            g_resizeStartInputW = currentInputWidth(parent);
            g_resizeStartInputH = currentInputHeight(parent);
            SetCapture(hwnd);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (g_resizingInput) {
            const POINT pt = parentClientPointFromEditLParam(hwnd, lParam);
            applyInputSizeResize(GetParent(hwnd), pt.x, pt.y);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_resizingInput) {
            g_resizingInput = false;
            ReleaseCapture();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if (g_resizingInput && reinterpret_cast<HWND>(lParam) != hwnd) {
            g_resizingInput = false;
        }
        break;

    case WM_PAINT: {
        const LRESULT result = CallWindowProcW(g_origEditProc, hwnd, msg, wParam, lParam);
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            drawEditGrip(hwnd, hdc);
            ReleaseDC(hwnd, hdc);
        }
        return result;
    }

    default:
        break;
    }

    return CallWindowProcW(g_origEditProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_dialog = hwnd;
        g_inputW = 0;
        g_inputH = defaultInputHeight(hwnd);
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        const HINSTANCE inst = static_cast<HINSTANCE>(cs->hInstance);

        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, ctrlId(IDC_TEXT_INPUT), inst, nullptr);
        if (hEdit) {
            g_origEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                hEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editSubclassProc)));
        }

        CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            0, 0, 0, 0, hwnd, ctrlId(IDC_QR_DISPLAY), inst, nullptr);

        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            setFont(child, UiTheme::fontBody());
        }

        const int cw = minClientWidth(hwnd);
        const int ch = minClientHeight(hwnd);
        UiTheme::resizeWindowToClient(hwnd, cw, ch);
        g_inputW = contentWidth(hwnd);
        layoutControls(hwnd);

        const std::wstring clip = readClipboardText();
        if (!clip.empty()) {
            if (hEdit) SetWindowTextW(hEdit, clip.c_str());
            updateQrDisplay();
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == kGenTimerId) {
            KillTimer(hwnd, kGenTimerId);
            updateQrDisplay();
        }
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == IDC_TEXT_INPUT) {
            scheduleGenerate(hwnd);
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

    case WM_CTLCOLOREDIT:
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = minClientWidth(hwnd);
        info->ptMinTrackSize.y = minClientHeight(hwnd);
        return 0;
    }

    case WM_SIZE:
        clampInputSize(hwnd);
        layoutControls(hwnd);
        if (!readTextFromEdit().empty()) {
            scheduleGenerate(hwnd);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kGenTimerId);
        if (HWND hDisplay = GetDlgItem(hwnd, IDC_QR_DISPLAY)) {
            detachQrBitmapFromDisplay(hDisplay);
        }
        if (g_qrBitmap) {
            DeleteObject(g_qrBitmap);
            g_qrBitmap = nullptr;
        }
        g_qrBmpW = 0;
        g_qrBmpH = 0;
        g_inputW = 0;
        g_inputH = 0;
        g_resizingInput = false;
        g_origEditProc = nullptr;
        g_dialog = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void QrDialog::show(HWND owner) {
    UiTheme::initialize();

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kDialogClass;
        RegisterClassExW(&wc);
        reg = true;
    }

    g_qrBitmap = nullptr;
    g_qrBmpW = 0;
    g_qrBmpH = 0;
    g_inputW = 0;
    g_inputH = 0;
    g_resizingInput = false;
    g_generating = false;

    RECT or_{};
    GetWindowRect(owner, &or_);
    const int x = or_.left + UiTheme::scale(owner, 48);
    const int y = or_.top + UiTheme::scale(owner, 48);

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kDialogClass, L"二维码",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, UiTheme::scale(owner, 360), UiTheme::scale(owner, 460),
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) return;

    if (HICON icon = reinterpret_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED))) {
        SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }

    UiTheme::enableWindowRoundCorner(dlg);
    EnableWindow(owner, FALSE);

    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_resizingInput || !IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

}  // namespace clipsync
