#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace clipsync {

constexpr uint16_t kTcpPortMin = 45000;
constexpr uint16_t kTcpPortMax = 65000;

/** 首次绑定与对端探测时优先尝试的约定端口 */
constexpr uint16_t kDefaultListenPorts[3] = {12123, 12345, 12306};

struct PeerEndpoint {
    std::string host;  // 计算机名 / 域 FQDN（配置持久化，连接时解析为 IP）
    std::string ip;    // 字面 IPv4/IPv6（与 host 二选一）
    uint16_t tcpPort = 0;

    std::string keyAddress() const;
    bool hasAddress() const;
};

struct PeerSocketTarget {
    sockaddr_storage addr{};
    int addrLen = 0;
};

enum class PeerLinkState { Unknown, Checking, Online, Offline };

struct PeerStatus {
    PeerEndpoint endpoint;
    PeerLinkState state = PeerLinkState::Unknown;
};

std::wstring peerLinkStateText(PeerLinkState state);

class NetworkManager {
public:
    using TextReceivedCallback = std::function<void(const std::string& utf8Text)>;
    using PeersChangedCallback = std::function<void(size_t peerCount)>;
    using PeerStatusCallback = std::function<void()>;

    NetworkManager();
    ~NetworkManager();

    bool start();
    void stop();

    bool broadcastText(const std::string& utf8Text);
    size_t peerCount() const;

    std::string localIPv4() const;
    std::string localIPv6() const;
    uint16_t listenPort() const;
    uint16_t savedListenPort() const;
    bool listenPortReusedFromConfig() const;
    bool listenPortChangedFromSaved() const;

    std::vector<PeerEndpoint> getPeers() const;
    void setPeers(const std::vector<PeerEndpoint>& peers, bool persistConfig = true);

    bool probePeer(const PeerEndpoint& peer, int timeoutMs = 2000) const;
    /** 在 kDefaultListenPorts 上探测，成功则写入 peer.tcpPort */
    bool discoverPeerPort(PeerEndpoint& peer, int timeoutMs = 2000) const;
    std::vector<PeerStatus> probePeers(const std::vector<PeerEndpoint>& peers,
                                       int timeoutMs = 2000) const;
    std::vector<PeerStatus> getPeerStatuses() const;
    void refreshPeerStatusesAsync();
    void runProbeAndNotify();
    size_t onlinePeerCount() const;

    void setOnTextReceived(TextReceivedCallback cb);
    void setOnPeersChanged(PeersChangedCallback cb);
    void setOnPeerStatusChanged(PeerStatusCallback cb);

private:
    static DWORD WINAPI tcpAcceptThreadProc(LPVOID param);
    static DWORD WINAPI tcpClientThreadProc(LPVOID param);
    void tcpAcceptLoop();
    void handleTcpClient(int clientSocket);

    bool bindListenSocket(uint16_t port);
    uint16_t pickListenPort() const;

    bool sendJsonToPeer(const PeerEndpoint& peer, const std::string& json);
    static bool sendFramed(int socket, const std::string& payload);
    static bool recvFramed(int socket, std::string& out);
    static std::string queryLocalIPv4();
    static std::string queryLocalIPv6();

    static bool tryConnectAny(const std::vector<PeerSocketTarget>& targets, int timeoutMs,
                              uintptr_t& outSocket);
    static bool tryConnectAny(const std::vector<PeerSocketTarget>& targets, int timeoutMs);

    uint16_t listenPort_ = 0;
    uint16_t savedListenPort_ = 0;
    bool listenPortReused_ = false;
    bool listenPortChanged_ = false;

    std::atomic<bool> running_{false};

    uintptr_t tcpSocket_ = static_cast<uintptr_t>(-1);

    mutable std::mutex peersMutex_;
    std::unordered_map<std::string, PeerEndpoint> peers_;

    HANDLE tcpThread_ = nullptr;

    mutable std::mutex statusMutex_;
    std::vector<PeerStatus> peerStatuses_;

    TextReceivedCallback onTextReceived_;
    PeersChangedCallback onPeersChanged_;
    PeerStatusCallback onPeerStatusChanged_;
};

}  // namespace clipsync
