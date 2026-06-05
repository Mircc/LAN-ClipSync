#include "UiTheme.h"

#include <commctrl.h>
#include <dwmapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace clipsync {

namespace {

bool g_inited = false;
HFONT g_fontBody = nullptr;
HFONT g_fontSection = nullptr;
HFONT g_fontCaption = nullptr;
UINT g_cachedDpi = 96;

HFONT createFontPx(int heightPx, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(heightPx, static_cast<int>(g_cachedDpi), 96);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

}  // namespace

bool UiTheme::initialize() {
    if (g_inited) {
        return true;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_cachedDpi = GetDpiForSystem();
    if (g_cachedDpi == 0) {
        g_cachedDpi = 96;
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
        g_fontBody = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!g_fontBody) {
        g_fontBody = createFontPx(14, FW_NORMAL);
    }

    g_fontSection = g_fontBody;
    g_fontCaption = g_fontBody;

    g_inited = g_fontBody != nullptr;
    return g_inited;
}

void UiTheme::shutdown() {
    if (g_fontBody) {
        DeleteObject(g_fontBody);
        g_fontBody = nullptr;
    }
    g_fontSection = nullptr;
    g_fontCaption = nullptr;
    g_inited = false;
}

HFONT UiTheme::fontBody() { return g_fontBody; }

HFONT UiTheme::fontSection() { return g_fontSection ? g_fontSection : g_fontBody; }

HFONT UiTheme::fontCaption() { return g_fontCaption ? g_fontCaption : g_fontBody; }

void UiTheme::applyBody(HWND hwnd) {
    if (g_fontBody && hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
    }
}

int UiTheme::scale(HWND ref, int px) {
    UINT dpi = g_cachedDpi;
    if (ref) {
        const UINT wdpi = GetDpiForWindow(ref);
        if (wdpi != 0) {
            dpi = wdpi;
        }
    }
    return MulDiv(px, static_cast<int>(dpi), 96);
}

COLORREF UiTheme::colorText() { return GetSysColor(COLOR_WINDOWTEXT); }

COLORREF UiTheme::colorTextSecondary() { return GetSysColor(COLOR_GRAYTEXT); }

COLORREF UiTheme::colorOnline() { return RGB(15, 123, 15); }

COLORREF UiTheme::colorOffline() { return RGB(196, 43, 28); }

COLORREF UiTheme::colorChecking() { return RGB(96, 96, 96); }

void UiTheme::enableWindowRoundCorner(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    enum DWM_WINDOW_CORNER_PREFERENCE { DWMWCP_ROUND = 2 };
    const DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref));
}

void UiTheme::resizeWindowToClient(HWND hwnd, int clientW, int clientH) {
    if (!hwnd) {
        return;
    }
    RECT rc{0, 0, clientW, clientH};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int outerW = rc.right - rc.left;
    const int outerH = rc.bottom - rc.top;
    SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace clipsync
