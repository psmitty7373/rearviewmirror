#pragma once
#include "common.h"

#if RVM_LOGIN_SERVICE
#include <wtsapi32.h>

// The app's side of the optional sign-in service (service/), which streams the
// console's sign-in or lock screen on the app's port. The app lets go of the
// port whenever its session is not the console's, unlocked.
namespace rvm {

constexpr wchar_t kLoginServiceName[] = L"RearViewMirrorLogin";

// A locked console shows the lock screen, which only the service can capture.
inline bool SessionLocked(DWORD session) {
    WTSINFOEXW* info = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx,
                                     reinterpret_cast<LPWSTR*>(&info), &bytes)) {
        return false;
    }
    const bool locked = info && info->Level == 1 &&
                        info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    WTSFreeMemory(info);
    return locked;
}

// WM_WTSSESSION_CHANGE for this session: locks, unlocks, connects, disconnects.
void WatchSessionChanges(HWND window, bool on);

// True when this session is not what the console shows, unlocked.
bool AwayFromConsole();

// The app hands off only while this is true as well.
bool LoginServiceRunning();

}  // namespace rvm
#endif  // RVM_LOGIN_SERVICE
