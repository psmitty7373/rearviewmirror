#pragma once
#include "login_handoff.h"

// The optional login-screen service (RVM_LOGIN_SERVICE). One executable,
// RearViewMirrorService.exe, in these modes:
//
//   --install     Elevated. Copies itself to Program Files, stores the current
//                 user's streaming port and key for the machine, and
//                 registers and starts the service. Run again to update.
//   --uninstall   Elevated. Stops and removes all of that.
//   --service     Started by Windows. Watches the console session and keeps
//                 the helper running exactly while nobody is signed in there.
//   --helper-test Streams this session's screen the same way, by hand, with
//                 the current user's settings; for trying it out.
//   --helper      Started by the service, as SYSTEM in the console session on
//                 the sign-in desktop. Captures it with Desktop Duplication
//                 and streams it on the same port and key as the app, so a
//                 client sees one server throughout: the sign-in or lock
//                 screen while the console shows one, the app's own mirrors
//                 otherwise.
//
// The app's only part in it is letting go of the port meanwhile
// (src/login_handoff.*), and only in builds with the service.
namespace rvm::login {

constexpr const wchar_t* kServiceName   = kLoginServiceName;
constexpr wchar_t kServiceDisplayName[] = L"Rear View Mirror sign-in screen";
constexpr wchar_t kServiceDescription[] =
    L"Streams the Windows sign-in screen to Rear View Mirror clients while nobody is signed in, "
    L"so the PC can be signed in to remotely after a restart.";

// The sign-in screen's mirror id: far above the app's ids, which count up
// from 1, so a client never mistakes one for the other.
constexpr uint32_t kLoginScreenMirrorId = 0x7F000001;

// How the service's helper is started: `--helper <service pid> <stop event>`,
// the event being the service's handle to an unnamed event, which the helper
// duplicates. Nothing named that anyone could make first or signal.
struct ServiceLink {
    DWORD     pid = 0;
    uintptr_t stopEvent = 0;
};

int RunInstall();
int RunUninstall();
int RunService();

// `service`: started by the service, which stops it through its event, with
// the machine's sign-in settings. Null: started by hand (--helper-test), for
// testing in the current session with the current user's streaming settings,
// and stops on Ctrl+C. `portOverride`, if not 0, serves on that port instead,
// beside a running app.
int RunLoginHelper(const ServiceLink* service, uint16_t portOverride = 0);

}  // namespace rvm::login
