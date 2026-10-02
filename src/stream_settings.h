#pragma once
#include "common.h"

namespace rvm {

// How much work the encoder puts into each frame.
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

// The machine store when this account may use it (see below), otherwise this
// user's stream.ini beside mirrors.ini, with the key DPAPI-protected to the user.
StreamSettings LoadStreamSettings();
bool SaveStreamSettings(const StreamSettings& settings);

// This user's stream.ini only.
StreamSettings LoadUserStreamSettings();
bool SaveUserStreamSettings(const StreamSettings& settings);

#if RVM_LOGIN_SERVICE
// With the sign-in service installed, the app and the service share one set of
// settings in HKLM\<kMachineSettingsKey>: readable and writable only by
// SYSTEM, Administrators and the account that installed it. The key is
// encrypted to the machine; the key's permissions are what keep it private.
constexpr wchar_t kMachineSettingsKey[] = L"SOFTWARE\\RearViewMirror\\Streaming";

// The key guards the sign-in screen, which remote control drives as SYSTEM.
constexpr size_t kMinMachineKeyChars = 20;

// Whether the store exists and this process may read and write it.
bool MachineSettingsInUse();
// False if the store is absent or unreadable. An undecryptable key comes back
// empty, its blob in lockedKey.
bool LoadMachineStreamSettings(StreamSettings& settings);
bool SaveMachineStreamSettings(const StreamSettings& settings);

// Where the app runs from, which the service starts in its owner's sessions.
// Written by the app at each start; only the installing account can.
constexpr wchar_t kAppPathValue[] = L"AppPath";
void RecordAppPath();
#endif

// What the dialog's Start/Stop button and live status line act on.
struct StreamControl {
    // Saves `settings` and starts the server. False with `error` set if it
    // could not start.
    std::function<bool(const StreamSettings& settings, std::wstring& error)> start;
    std::function<void()> stop;
    std::function<bool()> running;
    std::function<std::wstring()> status;   // One line: state, port, clients.
};

// Modal editor. Start and Stop act at once; true on OK with `settings`
// updated. An open dialog is brought to the front instead.
bool ShowStreamSettingsDialog(HWND owner, StreamSettings& settings, const StreamControl& control);

}  // namespace rvm
