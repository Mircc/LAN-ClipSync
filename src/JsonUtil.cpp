#include "JsonUtil.h"

#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

namespace {

std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    continue;
                }
                out += c;
                break;
        }
    }
    return out;
}

std::optional<std::string> extractStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string value;
    bool escape = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escape) {
            switch (c) {
                case '"':
                    value.push_back('"');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '/':
                    value.push_back('/');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'u':
                    if (pos + 4 < json.size()) {
                        pos += 4;
                    }
                    break;
                default:
                    value.push_back(c);
                    break;
            }
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            return value;
        }
        value.push_back(c);
    }
    return std::nullopt;
}

std::optional<int64_t> extractIntField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    size_t end = pos;
    while (end < json.size() && (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) {
        ++end;
    }
    if (end == pos) {
        return std::nullopt;
    }
    return std::stoll(json.substr(pos, end - pos));
}

}  // namespace

std::string buildTextMessage(const std::string& utf8Payload, int64_t timestamp) {
    std::ostringstream oss;
    oss << "{\"version\":\"1.0\",\"type\":\"text\",\"length\":" << utf8Payload.size()
        << ",\"payload\":\"" << escapeJson(utf8Payload) << "\",\"timestamp\":" << timestamp << "}";
    return oss.str();
}

std::optional<ClipMessage> parseClipMessage(const std::string& json) {
    ClipMessage msg;
    auto version = extractStringField(json, "version");
    auto type = extractStringField(json, "type");
    auto payload = extractStringField(json, "payload");
    auto timestamp = extractIntField(json, "timestamp");
    if (!type || !payload) {
        return std::nullopt;
    }
    msg.version = version.value_or("1.0");
    msg.type = *type;
    msg.payload = *payload;
    msg.timestamp = timestamp.value_or(0);
    return msg;
}

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

size_t wcharCount(const std::wstring& text) { return text.size(); }

std::wstring truncateWide(const std::wstring& text, size_t maxChars) {
    if (text.size() <= maxChars) {
        return text;
    }
    return text.substr(0, maxChars);
}

}  // namespace clipsync
