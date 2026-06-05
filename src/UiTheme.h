#pragma once

#include <windows.h>

namespace clipsync {

// 字体层级对齐 Windows 11 设置等系统应用：
// - 分区标题：Segoe UI Semibold 14pt
// - 正文/控件：Segoe UI 14pt（系统 Message 字体）
// - 辅助说明：Segoe UI 12pt，次要色
class UiTheme {
public:
    static bool initialize();
    static void shutdown();

    static HFONT fontBody();
    static HFONT fontSection();
    static HFONT fontCaption();

    static void applyBody(HWND hwnd);
    static int scale(HWND ref, int px);

    static COLORREF colorText();
    static COLORREF colorTextSecondary();
    static COLORREF colorOnline();
    static COLORREF colorOffline();
    static COLORREF colorChecking();

    static void enableWindowRoundCorner(HWND hwnd);
    static void resizeWindowToClient(HWND hwnd, int clientW, int clientH);
};

}  // namespace clipsync
