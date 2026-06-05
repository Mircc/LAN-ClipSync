#include "PeerConfig.h"

#include "JsonUtil.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace clipsync {

namespace {

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return L".\\";
    }
    std::wstring full(path);
    const auto pos = full.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        full.resize(pos + 1);
    }
    return full;
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

std::optional<std::string> extractQuotedAfterKey(const std::string& json, const std::string& key,
                                                 size_t fromPos) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle, fromPos);
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
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case '"':
                case '\\':
                case '/':
                    value.push_back(c);
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

bool isLiteralIPv4(const std::string& s) {
    in_addr addr{};
    return inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

std::string stripIpv6Brackets(std::string s) {
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

std::string normalizeIpv6Literal(const std::string& s) {
    return stripIpv6Brackets(s);
}

std::wstring trimWide(std::wstring s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) {
        s.pop_back();
    }
    return s;
}

bool isValidHostLabel(const std::string& s) {
    if (s.empty() || s.size() > 253) {
        return false;
    }
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::wstring displayHostOrIpWide(const std::string& hostOrIp) {
    if (isLiteralIPv6String(hostOrIp)) {
        return L"[" + utf8ToWide(normalizeIpv6Literal(hostOrIp)) + L"]";
    }
    return utf8ToWide(hostOrIp);
}

bool splitHostPortWide(const std::wstring& segment, std::wstring& hostPart, std::wstring& portPart) {
    const std::wstring s = trimWide(segment);
    if (s.empty()) {
        return false;
    }
    if (s.front() == L'[') {
        const auto close = s.find(L']');
        if (close == std::wstring::npos || close + 1 >= s.size() || s[close + 1] != L':') {
            return false;
        }
        hostPart = s.substr(1, close - 1);
        portPart = s.substr(close + 2);
        return true;
    }

    const auto colon = s.rfind(L':');
    if (colon == std::wstring::npos) {
        hostPart = s;
        portPart.clear();
        return true;
    }
    if (colon == 0 || colon == s.size() - 1) {
        return false;
    }
    hostPart = s.substr(0, colon);
    portPart = s.substr(colon + 1);
    const std::string hostUtf8 = wideToUtf8(hostPart);
    if (isLiteralIPv6String(hostUtf8)) {
        return true;
    }
    if (std::count(hostPart.begin(), hostPart.end(), L':') > 0) {
        return false;
    }
    return true;
}

bool parseSinglePeerSegment(const std::wstring& segment, PeerEndpoint& out) {
    out = {};
    std::wstring hostPart;
    std::wstring portPart;
    if (!splitHostPortWide(segment, hostPart, portPart)) {
        return false;
    }

    const std::string hostOrIp = wideToUtf8(trimWide(hostPart));
    if (hostOrIp.empty()) {
        return false;
    }

    if (isLiteralIPv4(hostOrIp)) {
        out.ip = hostOrIp;
    } else if (isLiteralIPv6String(hostOrIp)) {
        out.ip = normalizeIpv6Literal(hostOrIp);
    } else if (!isValidHostLabel(hostOrIp)) {
        return false;
    } else {
        out.host = hostOrIp;
    }

    if (!portPart.empty()) {
        wchar_t* end = nullptr;
        const unsigned long portVal = wcstoul(portPart.c_str(), &end, 10);
        if (end == portPart.c_str() || portVal == 0 || portVal > 65535) {
            return false;
        }
        out.tcpPort = static_cast<uint16_t>(portVal);
    }
    return true;
}

std::vector<std::wstring> splitSemicolonSegments(const std::wstring& input) {
    std::vector<std::wstring> segments;
    std::wstring current;
    for (const wchar_t c : input) {
        if (c == L';' || c == L'；') {
            const std::wstring part = trimWide(current);
            if (!part.empty()) {
                segments.push_back(part);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::wstring part = trimWide(current);
    if (!part.empty()) {
        segments.push_back(part);
    }
    return segments;
}

int targetPreference(const PeerSocketTarget& t) {
    if (t.addr.ss_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(&t.addr);
        const uint32_t ip = ntohl(sin->sin_addr.s_addr);
        if ((ip & 0xFF000000u) == 0x0A000000u || (ip & 0xFFFF0000u) == 0xC0A80000u ||
            (ip & 0xFFF00000u) == 0xAC100000u) {
            return 0;
        }
        return 2;
    }
    if (t.addr.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&t.addr);
        const in6_addr& a = sin6->sin6_addr;
        if (IN6_IS_ADDR_LOOPBACK(&a)) {
            return 100;
        }
        if (IN6_IS_ADDR_LINKLOCAL(&a)) {
            return 40;
        }
        return 10;
    }
    return 90;
}

bool setTargetPort(sockaddr_storage& storage, int& len, uint16_t port) {
    if (storage.ss_family == AF_INET) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&storage);
        sin->sin_port = htons(port);
        len = sizeof(sockaddr_in);
        return true;
    }
    if (storage.ss_family == AF_INET6) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&storage);
        sin6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

bool appendTarget(std::vector<PeerSocketTarget>& out, const addrinfo* info, uint16_t port) {
    PeerSocketTarget t{};
    memcpy(&t.addr, info->ai_addr, info->ai_addrlen);
    t.addrLen = static_cast<int>(info->ai_addrlen);
    if (!setTargetPort(t.addr, t.addrLen, port)) {
        return false;
    }
    out.push_back(t);
    return true;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::vector<PeerEndpoint> parsePeersArray(const std::string& json) {
    std::vector<PeerEndpoint> peers;
    size_t searchFrom = 0;
    while (true) {
        const auto hostPos = json.find("\"host\"", searchFrom);
        const auto ipPos = json.find("\"ip\"", searchFrom);
        if (hostPos == std::string::npos && ipPos == std::string::npos) {
            break;
        }
        const bool useHost =
            hostPos != std::string::npos && (ipPos == std::string::npos || hostPos < ipPos);
        const size_t keyPos = useHost ? hostPos : ipPos;
        const auto addrVal =
            extractQuotedAfterKey(json, useHost ? "host" : "ip", keyPos);
        if (!addrVal || addrVal->empty()) {
            searchFrom = keyPos + 4;
            continue;
        }
        const auto portVal = extractIntField(json.substr(keyPos), "port");
        if (!portVal || *portVal <= 0 || *portVal > 65535) {
            searchFrom = keyPos + 4;
            continue;
        }
        PeerEndpoint ep;
        ep.tcpPort = static_cast<uint16_t>(*portVal);
        if (useHost) {
            ep.host = *addrVal;
        } else {
            ep.ip = *addrVal;
        }
        peers.push_back(ep);
        searchFrom = keyPos + 4;
    }
    return peers;
}

}  // namespace

std::wstring peerConfigFilePath() { return exeDirectory() + L"LAN-ClipSync-peers.json"; }

bool peerConfigFileExists() {
    const DWORD attr = GetFileAttributesW(peerConfigFilePath().c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

namespace {

constexpr wchar_t kRegKey[] = L"Software\\LAN-ClipSync";
constexpr wchar_t kRegPeersConfigured[] = L"PeersConfigured";

}  // namespace

bool isPeerSetupCompleted() {
    HKEY key = nullptr;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegQueryValueExW(key, kRegPeersConfigured, nullptr, nullptr, reinterpret_cast<LPBYTE>(&value),
                     &size);
    RegCloseKey(key);
    return value != 0;
}

void markPeerSetupCompleted() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD value = 1;
    RegSetValueExW(key, kRegPeersConfigured, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                   sizeof(value));
    RegCloseKey(key);
}

bool loadPeerConfig(AppPeerConfig& out) {
    out = {};
    const std::wstring wpath = peerConfigFilePath();
    // Convert wide path to UTF-8 for std::ifstream (MinGW compatibility)
    int nb = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string path(nb > 0 ? nb - 1 : 0, '\0');
    if (nb > 0) WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), nb, nullptr, nullptr);
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    const std::string json = oss.str();
    if (json.empty()) {
        return false;
    }

    const auto port = extractIntField(json, "listen_port");
    if (port && *port > 0 && *port <= 65535) {
        out.listenPort = static_cast<uint16_t>(*port);
    }
    out.peers = parsePeersArray(json);
    return true;
}

bool savePeerConfig(const AppPeerConfig& config) {
    std::ostringstream oss;
    oss << "{\"listen_port\":" << config.listenPort << ",\"peers\":[";
    for (size_t i = 0; i < config.peers.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        const auto& p = config.peers[i];
        if (!p.host.empty()) {
            oss << "{\"host\":\"" << jsonEscape(p.host) << "\",\"port\":" << p.tcpPort << "}";
        } else {
            oss << "{\"ip\":\"" << jsonEscape(p.ip) << "\",\"port\":" << p.tcpPort << "}";
        }
    }
    oss << "]}";

    const std::wstring wpath = peerConfigFilePath();
    // Convert wide path to UTF-8 for std::ofstream (MinGW compatibility)
    int nb = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string path(nb > 0 ? nb - 1 : 0, '\0');
    if (nb > 0) WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), nb, nullptr, nullptr);
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        return false;
    }
    const std::string body = oss.str();
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(file);
}

std::string localComputerNameUtf8() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &size)) {
        return {};
    }
    return wideToUtf8(name);
}

bool isLiteralIPv6String(const std::string& s) {
    const std::string bare = stripIpv6Brackets(s);
    in6_addr addr{};
    return inet_pton(AF_INET6, bare.c_str(), &addr) == 1;
}

bool resolveHostToTargets(const std::string& host, uint16_t port,
                          std::vector<PeerSocketTarget>& out) {
    if (host.empty()) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string portStr = std::to_string(port);
    addrinfo* result = nullptr;
    const int err = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (err != 0 || !result) {
        if (result) {
            freeaddrinfo(result);
        }
        return false;
    }

    for (addrinfo* p = result; p; p = p->ai_next) {
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) {
            continue;
        }
        appendTarget(out, p, port);
    }
    freeaddrinfo(result);

    std::sort(out.begin(), out.end(),
              [](const PeerSocketTarget& a, const PeerSocketTarget& b) {
                  return targetPreference(a) < targetPreference(b);
              });
    return !out.empty();
}

bool resolveLiteralIpTarget(const std::string& ip, uint16_t port, std::vector<PeerSocketTarget>& out) {
    PeerSocketTarget t{};
    if (isLiteralIPv4(ip)) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&t.addr);
        sin->sin_family = AF_INET;
        inet_pton(AF_INET, ip.c_str(), &sin->sin_addr);
        t.addrLen = sizeof(sockaddr_in);
        setTargetPort(t.addr, t.addrLen, port);
        out.push_back(t);
        return true;
    }
    if (isLiteralIPv6String(ip)) {
        const std::string bare = normalizeIpv6Literal(ip);
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&t.addr);
        sin6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, bare.c_str(), &sin6->sin6_addr);
        t.addrLen = sizeof(sockaddr_in6);
        setTargetPort(t.addr, t.addrLen, port);
        out.push_back(t);
        return true;
    }
    return false;
}

bool resolvePeerEndpointTargets(const PeerEndpoint& peer, std::vector<PeerSocketTarget>& targets) {
    targets.clear();
    if (!peer.hasAddress() || peer.tcpPort == 0) {
        return false;
    }
    if (!peer.ip.empty()) {
        return resolveLiteralIpTarget(peer.ip, peer.tcpPort, targets);
    }
    if (!peer.host.empty()) {
        return resolveHostToTargets(peer.host, peer.tcpPort, targets);
    }
    return false;
}

bool parsePeerAddress(const std::wstring& input, PeerEndpoint& out) {
    out = {};
    const std::wstring trimmed = trimWide(input);
    if (trimmed.empty()) {
        return false;
    }

    const auto segments = splitSemicolonSegments(trimmed);
    if (segments.empty()) {
        return false;
    }

    if (segments.size() == 1) {
        return parseSinglePeerSegment(segments[0], out);
    }

    std::vector<PeerEndpoint> candidates;
    candidates.reserve(segments.size());
    for (const auto& seg : segments) {
        PeerEndpoint ep;
        if (parseSinglePeerSegment(seg, ep)) {
            candidates.push_back(ep);
        }
    }
    if (candidates.empty()) {
        return false;
    }

    auto pickPreferred = [&]() -> const PeerEndpoint* {
        for (const auto& ep : candidates) {
            if (!ep.host.empty()) {
                return &ep;
            }
        }
        for (const auto& ep : candidates) {
            if (!ep.ip.empty() && isLiteralIPv4(ep.ip)) {
                return &ep;
            }
        }
        for (const auto& ep : candidates) {
            if (!ep.ip.empty() && isLiteralIPv6String(ep.ip)) {
                return &ep;
            }
        }
        return &candidates.front();
    };

    out = *pickPreferred();
    if (out.tcpPort == 0) {
        for (const auto& ep : candidates) {
            if (ep.tcpPort != 0) {
                out.tcpPort = ep.tcpPort;
                break;
            }
        }
    }
    return true;
}

std::wstring formatPeerAddress(const PeerEndpoint& peer) {
    return formatPeerAddress(peer.keyAddress(), peer.tcpPort);
}

std::wstring formatPeerAddress(const std::string& hostOrIp, uint16_t port) {
    return displayHostOrIpWide(hostOrIp) + L":" + std::to_wstring(port);
}

std::wstring formatLocalListenAddresses(const std::string& ipv4, const std::string& ipv6,
                                        const std::string& computerName, uint16_t port) {
    std::wstring line;
    auto appendPart = [&](const std::wstring& part) {
        if (part.empty()) {
            return;
        }
        if (!line.empty()) {
            line += L';';
        }
        line += part;
    };

    if (!ipv4.empty() && ipv4 != "127.0.0.1") {
        appendPart(formatPeerAddress(ipv4, port));
    }
    if (!ipv6.empty()) {
        appendPart(formatPeerAddress(ipv6, port));
    }
    if (!computerName.empty()) {
        appendPart(formatPeerAddress(computerName, port));
    }
    if (line.empty() && !ipv4.empty()) {
        line = formatPeerAddress(ipv4, port);
    }
    return line;
}

}  // namespace clipsync
