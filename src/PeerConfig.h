#pragma once

#include "NetworkManager.h"

#include <string>
#include <vector>

namespace clipsync {

struct AppPeerConfig {
    uint16_t listenPort = 0;
    std::vector<PeerEndpoint> peers;
};

std::wstring peerConfigFilePath();
bool peerConfigFileExists();
bool loadPeerConfig(AppPeerConfig& out);
bool savePeerConfig(const AppPeerConfig& config);

bool isPeerSetupCompleted();
void markPeerSetupCompleted();

bool parsePeerAddress(const std::wstring& input, PeerEndpoint& out);
std::wstring formatPeerAddress(const PeerEndpoint& peer);
std::wstring formatPeerAddress(const std::string& hostOrIp, uint16_t port);
std::wstring formatLocalListenAddresses(const std::string& ipv4, const std::string& ipv6,
                                        const std::string& computerName, uint16_t port);

bool isLiteralIPv6String(const std::string& s);
bool resolvePeerEndpointTargets(const PeerEndpoint& peer,
                                std::vector<PeerSocketTarget>& targets);
std::string localComputerNameUtf8();

}  // namespace clipsync
