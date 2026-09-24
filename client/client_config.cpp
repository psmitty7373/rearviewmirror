#include "client_config.h"
#include "net/crypto.h"
#include "net/protocol.h"
#include "persist.h"

namespace rvm {

namespace {

std::wstring SettingsPath() {
    return ConfigDir() + L"\\client.ini";
}

std::wstring ReadStr(const std::wstring& section, const wchar_t* key, const std::wstring& path) {
    wchar_t buffer[4096]{};
    GetPrivateProfileStringW(section.c_str(), key, L"", buffer, ARRAYSIZE(buffer), path.c_str());
    return buffer;
}

int ReadInt(const std::wstring& section, const wchar_t* key, int fallback, const std::wstring& path) {
    // GetPrivateProfileInt cannot return a negative number, and screen
    // coordinates on a left-hand monitor are negative.
    const std::wstring s = ReadStr(section, key, path);
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    const long v = wcstol(s.c_str(), &end, 10);
    return (end == s.c_str()) ? fallback : static_cast<int>(v);
}

// A host name is one line of the file; nothing that could break that.
std::wstring OneLine(std::wstring s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c < 0x20; }), s.end());
    return s;
}

void Line(std::wstring& text, const wchar_t* key, const std::wstring& value) {
    text += key;
    text += L"=";
    text += value;
    text += L"\r\n";
}
void Line(std::wstring& text, const wchar_t* key, int value) {
    Line(text, key, std::to_wstring(value));
}

bool ReadServer(const std::wstring& section, const std::wstring& path, ConnectSettings& s) {
    s.host = OneLine(ReadStr(section, L"Host", path));
    if (s.host.empty()) return false;
    s.port = static_cast<uint16_t>(ClampI(ReadInt(section, L"Port", net::kDefaultPort, path), 1, 65535));
    const std::vector<uint8_t> blob = net::FromHex(ReadStr(section, L"KeyBlob", path));
    std::wstring secret;
    if (net::UnprotectSecret(blob, secret)) {
        s.key = std::move(secret);
    } else if (!blob.empty()) {
        s.lockedKey = blob;
        Log(L"client: the saved key for %s could not be decrypted by this Windows account",
            s.host.c_str());
    }
    return true;
}

// `unit`: device-independent pixels per stored unit. Files from the grid
// layout stored 20-pixel cells.
bool ReadTile(const std::wstring& section, const std::wstring& path, int unit, TileLayout& t) {
    t.server = OneLine(ReadStr(section, L"Server", path));
    t.mirror = static_cast<uint32_t>(ReadInt(section, L"Mirror", 0, path));
    if (t.server.empty() || t.mirror == 0) return false;
    const auto read = [&](const wchar_t* key, int fallback) {
        return ClampI(ReadInt(section, key, fallback, path), 0, kMaxCanvasDip) * unit;
    };
    t.w = ClampI(read(L"W", 16), kMinBoxW, kMaxCanvasDip);
    t.h = ClampI(read(L"H", 9), kMinBoxH, kMaxCanvasDip);
    t.x = ClampI(read(L"X", 0), 0, kMaxCanvasDip - t.w);
    t.y = ClampI(read(L"Y", 0), 0, kMaxCanvasDip - t.h);
    t.popped = ReadInt(section, L"Popped", 0, path) != 0;
    t.popRect.left   = ClampI(ReadInt(section, L"PopX", 0, path), -kMaxExtent, kMaxExtent);
    t.popRect.top    = ClampI(ReadInt(section, L"PopY", 0, path), -kMaxExtent, kMaxExtent);
    t.popRect.right  = t.popRect.left + ClampI(ReadInt(section, L"PopW", 0, path), 0, kMaxExtent);
    t.popRect.bottom = t.popRect.top  + ClampI(ReadInt(section, L"PopH", 0, path), 0, kMaxExtent);
    t.popOpacity = ClampI(ReadInt(section, L"PopOpacity", 100, path), 5, 100) / 100.0f;
    t.popClickThrough = ReadInt(section, L"PopClickThrough", 0, path) != 0;
    t.popAspectLocked = ReadInt(section, L"PopAspectLock", 1, path) != 0;
    return true;
}

}  // namespace

ClientConfig LoadClientConfig() {
    const std::wstring path = SettingsPath();
    ClientConfig config;
    config.sidebarHidden = ReadInt(L"Client", L"SidebarHidden", 0, path) != 0;

    const int ww = ClampI(ReadInt(L"Client", L"WindowW", 0, path), 0, kMaxExtent);
    const int wh = ClampI(ReadInt(L"Client", L"WindowH", 0, path), 0, kMaxExtent);
    if (ww >= 320 && wh >= 240) {
        config.window.left   = ClampI(ReadInt(L"Client", L"WindowX", 0, path), -kMaxExtent, kMaxExtent);
        config.window.top    = ClampI(ReadInt(L"Client", L"WindowY", 0, path), -kMaxExtent, kMaxExtent);
        config.window.right  = config.window.left + ww;
        config.window.bottom = config.window.top + wh;
        config.windowMaximized = ReadInt(L"Client", L"WindowMax", 0, path) != 0;
    }

    const int servers = ClampI(ReadInt(L"Client", L"Servers", -1, path), -1,
                               static_cast<int>(kMaxSavedServers));
    if (servers < 0) {
        // Older files remembered a single server under [Client].
        ConnectSettings s;
        if (ReadServer(L"Client", path, s)) config.servers.push_back(std::move(s));
    }
    for (int i = 1; i <= servers; ++i) {
        ConnectSettings s;
        if (ReadServer(L"Server" + std::to_wstring(i), path, s)) config.servers.push_back(std::move(s));
    }

    const int tiles = ClampI(ReadInt(L"Client", L"Tiles", 0, path), 0, static_cast<int>(kMaxSavedTiles));
    const bool pixels = ReadStr(L"Client", L"TileUnits", path) == L"dip";
    for (int i = 1; i <= tiles; ++i) {
        TileLayout t;
        if (ReadTile(L"Tile" + std::to_wstring(i), path, pixels ? 1 : 20, t)) {
            config.tiles.push_back(std::move(t));
        }
    }
    return config;
}

bool SaveClientConfig(const ClientConfig& config) {
    const size_t servers = (std::min)(config.servers.size(), kMaxSavedServers);
    const size_t tiles   = (std::min)(config.tiles.size(), kMaxSavedTiles);

    std::wstring text = L"[Client]\r\n";
    Line(text, L"SidebarHidden", config.sidebarHidden ? 1 : 0);
    if (RectW(config.window) > 0 && RectH(config.window) > 0) {
        Line(text, L"WindowX", config.window.left);
        Line(text, L"WindowY", config.window.top);
        Line(text, L"WindowW", RectW(config.window));
        Line(text, L"WindowH", RectH(config.window));
        Line(text, L"WindowMax", config.windowMaximized ? 1 : 0);
    }
    Line(text, L"Servers", static_cast<int>(servers));
    Line(text, L"Tiles", static_cast<int>(tiles));
    Line(text, L"TileUnits", std::wstring(L"dip"));

    for (size_t i = 0; i < servers; ++i) {
        const ConnectSettings& s = config.servers[i];
        std::vector<uint8_t> blob = s.lockedKey;
        if (!s.key.empty() && !net::ProtectSecret(s.key, blob)) return false;
        text += L"\r\n[Server" + std::to_wstring(i + 1) + L"]\r\n";
        Line(text, L"Host", OneLine(s.host));
        Line(text, L"Port", s.port);
        Line(text, L"KeyBlob", net::ToHex(blob));
    }

    for (size_t i = 0; i < tiles; ++i) {
        const TileLayout& t = config.tiles[i];
        text += L"\r\n[Tile" + std::to_wstring(i + 1) + L"]\r\n";
        Line(text, L"Server", OneLine(t.server));
        Line(text, L"Mirror", static_cast<int>(t.mirror));
        Line(text, L"X", t.x);
        Line(text, L"Y", t.y);
        Line(text, L"W", t.w);
        Line(text, L"H", t.h);
        Line(text, L"Popped", t.popped ? 1 : 0);
        Line(text, L"PopX", t.popRect.left);
        Line(text, L"PopY", t.popRect.top);
        Line(text, L"PopW", RectW(t.popRect));
        Line(text, L"PopH", RectH(t.popRect));
        Line(text, L"PopOpacity", static_cast<int>(std::lround(t.popOpacity * 100.0f)));
        Line(text, L"PopClickThrough", t.popClickThrough ? 1 : 0);
        Line(text, L"PopAspectLock", t.popAspectLocked ? 1 : 0);
    }
    return WriteTextAtomically(SettingsPath(), text);
}

}  // namespace rvm
