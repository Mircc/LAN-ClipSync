#pragma once

#include <windows.h>

namespace clipsync {

class QrDialog {
public:
    /** 显示二维码工具（生成 / 读取，模态） */
    static void show(HWND owner);
};

}  // namespace clipsync
