#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

constexpr int kQrModulePx = 6;
constexpr int kQrBorderModules = 4;
/** 对话框内 QR 显示区域最小边长（逻辑像素，需按 DPI 缩放） */
constexpr int kQrMinUiPx = 400;

/** 最小 QR（版本 1）在默认参数下的像素边长，用于 PNG 等默认渲染估算 */
constexpr int kQrMinDisplayPx =
    (21 + 2 * kQrBorderModules) * kQrModulePx;

/** 渲染像素边长 = (qrModuleCount + 2 * border) * modulePx */
int qrRenderedPxSize(int qrModuleCount, int modulePx = kQrModulePx, int border = kQrBorderModules);

/** 编码文本并返回 QR 模块边长（不含 border）；失败返回 0 */
int qrModuleCountForText(const std::string& utf8Text);

/** 根据目标显示边长反算 modulePx（>= 2 且为偶数） */
int modulePxForTargetDisplay(int qrModuleCount, int targetDisplayPx,
                             int border = kQrBorderModules);

/** 将 UTF-8 文本编码为 QR 并渲染到 BGRA 缓冲区（自上而下，每像素 4 字节，纯黑/纯白） */
bool renderQrToBgra(const std::string& utf8Text, std::vector<uint8_t>& outBgra, int& outPxSize,
                    int modulePx = kQrModulePx, int border = kQrBorderModules);

/** 从 BGRA 像素创建 32 位 DIB 位图（无缩放、无抗锯齿） */
HBITMAP createBitmapFromBgra(const uint8_t* bgra, int pxSize);

/** 生成 QR 位图；失败返回 nullptr */
HBITMAP renderQrToBitmap(const std::string& utf8Text,
                         int modulePx = kQrModulePx, int border = kQrBorderModules);

/** 保存 QR 为 PNG（直接写入像素，禁用 GDI+ 平滑/插值） */
bool saveQrToPng(const std::string& utf8Text, const std::wstring& path,
                 int modulePx = kQrModulePx, int border = kQrBorderModules);

}  // namespace clipsync
