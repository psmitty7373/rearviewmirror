#pragma once
#include "common.h"

namespace rvm {

// On a crash, logs the exception and the crashing thread's stack (with names
// when the .pdb is beside the .exe), writes ConfigDir()\<name>-crash-<time>.dmp,
// then lets Windows report it as before. Call once, after LogOpen.
void InstallCrashHandler(const wchar_t* name);

}  // namespace rvm
