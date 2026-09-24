#pragma once
#include "common.h"

namespace rvm {

struct ConnectSettings {
    std::wstring host;
    uint16_t     port = 5901;
    std::wstring key;
};

// Where one mirror sits on the canvas, in device-independent pixels, and how
// its pop-out window was last arranged. Keyed by server label (host:port) and
// mirror id.
struct TileLayout {
    std::wstring server;
    uint32_t mirror = 0;
    int x = 0, y = 0, w = 320, h = 180;

    bool  popped = false;
    RECT  popRect{};              // Screen pixels; empty means "place it".
    float popOpacity = 1.0f;
    bool  popClickThrough = false;
    bool  popAspectLocked = true;
};

constexpr size_t kMaxSavedServers = 16;
constexpr size_t kMaxSavedTiles   = 128;
constexpr int    kMaxCanvasDip = 16384;
constexpr int    kMinBoxW = 64;
constexpr int    kMinBoxH = 40;

// Remembered in client.ini beside the app's own settings; keys are
// DPAPI-protected to the current user.
struct ClientConfig {
    std::vector<ConnectSettings> servers;
    std::vector<TileLayout>      tiles;
    bool sidebarHidden = false;
};

ClientConfig LoadClientConfig();
bool SaveClientConfig(const ClientConfig& config);

}  // namespace rvm
