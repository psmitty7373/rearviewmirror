#pragma once
#include "common.h"

namespace rvm {

// How much work the encoder puts into each frame. NVIDIA's encoder has three
// distinct steps; measured at 4096x1152 on text, Fastest took half the time of
// Balanced per frame at no loss, and Quality took a third longer.
enum class EncoderPreset : int {
    Balanced = 0,   // The encoder's own default.
    Fastest  = 1,
    Quality  = 2,
};

struct StreamSettings {
    bool         enabled = false;   // Running; restored at the next launch.
    uint16_t     port = 5901;
    std::wstring key;               // The pre-shared passphrase.
    // A saved key this Windows account could not decrypt (the file came from
    // another user or PC). Kept as is, so saving does not erase it.
    std::vector<uint8_t> lockedKey;
    UINT         bitrateKbps = 8000;
    UINT         fps = 60;          // Upper limit per stream; a still window sends fewer.
    EncoderPreset preset = EncoderPreset::Balanced;
};

constexpr UINT kMinStreamFps = 1;
constexpr UINT kMaxStreamFps = 240;

// Kept in its own file beside mirrors.ini; the key is DPAPI-protected to the
// current Windows user rather than stored in the clear.
StreamSettings LoadStreamSettings();
bool SaveStreamSettings(const StreamSettings& settings);

// What the dialog's Start/Stop button and live status line act on.
struct StreamControl {
    // Saves `settings` and starts the server. False with `error` set if it
    // could not start.
    std::function<bool(const StreamSettings& settings, std::wstring& error)> start;
    std::function<void()> stop;
    std::function<bool()> running;
    std::function<std::wstring()> status;   // One line: state, port, clients.
};

// Modal editor. Start and Stop act at once; OK keeps the edited fields (and
// restarts a running server if they changed), Cancel discards the edits.
// Returns true on OK with `settings` updated. If the dialog is already open
// it is brought to the front instead.
bool ShowStreamSettingsDialog(HWND owner, StreamSettings& settings, const StreamControl& control);

}  // namespace rvm
