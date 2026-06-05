#pragma once

#include <string>
#include <windows.h>

namespace clipsync {

/** 从二维码图片中解码文本（离线，无网络） */
class QrDecoder {
public:
    static bool decodeFromFile(const std::wstring& path, std::string& outUtf8,
                               std::wstring& errMsg);
    static bool decodeFromHBITMAP(HBITMAP hBmp, std::string& outUtf8,
                                std::wstring& errMsg);
};

}  // namespace clipsync
