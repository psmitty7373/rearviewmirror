#pragma once
#include "stream_settings.h"

namespace rvm::login {

// %ProgramData%\RearViewMirror: the service's logs and crash dumps. SYSTEM and
// Administrators only.
std::wstring MachineDir();

// Elevated. Creates MachineDir() and (re)applies its access rules. Any user
// may create folders in ProgramData, so an untrusted one is never taken over:
// a link is removed, a folder moved aside (to `movedAside`).
bool SecureMachineDir(std::wstring& movedAside);

// Whether MachineDir() is a real folder, owned by SYSTEM or Administrators,
// that grants nobody else anything. The service and helper use it for nothing
// otherwise, not even a log.
bool MachineDirTrusted();

// Elevated. Creates the shared settings key (kMachineSettingsKey) owned by
// Administrators, readable and writable by SYSTEM, Administrators and the
// account running this, and nobody else.
bool CreateMachineSettings();
void DeleteMachineSettings();

// The shared settings, for the helper; false without a key of at least
// kMinMachineKeyChars.
bool LoadLoginSettings(StreamSettings& settings);

// The account the settings key grants access to: the one that installed it,
// read from the key's rules, which that account cannot change.
bool AppOwnerSid(std::vector<uint8_t>& sid);
// What RecordAppPath last wrote; empty if never.
std::wstring RecordedAppPath();

}  // namespace rvm::login
