#include "login_handoff.h"

#if RVM_LOGIN_SERVICE
namespace rvm {

void WatchSessionChanges(HWND window, bool on) {
    if (on) {
        WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
    } else {
        WTSUnRegisterSessionNotification(window);
    }
}

bool AwayFromConsole() {
    DWORD self = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &self);
    return WTSGetActiveConsoleSessionId() != self || SessionLocked(self);
}

// Anyone signed in may ask a service whether it is running.
bool LoginServiceRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    bool running = false;
    if (SC_HANDLE service = OpenServiceW(scm, kLoginServiceName, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS status{};
        running = QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_RUNNING;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return running;
}

}  // namespace rvm
#endif  // RVM_LOGIN_SERVICE
