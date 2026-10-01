#pragma once
#include "stream_settings.h"

namespace rvm::login {

// %ProgramData%\RearViewMirror: the service's settings, logs and crash dumps.
// Readable and writable only by SYSTEM and Administrators, since it holds the
// streaming key. Nobody is signed in when the helper runs, so neither a
// user's AppData nor a key encrypted to a user could be used.
std::wstring MachineDir();

// Creates MachineDir() if need be and (re)applies its access rules: owned by
// Administrators, SYSTEM and Administrators only, nothing inherited. A folder
// someone else made first is taken over rather than trusted. Elevated only.
bool SecureMachineDir();

// The helper's streaming settings. The key is encrypted to this machine (DPAPI
// local-machine scope); the folder's access rules are what keep it private.
bool SaveLoginSettings(const StreamSettings& settings);
bool LoadLoginSettings(StreamSettings& settings);

}  // namespace rvm::login
