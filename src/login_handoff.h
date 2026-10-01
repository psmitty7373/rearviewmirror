#pragma once
#include "common.h"

#if RVM_LOGIN_SERVICE
#include <wtsapi32.h>

// The app's side of the optional sign-in service (service/). The service
// streams whatever sign-in or lock screen the console shows, on the app's
// port, so a client sees one server whatever the console is showing. For that
// the app must let go of the port whenever its own session is not the one on
// the console, unlocked: locked, disconnected, remote, or switched away from.
namespace rvm {

constexpr wchar_t kLoginServiceName[] = L"RearViewMirrorLogin";

// Whether a session is locked. A locked console shows the lock screen, which
// only the service can capture.
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

// True when this session is not what the console shows, and the service is
// running to stream the console instead. Without the service, never: the app
// carries on as it always has.
bool LoginServiceTakesOver();

}  // namespace rvm
#endif  // RVM_LOGIN_SERVICE
