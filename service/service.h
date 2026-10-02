#pragma once
#include "login_handoff.h"

// The optional sign-in screen service (RVM_LOGIN_SERVICE); modes in main.cpp.
namespace rvm::login {

constexpr const wchar_t* kServiceName   = kLoginServiceName;
constexpr wchar_t kServiceDisplayName[] = L"Rear View Mirror sign-in screen";
constexpr wchar_t kServiceDescription[] =
    L"Streams the Windows sign-in screen to Rear View Mirror clients while nobody is signed in, "
    L"so the PC can be signed in to remotely after a restart.";

// The sign-in screen's mirror id: far above the app's ids, which count up
// from 1, so a client never mistakes one for the other.
constexpr uint32_t kLoginScreenMirrorId = 0x7F000001;

// `--helper <service pid> <stop event>`: the service's handle to an unnamed
// event, which the helper duplicates, so nobody else can make or signal it.
struct ServiceLink {
    DWORD     pid = 0;
    uintptr_t stopEvent = 0;
};

int RunInstall();
int RunUninstall();
int RunService();

// `service` null: --helper-test, with the current user's settings, until
// Ctrl+C. `portOverride`, if not 0, serves on that port instead.
int RunLoginHelper(const ServiceLink* service, uint16_t portOverride = 0);

}  // namespace rvm::login
