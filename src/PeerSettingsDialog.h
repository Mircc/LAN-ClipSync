#pragma once

#include <windows.h>

namespace clipsync {

class NetworkManager;

class PeerSettingsDialog {
public:
    static void show(HWND owner, NetworkManager* network);
};

}  // namespace clipsync
