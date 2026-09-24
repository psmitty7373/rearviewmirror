#pragma once
#include "common.h"

namespace rvm {

// Everything needed to bring a mirror back after a restart.
// What a mirror shows.
enum class SourceKind {
    Window,    // One application window, found again by its identity.
    Desktop,   // Every monitor, as one picture; always available.
};

struct MirrorState {
    SourceKind source = SourceKind::Window;

    // Window handles are not stable across runs, so a mirror is re-bound by
    // matching this identity against the live windows at startup.
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

    // Mirrors made from the same window share a group, so they bind to one
    // window together; different groups never share a window. Never 0 once
    // loaded or created.
    uint32_t  group        = 0;
};

// A group number for a mirror with nothing better to go on: the same for the
// same saved identity, so mirrors saved from one window still end up together.
uint32_t GroupFromIdentity(const MirrorState& state);

std::wstring ConfigDir();
std::wstring ConfigPath();

// Writes to a sibling temp file, then renames over the target, as UTF-16 with
// a BOM: a crash mid-write leaves the previous file intact.
bool WriteTextAtomically(const std::wstring& path, const std::wstring& text);

std::vector<MirrorState> LoadMirrorStates();

// Writes the whole file atomically, as UTF-16 with a BOM so titles in any
// script round-trip.
bool SaveMirrorStates(const std::vector<MirrorState>& states);

// Record which window a mirror is watching, so it can be found again later.
void FillIdentity(HWND hwnd, MirrorState& state);

// The live window that best matches a saved identity, or nullptr. Windows in
// `exclude` are never chosen, so two saved mirrors cannot share one source.
HWND FindMatchingWindow(const MirrorState& state, const std::vector<HWND>& exclude = {});

// Nudge a saved rect back onto a currently-connected monitor.
RECT ClampToVisibleMonitor(const RECT& rect);

}  // namespace rvm
