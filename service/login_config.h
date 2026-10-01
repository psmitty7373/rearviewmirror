#pragma once
#include "stream_settings.h"

namespace rvm::login {

// The stream key is all that guards the sign-in screen, which remote control
// drives as SYSTEM. The app's generated keys are this long.
constexpr size_t kMinLoginKeyChars = 20;

// %ProgramData%\RearViewMirror: the service's settings, logs and crash dumps.
// Readable and writable only by SYSTEM and Administrators, since it holds the
// streaming key. Nobody is signed in when the helper runs, so neither a
// user's AppData nor a key encrypted to a user could be used.
std::wstring MachineDir();

// Creates MachineDir() if need be and (re)applies its access rules: owned by
// Administrators, SYSTEM and Administrators only, nothing inherited. Any user
// may create folders in ProgramData, so one found there that is not already
// like that, or is a link, is never taken over: a link is removed, a folder
// moved aside (`movedAside` names where), and a new one made. Elevated only.
bool SecureMachineDir(std::wstring& movedAside);

// Whether MachineDir() is a real folder, owned by SYSTEM or Administrators,
// that grants nobody else anything. The service and helper use it for nothing
// otherwise, not even a log.
bool MachineDirTrusted();

// The helper's streaming settings. The key is encrypted to this machine (DPAPI
// local-machine scope); the folder's access rules are what keep it private.
// A key shorter than kMinLoginKeyChars is refused.
bool SaveLoginSettings(const StreamSettings& settings);
bool LoadLoginSettings(StreamSettings& settings);

}  // namespace rvm::login
