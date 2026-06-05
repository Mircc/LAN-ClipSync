#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace clipsync {

struct ClipMessage {
    std::string version;
    std::string type;
    std::string payload;
    int64_t timestamp = 0;
};

std::string buildTextMessage(const std::string& utf8Payload, int64_t timestamp);
std::optional<ClipMessage> parseClipMessage(const std::string& json);

std::wstring utf8ToWide(const std::string& utf8);
std::string wideToUtf8(const std::wstring& wide);

size_t wcharCount(const std::wstring& text);
std::wstring truncateWide(const std::wstring& text, size_t maxChars);

}  // namespace clipsync
