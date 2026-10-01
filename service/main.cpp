#include "service.h"

#include <cstdio>
#include <cwchar>

namespace {

void Usage() {
    wprintf(L"RearViewMirrorService: streams the Windows sign-in screen while nobody is signed in.\n\n"
            L"  --install            Install or update (elevated). Uses this account's streaming\n"
            L"                       port and key, so the client sees the same server.\n"
            L"  --uninstall          Remove the service, its files, settings and logs (elevated).\n"
            L"  --helper-test [port] Stream this session's screen the same way, until Ctrl+C,\n"
            L"                       to try it without installing. A port avoids a running app.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"--service")   return rvm::login::RunService();
    if (mode == L"--install")   return rvm::login::RunInstall();
    if (mode == L"--uninstall") return rvm::login::RunUninstall();
    if (mode == L"--helper") {
        if (argc < 4) return 1;
        const rvm::login::ServiceLink link{ wcstoul(argv[2], nullptr, 10),
                                            static_cast<uintptr_t>(_wcstoui64(argv[3], nullptr, 10)) };
        return rvm::login::RunLoginHelper(&link);
    }
    if (mode == L"--helper-test") {
        const unsigned long port = argc > 2 ? wcstoul(argv[2], nullptr, 10) : 0;
        if (port > 65535) {
            Usage();
            return 1;
        }
        return rvm::login::RunLoginHelper(nullptr, static_cast<uint16_t>(port));
    }
    Usage();
    return mode.empty() || mode == L"--help" ? 0 : 1;
}
