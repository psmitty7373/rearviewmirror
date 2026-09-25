#pragma once
#include "common.h"

namespace rvm {

// On a crash, writes what is needed to find its cause: the exception and the
// crashing thread's stack go to the log (module and offset for every frame,
// and function names when the .pdb sits next to the .exe), and a small dump
// goes to %APPDATA%\RearViewMirror\<name>-crash-<time>.dmp. Windows then
// reports the crash as before. Call once, after LogOpen.
void InstallCrashHandler(const wchar_t* name);

}  // namespace rvm
