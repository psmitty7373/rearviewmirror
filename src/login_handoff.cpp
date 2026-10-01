#include "login_handoff.h"

#if RVM_LOGIN_SERVICE
namespace rvm {

namespace {

// Anyone signed in may ask a service whether it is running.
bool ServiceRunning() {
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

}  // namespace

void WatchSessionChanges(HWND window, bool on) {
    if (on) {
        WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
    } else {
        WTSUnRegisterSessionNotification(window);
    }
}

bool LoginServiceTakesOver() {
    DWORD self = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &self);
    if (WTSGetActiveConsoleSessionId() == self && !SessionLocked(self)) return false;
    return ServiceRunning();
}

}  // namespace rvm
#endif  // RVM_LOGIN_SERVICE
