#include "crash.h"
#include "login_config.h"
#include "persist.h"
#include "service.h"

#include <cstdio>
#include <wtsapi32.h>

namespace rvm::login {

namespace {

constexpr DWORD kNoSession = 0xFFFFFFFF;
constexpr DWORD kHelperStopWaitMs = 5000;
// Session notifications are the cue to act; this catches anything missed.
constexpr DWORD kRecheckMs = 5000;
// A helper that keeps exiting soon after starting is restarted ever less often.
constexpr ULONGLONG kQuickExitMs = 30000;
constexpr DWORD kMinBackoffMs = 1000, kMaxBackoffMs = 60000;

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stop = nullptr;   // The service is to stop.
HANDLE g_wake = nullptr;   // A session changed: look again.

bool g_trustedDir = false;

void Report(DWORD state, DWORD waitHintMs = 0, DWORD exitCode = NO_ERROR) {
    static DWORD checkpoint = 1;
    SERVICE_STATUS s{};
    s.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s.dwCurrentState = state;
    s.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    s.dwWin32ExitCode = exitCode;
    s.dwWaitHint = waitHintMs;
    s.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_statusHandle, &s);
}

const wchar_t* SessionEventName(DWORD type) {
    switch (type) {
    case WTS_CONSOLE_CONNECT:    return L"console connect";
    case WTS_CONSOLE_DISCONNECT: return L"console disconnect";
    case WTS_REMOTE_CONNECT:     return L"remote connect";
    case WTS_REMOTE_DISCONNECT:  return L"remote disconnect";
    case WTS_SESSION_LOGON:      return L"sign-in";
    case WTS_SESSION_LOGOFF:     return L"sign-out";
    case WTS_SESSION_LOCK:       return L"lock";
    case WTS_SESSION_UNLOCK:     return L"unlock";
    default:                     return L"other change";
    }
}

DWORD WINAPI Control(DWORD control, DWORD type, LPVOID data, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        Report(SERVICE_STOP_PENDING, kHelperStopWaitMs * 2);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE: {
        const auto* note = static_cast<const WTSSESSION_NOTIFICATION*>(data);
        Log(L"service: session %lu: %s", note ? note->dwSessionId : kNoSession, SessionEventName(type));
        SetEvent(g_wake);
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Signed in, or signing in: from that moment the session belongs to the app,
// unless it is locked.
bool SignedIn(DWORD session) {
    wchar_t* user = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &bytes)) {
        return false;
    }
    const bool signedIn = user && user[0] != L'\0';
    WTSFreeMemory(user);
    return signedIn;
}

// The helper is this executable again, as SYSTEM like the service, but in the
// console session and on its sign-in desktop, where the screen is. Handles
// cannot be inherited across sessions, so it duplicates `stopEvent` from here.
HANDLE LaunchHelper(DWORD session, HANDLE stopEvent) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));

    HANDLE self = nullptr, token = nullptr;
    HANDLE process = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &self) &&
        DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &token) &&
        SetTokenInformation(token, TokenSessionId, &session, sizeof(session))) {
        STARTUPINFOW si{ sizeof(si) };
        si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\winlogon");
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --helper " + std::to_wstring(GetCurrentProcessId()) +
                               L" " + std::to_wstring(reinterpret_cast<uintptr_t>(stopEvent));
        PROCESS_INFORMATION pi{};
        if (CreateProcessAsUserW(token, exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            process = pi.hProcess;
        }
    }
    const DWORD error = GetLastError();
    if (token) CloseHandle(token);
    if (self) CloseHandle(self);
    SetLastError(error);
    return process;
}

void StopHelper(HANDLE process, HANDLE stopEvent, const wchar_t* why) {
    Log(L"service: stopping the helper: %s", why);
    SetEvent(stopEvent);
    if (WaitForSingleObject(process, kHelperStopWaitMs) != WAIT_OBJECT_0) {
        Log(L"service: the helper did not stop; ending it");
        TerminateProcess(process, 1);
        WaitForSingleObject(process, kHelperStopWaitMs);
    }
    CloseHandle(process);
}

// What the console session shows: the app's desktop only when someone is
// signed in and it is not locked. Otherwise a sign-in or lock screen, which
// the app cannot capture and lets go of the port for.
enum class Console { None, SignIn, Locked, Desktop };

Console ConsoleShows(DWORD session) {
    if (session == kNoSession) return Console::None;   // Between sessions for a moment.
    if (!SignedIn(session)) return Console::SignIn;
    return SessionLocked(session) ? Console::Locked : Console::Desktop;
}

// Keeps exactly one helper running, in the console session, while the console
// shows a sign-in or lock screen, and none otherwise. Returns why it stopped.
DWORD Supervise() {
    HANDLE helperStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!helperStop) {
        const DWORD error = GetLastError();
        Log(L"service: cannot create the helper's stop event (%lu)", error);
        return error;
    }
    DWORD result = NO_ERROR;
    HANDLE helper = nullptr;
    DWORD helperSession = kNoSession;
    ULONGLONG helperStarted = 0, nextLaunch = 0;
    DWORD backoff = kMinBackoffMs;
    Console last = Console::None;

    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (helper && WaitForSingleObject(helper, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(helper, &code);
            CloseHandle(helper);
            helper = nullptr;
            backoff = now - helperStarted < kQuickExitMs ? (std::min)(backoff * 2, kMaxBackoffMs) : kMinBackoffMs;
            nextLaunch = now + backoff;
            Log(L"service: the helper exited (code %lu); starting it again in %lu s", code, backoff / 1000);
        }

        const DWORD console = WTSGetActiveConsoleSessionId();
        const Console shows = ConsoleShows(console);
        const bool wanted = shows == Console::SignIn || shows == Console::Locked;
        if (shows != last) {
            Log(L"service: console session %lu %s", console,
                shows == Console::SignIn   ? L"has nobody signed in: the helper streams it"
                : shows == Console::Locked ? L"is locked: the helper streams it"
                : shows == Console::Desktop ? L"is signed in: the app streams it"
                                            : L"is between sessions");
            last = shows;
        }
        if (helper && (!wanted || helperSession != console)) {
            StopHelper(helper, helperStop,
                       wanted ? L"the console moved to another session" : L"the console shows a desktop again");
            helper = nullptr;
            backoff = kMinBackoffMs;
            nextLaunch = 0;
        }
        if (wanted && !helper && now >= nextLaunch) {
            ResetEvent(helperStop);
            helper = LaunchHelper(console, helperStop);
            if (helper) {
                helperSession = console;
                helperStarted = now;
                Log(L"service: helper started in session %lu", console);
            } else {
                Log(L"service: the helper could not start (%lu)", GetLastError());
                nextLaunch = now + backoff;
                backoff = (std::min)(backoff * 2, kMaxBackoffMs);
            }
        }

        DWORD timeout = kRecheckMs;
        if (wanted && !helper) {
            const ULONGLONG untilLaunch = nextLaunch > now ? nextLaunch - now : 0;
            timeout = static_cast<DWORD>((std::min)(static_cast<ULONGLONG>(timeout), untilLaunch));
        }
        const HANDLE waits[] = { g_stop, g_wake, helper };
        const DWORD woke = WaitForMultipleObjects(helper ? 3 : 2, waits, FALSE, timeout);
        if (woke == WAIT_OBJECT_0) break;
        if (woke == WAIT_FAILED) {
            result = GetLastError();
            Log(L"service: waiting failed (%lu); stopping", result);
            break;
        }
    }
    if (helper) StopHelper(helper, helperStop, L"the service is stopping");
    CloseHandle(helperStop);
    return result;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, &Control, nullptr);
    if (!g_statusHandle) return;
    if (!g_trustedDir) {
        // Nowhere safe to log: Windows records the error.
        Report(SERVICE_STOPPED, 0, ERROR_ACCESS_DENIED);
        return;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stop || !g_wake) {
        const DWORD error = GetLastError();
        Log(L"service: cannot create its events (%lu)", error);
        Report(SERVICE_STOPPED, 0, error);
        return;
    }
    Report(SERVICE_RUNNING);
    Log(L"service: running");
    const DWORD result = Supervise();
    Log(L"service: stopped");
    Report(SERVICE_STOPPED, 0, result);
}

}  // namespace

int RunService() {
    // A folder anyone else could have made or changed is not trusted with
    // the key, nor written to as SYSTEM.
    g_trustedDir = MachineDirTrusted();
    if (g_trustedDir) {
        SetConfigDir(MachineDir());
        LogOpen(L"service");
        InstallCrashHandler(L"service");
    }
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<wchar_t*>(kServiceName), &ServiceMain }, { nullptr, nullptr } };
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            fwprintf(stderr, L"--service is for Windows to start. Use --install.\n");
        }
        return 1;
    }
    return 0;
}

}  // namespace rvm::login
