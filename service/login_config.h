#pragma once
#include "stream_settings.h"

namespace rvm::login {

// The stream key is all that guards the sign-in screen, which remote control
// drives as SYSTEM. The app's generated keys are this long.
constexpr size_t kMinLoginKeyChars = 20;

// %ProgramData%\RearViewMirror: the service's settings (with the key), logs
// and crash dumps. SYSTEM and Administrators only.
std::wstring MachineDir();

// Elevated. Creates MachineDir() and (re)applies its access rules. Any user
// may create folders in ProgramData, so an untrusted one is never taken over:
// a link is removed, a folder moved aside (to `movedAside`).
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
