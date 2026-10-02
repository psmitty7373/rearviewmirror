#pragma once
#include "common.h"

namespace rvm {

// What a mirror shows.
enum class SourceKind {
    Window,    // One application window, found again by its identity.
    Desktop,   // Every monitor, as one picture; always available.
};

// Everything needed to bring a mirror back after a restart.
struct MirrorState {
    SourceKind source = SourceKind::Window;

    // Handles do not survive restarts; this identity finds the window again.
    std::wstring exeName;
    std::wstring className;
    std::wstring title;

    RECT crop{};       // Capture-texture pixels.
    SIZE baseSize{};   // The capture size the crop was chosen against.
    RECT placement{};  // Mirror window rect in screen pixels; empty means "auto".

    float     opacity      = 1.0f;
    bool      aspectLocked = true;
    TrackMode track        = TrackMode::Anchored;

    // A disabled mirror keeps its place in the list but stops capturing.
    bool      enabled      = true;

    // The mouse passes through the window as though it were not there.
    bool      clickThrough = false;

    // No window on screen, but capture (and so streaming) carries on.
    bool      hidden       = false;

    // Mirrors of one window share a group and bind together; groups never
    // share a window. Never 0 once loaded or created.
    uint32_t  group        = 0;
};

// A fallback group, the same for the same identity: mirrors of one window stay together.
uint32_t GroupFromIdentity(const MirrorState& state);

// Roaming AppData\RearViewMirror (else the exe's folder), unless SetConfigDir moved it.
std::wstring ConfigDir();
std::wstring ConfigPath();

// For processes with no user, like the login service. Call before any thread
// starts or anything reads ConfigDir().
void SetConfigDir(const std::wstring& dir);

// UTF-16 with a BOM, via a temp file and rename: a crash keeps the old file.
bool WriteTextAtomically(const std::wstring& path, const std::wstring& text);

// The most mirrors the file holds, waiting ones included.
constexpr int kMaxMirrors = 32;

std::vector<MirrorState> LoadMirrorStates();

// Skipped if the file would not change. UI thread only.
bool SaveMirrorStates(const std::vector<MirrorState>& states);

// Record which window a mirror is watching, so it can be found again later.
void FillIdentity(HWND hwnd, MirrorState& state);

// Executable names by process id, for the searches of one pass to share.
using ExeNameCache = std::vector<std::pair<DWORD, std::wstring>>;

// The live window best matching a saved identity, never one in `exclude`.
HWND FindMatchingWindow(const MirrorState& state, const std::vector<HWND>& exclude = {},
                        ExeNameCache* exes = nullptr);

// Nudge a saved rect back onto a currently-connected monitor.
RECT ClampToVisibleMonitor(const RECT& rect);

}  // namespace rvm
