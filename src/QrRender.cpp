#include "QrRender.h"

#include "Logger.h"
#include "qrcodegen.hpp"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <propidl.h>
#pragma comment(lib, "gdiplus.lib")
#include <gdiplus.h>

namespace clipsync {

namespace {

int clampBorder(int border) {
    return border < kQrBorderModules ? kQrBorderModules : border;
}

void fillBgraPixel(uint8_t* px, bool dark) {
    if (dark) {
        px[0] = 0;
        px[1] = 0;
        px[2] = 0;
    } else {
        px[0] = 255;
        px[1] = 255;
        px[2] = 255;
    }
    px[3] = 255;
}

bool renderEncodedQr(const qrcodegen::QrCode& qr, std::vector<uint8_t>& outBgra, int& outPxSize,
                     int modulePx, int border) {
    if (modulePx <= 0) return false;
    border = clampBorder(border);

    const int qrSize = qr.getSize();
    const int imgSize = (qrSize + 2 * border) * modulePx;
    outPxSize = imgSize;
    outBgra.assign(static_cast<size_t>(imgSize) * imgSize * 4, 0);

    for (int y = 0; y < imgSize; ++y) {
        const int moduleY = y / modulePx - border;
        for (int x = 0; x < imgSize; ++x) {
            const int moduleX = x / modulePx - border;
            bool dark = false;
            if (moduleX >= 0 && moduleX < qrSize && moduleY >= 0 && moduleY < qrSize) {
                dark = qr.getModule(moduleX, moduleY);
            }
            fillBgraPixel(&outBgra[(static_cast<size_t>(y) * imgSize + x) * 4], dark);
        }
    }
    return true;
}

}  // namespace

int qrRenderedPxSize(int qrModuleCount, int modulePx, int border) {
    border = clampBorder(border);
    if (modulePx <= 0 || qrModuleCount <= 0) return 0;
    return (qrModuleCount + 2 * border) * modulePx;
}

int qrModuleCountForText(const std::string& utf8Text) {
    if (utf8Text.empty()) return 0;
    try {
        const qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(utf8Text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        return qr.getSize();
    } catch (const qrcodegen::data_too_long&) {
        return 0;
    } catch (const std::exception&) {
        return 0;
    }
}

int modulePxForTargetDisplay(int qrModuleCount, int targetDisplayPx, int border) {
    border = clampBorder(border);
    if (qrModuleCount <= 0) return 2;
    if (targetDisplayPx <= 0) return 2;
    int calcModulePx = targetDisplayPx / (qrModuleCount + 2 * border);
    if (calcModulePx < 2) calcModulePx = 2;
    if (calcModulePx % 2 != 0) --calcModulePx;
    return calcModulePx;
}

bool renderQrToBgra(const std::string& utf8Text, std::vector<uint8_t>& outBgra, int& outPxSize,
                    int modulePx, int border) {
    if (utf8Text.empty()) return false;
    try {
        const qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(utf8Text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        return renderEncodedQr(qr, outBgra, outPxSize, modulePx, border);
    } catch (const qrcodegen::data_too_long&) {
        Logger::error(L"QR 内容过长，超出最大容量");
        return false;
    } catch (const std::exception&) {
        Logger::error(L"二维码生成失败");
        return false;
    }
}

HBITMAP createBitmapFromBgra(const uint8_t* bgra, int pxSize) {
    if (!bgra || pxSize <= 0) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pxSize;
    bmi.bmiHeader.biHeight = -pxSize;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        return nullptr;
    }

    const size_t byteCount = static_cast<size_t>(pxSize) * pxSize * 4;
    memcpy(bits, bgra, byteCount);
    return hBmp;
}

HBITMAP renderQrToBitmap(const std::string& utf8Text, int modulePx, int border) {
    std::vector<uint8_t> pixels;
    int pxSize = 0;
    if (!renderQrToBgra(utf8Text, pixels, pxSize, modulePx, border)) {
        return nullptr;
    }
    return createBitmapFromBgra(pixels.data(), pxSize);
}

bool saveQrToPng(const std::string& utf8Text, const std::wstring& path,
                 int modulePx, int border) {
    std::vector<uint8_t> pixels;
    int pxSize = 0;
    if (!renderQrToBgra(utf8Text, pixels, pxSize, modulePx, border)) {
        return false;
    }

    Gdiplus::GdiplusStartupInput input{};
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        return false;
    }

    bool ok = false;
    do {
        Gdiplus::Bitmap bmp(pxSize, pxSize, PixelFormat32bppARGB);
        if (bmp.GetLastStatus() != Gdiplus::Ok) break;

        Gdiplus::BitmapData locked{};
        Gdiplus::Rect rect(0, 0, pxSize, pxSize);
        if (bmp.LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &locked) !=
            Gdiplus::Ok) {
            break;
        }

        const int stride = locked.Stride;
        auto* dst = static_cast<uint8_t*>(locked.Scan0);
        for (int y = 0; y < pxSize; ++y) {
            memcpy(dst + static_cast<size_t>(y) * stride,
                   pixels.data() + static_cast<size_t>(y) * pxSize * 4,
                   static_cast<size_t>(pxSize) * 4);
        }
        bmp.UnlockBits(&locked);

        UINT numEncoders = 0;
        UINT encoderSize = 0;
        if (Gdiplus::GetImageEncodersSize(&numEncoders, &encoderSize) != Gdiplus::Ok ||
            encoderSize == 0) {
            break;
        }

        std::vector<uint8_t> encoderBytes(encoderSize);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(encoderBytes.data());
        if (Gdiplus::GetImageEncoders(numEncoders, encoderSize, encoders) != Gdiplus::Ok) {
            break;
        }

        CLSID pngClsid{};
        bool foundPng = false;
        for (UINT i = 0; i < numEncoders; ++i) {
            if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                pngClsid = encoders[i].Clsid;
                foundPng = true;
                break;
            }
        }
        if (!foundPng) break;

        ok = (bmp.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok);
    } while (false);

    Gdiplus::GdiplusShutdown(token);
    return ok;
}

}  // namespace clipsync
