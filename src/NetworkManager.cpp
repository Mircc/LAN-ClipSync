#include "NetworkManager.h"

#include "JsonUtil.h"
#include "Logger.h"
#include "PeerConfig.h"
#include "ThreadUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace clipsync {

namespace {

struct TcpClientCtx {
    NetworkManager* mgr;
    int socket;
};

using SocketHandle = SOCKET;

SocketHandle toSocket(uintptr_t v) { return static_cast<SocketHandle>(v); }

int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string peerKey(const PeerEndpoint& peer) {
    return peer.keyAddress() + ":" + std::to_string(peer.tcpPort);
}

bool endpointRefersToSelf(const PeerEndpoint& peer, uint16_t listenPort, const std::string& localV4,
                          const std::string& localV6, const std::string& localName) {
    if (peer.tcpPort != listenPort) {
        return false;
    }
    if (!peer.host.empty() && !localName.empty() &&
        _stricmp(peer.host.c_str(), localName.c_str()) == 0) {
        return true;
    }
    if (!peer.ip.empty()) {
        if (peer.ip == localV4) {
            return true;
        }
        if (!localV6.empty() && peer.ip == localV6) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string PeerEndpoint::keyAddress() const { return host.empty() ? ip : host; }

bool PeerEndpoint::hasAddress() const { return !keyAddress().empty(); }

std::wstring peerLinkStateText(PeerLinkState state) {
    switch (state) {
        case PeerLinkState::Online:
            return L"已连接";
        case PeerLinkState::Offline:
            return L"未连接";
        case PeerLinkState::Checking:
            return L"检测中";
        default:
            return L"—";
    }
}

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() { stop(); }

bool NetworkManager::bindListenSocket(uint16_t port) {
    sockaddr_in6 tcpAddr{};
    tcpAddr.sin6_family = AF_INET6;
    tcpAddr.sin6_addr = in6addr_any;
    tcpAddr.sin6_port = htons(port);
    if (bind(toSocket(tcpSocket_), reinterpret_cast<sockaddr*>(&tcpAddr), sizeof(tcpAddr)) != 0) {
        return false;
    }
    if (listen(toSocket(tcpSocket_), SOMAXCONN) != 0) {
        return false;
    }
    listenPort_ = port;
    return true;
}

uint16_t NetworkManager::pickListenPort() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(kTcpPortMin, kTcpPortMax);
    return static_cast<uint16_t>(dist(gen));
}

bool NetworkManager::start() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Logger::error(L"WSAStartup 失败");
        return false;
    }

    tcpSocket_ = static_cast<uintptr_t>(socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
    if (toSocket(tcpSocket_) == INVALID_SOCKET) {
        Logger::error(L"创建 TCP 套接字失败");
        return false;
    }

    DWORD dualStack = 0;
    setsockopt(toSocket(tcpSocket_), IPPROTO_IPV6, IPV6_V6ONLY,
               reinterpret_cast<const char*>(&dualStack), sizeof(dualStack));

    BOOL reuse = TRUE;
    setsockopt(toSocket(tcpSocket_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    AppPeerConfig config;
    const bool hasConfigFile = loadPeerConfig(config);
    savedListenPort_ = config.listenPort;

    std::vector<uint16_t> candidates;
    if (config.listenPort != 0) {
        candidates.push_back(config.listenPort);
    }
    for (const uint16_t port : kDefaultListenPorts) {
        if (port != config.listenPort) {
            candidates.push_back(port);
        }
    }
    for (int i = 0; i < 24; ++i) {
        candidates.push_back(pickListenPort());
    }

    bool bound = false;
    uint16_t boundPort = 0;
    for (const uint16_t port : candidates) {
        if (bindListenSocket(port)) {
            bound = true;
            boundPort = port;
            break;
        }
        if (toSocket(tcpSocket_) != INVALID_SOCKET) {
            closesocket(toSocket(tcpSocket_));
        }
        tcpSocket_ = static_cast<uintptr_t>(socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
        if (toSocket(tcpSocket_) == INVALID_SOCKET) {
            break;
        }
        setsockopt(toSocket(tcpSocket_), IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&dualStack), sizeof(dualStack));
        setsockopt(toSocket(tcpSocket_), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }

    if (!bound) {
        Logger::error(L"TCP 监听端口绑定失败");
        return false;
    }

    if (savedListenPort_ != 0 && boundPort == savedListenPort_) {
        listenPortReused_ = true;
        std::wstringstream ss;
        ss << L"复用上次监听端口 " << boundPort;
        Logger::info(ss.str());
    } else if (savedListenPort_ != 0 && boundPort != savedListenPort_) {
        listenPortChanged_ = true;
        std::wstringstream ss;
        ss << L"上次端口 " << savedListenPort_ << L" 已被占用，已改用 " << boundPort;
        Logger::warn(ss.str());
    } else if (!hasConfigFile) {
        std::wstringstream ss;
        ss << L"首次分配监听端口 " << boundPort;
        Logger::info(ss.str());
    }

    AppPeerConfig saveCfg;
    saveCfg.listenPort = listenPort_;
    saveCfg.peers = config.peers;
    savePeerConfig(saveCfg);

    setPeers(config.peers, false);

    running_ = true;
    tcpThread_ = createSmallStackThread(&NetworkManager::tcpAcceptThreadProc, this, 192 * 1024);
    if (!tcpThread_) {
        Logger::error(L"创建 TCP 接受线程失败");
        running_ = false;
        return false;
    }

    std::wstringstream ss;
    ss << L"网络已启动，本机地址 " << utf8ToWide(localIPv4()) << L":" << listenPort_
       << L"（请在对方添加此地址）";
    Logger::info(ss.str());
    return true;
}

void NetworkManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (toSocket(tcpSocket_) != INVALID_SOCKET) {
        closesocket(toSocket(tcpSocket_));
        tcpSocket_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }

    if (tcpThread_) {
        WaitForSingleObject(tcpThread_, 5000);
        CloseHandle(tcpThread_);
        tcpThread_ = nullptr;
    }

    WSACleanup();
}

size_t NetworkManager::peerCount() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    return peers_.size();
}

std::string NetworkManager::localIPv4() const { return queryLocalIPv4(); }

std::string NetworkManager::localIPv6() const { return queryLocalIPv6(); }

uint16_t NetworkManager::listenPort() const { return listenPort_; }

uint16_t NetworkManager::savedListenPort() const { return savedListenPort_; }

bool NetworkManager::listenPortReusedFromConfig() const { return listenPortReused_; }

bool NetworkManager::listenPortChangedFromSaved() const { return listenPortChanged_; }

std::vector<PeerEndpoint> NetworkManager::getPeers() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    std::vector<PeerEndpoint> out;
    out.reserve(peers_.size());
    for (const auto& kv : peers_) {
        out.push_back(kv.second);
    }
    return out;
}

void NetworkManager::setPeers(const std::vector<PeerEndpoint>& peers, bool persistConfig) {
    const std::string localIp = queryLocalIPv4();
    std::unordered_map<std::string, PeerEndpoint> next;
    next.reserve(peers.size());
    const std::string localName = localComputerNameUtf8();
    const std::string localV6 = queryLocalIPv6();
    for (const auto& peer : peers) {
        if (!peer.hasAddress() || peer.tcpPort == 0) {
            continue;
        }
        if (endpointRefersToSelf(peer, listenPort_, localIp, localV6, localName)) {
            continue;
        }
        next[peerKey(peer)] = peer;
    }

    std::vector<PeerEndpoint> stored;
    stored.reserve(next.size());

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        changed = peers_.size() != next.size();
        if (!changed) {
            for (const auto& kv : next) {
                if (peers_.find(kv.first) == peers_.end()) {
                    changed = true;
                    break;
                }
            }
        }
        peers_ = std::move(next);
        for (const auto& kv : peers_) {
            stored.push_back(kv.second);
        }
    }

    if (persistConfig && listenPort_ != 0) {
        AppPeerConfig cfg;
        cfg.listenPort = listenPort_;
        cfg.peers = stored;
        if (savePeerConfig(cfg)) {
            markPeerSetupCompleted();
            Logger::info(L"对端配置已保存");
        }
    }

    if (changed && onPeersChanged_) {
        onPeersChanged_(peerCount());
    }
    refreshPeerStatusesAsync();
}

bool NetworkManager::tryConnectAny(const std::vector<PeerSocketTarget>& targets, int timeoutMs) {
    uintptr_t sock = static_cast<uintptr_t>(INVALID_SOCKET);
    return tryConnectAny(targets, timeoutMs, sock);
}

bool NetworkManager::tryConnectAny(const std::vector<PeerSocketTarget>& targets, int timeoutMs,
                                 uintptr_t& outSocket) {
    outSocket = static_cast<uintptr_t>(INVALID_SOCKET);
    for (const auto& target : targets) {
        SocketHandle sock =
            socket(target.addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            continue;
        }

        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);

        const int connectResult = connect(sock, reinterpret_cast<const sockaddr*>(&target.addr),
                                          target.addrLen);
        if (connectResult == 0) {
            u_long blocking = 0;
            ioctlsocket(sock, FIONBIO, &blocking);
            outSocket = static_cast<uintptr_t>(sock);
            return true;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(sock);
            continue;
        }

        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        const int sel = select(0, nullptr, &writefds, nullptr, &tv);
        if (sel <= 0) {
            closesocket(sock);
            continue;
        }

        int soError = 0;
        int len = sizeof(soError);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (soError != 0) {
            closesocket(sock);
            continue;
        }

        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        outSocket = static_cast<uintptr_t>(sock);
        return true;
    }
    return false;
}

bool NetworkManager::probePeer(const PeerEndpoint& peer, int timeoutMs) const {
    std::vector<PeerSocketTarget> targets;
    if (!resolvePeerEndpointTargets(peer, targets)) {
        return false;
    }
    return tryConnectAny(targets, timeoutMs);
}

bool NetworkManager::discoverPeerPort(PeerEndpoint& peer, int timeoutMs) const {
    if (!peer.hasAddress()) {
        return false;
    }
    const uint16_t saved = peer.tcpPort;
    for (const uint16_t port : kDefaultListenPorts) {
        peer.tcpPort = port;
        if (probePeer(peer, timeoutMs)) {
            return true;
        }
    }
    peer.tcpPort = saved;
    return false;
}

std::vector<PeerStatus> NetworkManager::probePeers(const std::vector<PeerEndpoint>& peers,
                                                   int timeoutMs) const {
    std::vector<PeerStatus> result;
    result.reserve(peers.size());
    for (const auto& peer : peers) {
        PeerStatus row;
        row.endpoint = peer;
        row.state = PeerLinkState::Checking;
        row.state = probePeer(peer, timeoutMs) ? PeerLinkState::Online : PeerLinkState::Offline;
        result.push_back(row);
    }
    return result;
}

std::vector<PeerStatus> NetworkManager::getPeerStatuses() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return peerStatuses_;
}

void NetworkManager::refreshPeerStatusesAsync() { runProbeAndNotify(); }

size_t NetworkManager::onlinePeerCount() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    size_t n = 0;
    for (const auto& row : peerStatuses_) {
        if (row.state == PeerLinkState::Online) {
            ++n;
        }
    }
    return n;
}

void NetworkManager::runProbeAndNotify() {
    if (!running_) {
        return;
    }
    const auto peers = getPeers();
    if (peers.empty()) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        peerStatuses_.clear();
    } else {
        const auto statuses = probePeers(peers, 1000);
        std::lock_guard<std::mutex> lock(statusMutex_);
        peerStatuses_ = statuses;
    }
    if (onPeerStatusChanged_) {
        onPeerStatusChanged_();
    }
}

DWORD WINAPI NetworkManager::tcpAcceptThreadProc(LPVOID param) {
    static_cast<NetworkManager*>(param)->tcpAcceptLoop();
    return 0;
}

DWORD WINAPI NetworkManager::tcpClientThreadProc(LPVOID param) {
    auto* ctx = static_cast<TcpClientCtx*>(param);
    ctx->mgr->handleTcpClient(ctx->socket);
    delete ctx;
    return 0;
}

void NetworkManager::setOnTextReceived(TextReceivedCallback cb) {
    onTextReceived_ = std::move(cb);
}

void NetworkManager::setOnPeersChanged(PeersChangedCallback cb) {
    onPeersChanged_ = std::move(cb);
}

void NetworkManager::setOnPeerStatusChanged(PeerStatusCallback cb) {
    onPeerStatusChanged_ = std::move(cb);
}

void NetworkManager::tcpAcceptLoop() {
    while (running_) {
        sockaddr_storage clientAddr{};
        int len = sizeof(clientAddr);
        const SocketHandle client =
            accept(toSocket(tcpSocket_), reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (client == INVALID_SOCKET) {
            if (!running_) {
                break;
            }
            Sleep(50);
            continue;
        }
        auto* ctx = new TcpClientCtx{this, static_cast<int>(client)};
        HANDLE th = createSmallStackThread(&NetworkManager::tcpClientThreadProc, ctx, 128 * 1024);
        if (th) {
            CloseHandle(th);
        } else {
            handleTcpClient(ctx->socket);
            delete ctx;
        }
    }
}

bool NetworkManager::recvFramed(int socket, std::string& out) {
    uint32_t netLen = 0;
    int received = 0;
    while (received < 4) {
        const int n = recv(socket, reinterpret_cast<char*>(&netLen) + received, 4 - received, 0);
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    const uint32_t len = ntohl(netLen);
    if (len == 0 || len > 64 * 1024) {
        return false;
    }
    out.resize(len);
    received = 0;
    while (received < static_cast<int>(len)) {
        const int n = recv(socket, out.data() + received, static_cast<int>(len) - received, 0);
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    return true;
}

bool NetworkManager::sendFramed(int socket, const std::string& payload) {
    const uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    if (send(socket, reinterpret_cast<const char*>(&len), 4, 0) != 4) {
        return false;
    }
    int sent = 0;
    while (sent < static_cast<int>(payload.size())) {
        const int n =
            send(socket, payload.data() + sent, static_cast<int>(payload.size()) - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += n;
    }
    return true;
}

void NetworkManager::handleTcpClient(int clientSocket) {
    std::string json;
    if (!recvFramed(clientSocket, json)) {
        closesocket(static_cast<SocketHandle>(clientSocket));
        return;
    }
    closesocket(static_cast<SocketHandle>(clientSocket));

    const auto msg = parseClipMessage(json);
    if (!msg || msg->type != "text") {
        return;
    }

    std::string payload = msg->payload;

    if (onTextReceived_) {
        onTextReceived_(payload);
    }
}

bool NetworkManager::sendJsonToPeer(const PeerEndpoint& peer, const std::string& json) {
    std::vector<PeerSocketTarget> targets;
    if (!resolvePeerEndpointTargets(peer, targets)) {
        return false;
    }

    uintptr_t sock = static_cast<uintptr_t>(INVALID_SOCKET);
    if (!tryConnectAny(targets, 3000, sock)) {
        return false;
    }

    const bool ok = sendFramed(static_cast<int>(sock), json);
    closesocket(static_cast<SocketHandle>(sock));
    return ok;
}

bool NetworkManager::broadcastText(const std::string& utf8Text) {
    const std::string json = buildTextMessage(utf8Text, nowUnixSeconds());

    std::vector<PeerEndpoint> peersCopy;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peersCopy.reserve(peers_.size());
        for (const auto& kv : peers_) {
            peersCopy.push_back(kv.second);
        }
    }

    if (peersCopy.empty()) {
        Logger::info(L"未配置对端，跳过发送（请在托盘菜单中设置对端 IP）");
        return false;
    }

    int success = 0;
    for (const auto& peer : peersCopy) {
        if (sendJsonToPeer(peer, json)) {
            ++success;
        }
    }

    std::wstringstream ss;
    ss << L"已向 " << success << L"/" << peersCopy.size() << L" 个对端发送剪切板文本";
    Logger::info(ss.str());
    return success > 0;
}

std::string NetworkManager::queryLocalIPv4() {
    ULONG size = 4096;
    BYTE stackBuf[4096];
    std::vector<BYTE> heapBuf;
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(stackBuf);
    ULONG err = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                GAA_FLAG_SKIP_DNS_SERVER,
                                     nullptr, adapters, &size);
    if (err == ERROR_BUFFER_OVERFLOW) {
        heapBuf.resize(size);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(heapBuf.data());
        err = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, adapters, &size);
    }
    if (err != NO_ERROR) {
        return "127.0.0.1";
    }

    for (auto adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (auto addr = adapter->FirstUnicastAddress; addr; addr = addr->Next) {
            if (addr->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* sin = reinterpret_cast<sockaddr_in*>(addr->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            if (strcmp(ip, "127.0.0.1") != 0) {
                return ip;
            }
        }
    }
    return "127.0.0.1";
}

std::string NetworkManager::queryLocalIPv6() {
    ULONG size = 4096;
    BYTE stackBuf[4096];
    std::vector<BYTE> heapBuf;
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(stackBuf);
    ULONG err = GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                 GAA_FLAG_SKIP_DNS_SERVER,
                                     nullptr, adapters, &size);
    if (err == ERROR_BUFFER_OVERFLOW) {
        heapBuf.resize(size);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(heapBuf.data());
        err = GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, adapters, &size);
    }
    if (err != NO_ERROR) {
        return {};
    }

    std::string linkLocal;
    for (auto adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (auto addr = adapter->FirstUnicastAddress; addr; addr = addr->Next) {
            if (addr->Address.lpSockaddr->sa_family != AF_INET6) {
                continue;
            }
            const auto* sin6 = reinterpret_cast<sockaddr_in6*>(addr->Address.lpSockaddr);
            char ip[INET6_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, ip, sizeof(ip)) == nullptr) {
                continue;
            }
            if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) {
                continue;
            }
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
                if (linkLocal.empty()) {
                    linkLocal = ip;
                }
                continue;
            }
            return ip;
        }
    }
    return linkLocal;
}

}  // namespace clipsync
