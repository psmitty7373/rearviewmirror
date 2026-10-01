#include "crash.h"
#include "duplication.h"
#include "login_config.h"
#include "persist.h"
#include "service.h"
#include "stream_server.h"

#include <cstdio>

namespace rvm::login {

namespace {

HANDLE g_consoleStop = nullptr;

BOOL WINAPI OnConsoleCtrl(DWORD) {
    if (g_consoleStop) SetEvent(g_consoleStop);
    return TRUE;
}

}  // namespace

// Started by the service as SYSTEM, in the console session, on the sign-in
// desktop. Every thread starts on that desktop, and it stays the input
// desktop for as long as this runs (the service stops it the moment someone
// signs in), so remote input from the stream server's own thread lands on the
// sign-in screen as it is.
//
// Started by hand (--helper-test), it does the same for the current session
// with the current user's streaming settings, to try the capture and stream
// without installing anything.
int RunLoginHelper(const ServiceLink* service, uint16_t portOverride) {
    const bool byService = service != nullptr;
    if (byService) {
        if (!MachineDirTrusted()) return 5;   // Not even to log: see RunService.
        SetConfigDir(MachineDir());
    }
    LogOpen(L"login");
    InstallCrashHandler(L"login");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    wchar_t user[256]{};
    DWORD userLen = ARRAYSIZE(user);
    GetUserNameW(user, &userLen);
    Log(L"login: helper started in session %lu as %s%s", session, user, byService ? L"" : L" (by hand)");

    StreamSettings settings;
    if (byService) {
        if (!LoadLoginSettings(settings)) {
            Log(L"login: no usable sign-in settings; run RearViewMirrorService.exe --install");
            return 2;
        }
    } else {
        settings = LoadStreamSettings();
        settings.enabled = true;
        if (settings.key.empty()) {
            fwprintf(stderr, L"No streaming key: set one up in Rear View Mirror's Streaming dialog first.\n");
            return 2;
        }
    }
    if (portOverride) settings.port = portOverride;

    // Started by the service: its stop event, and the service itself, so that
    // a helper whose service died does not hold the port on its own.
    HANDLE stop = nullptr, serviceProcess = nullptr;
    if (byService) {
        serviceProcess = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, service->pid);
        if (serviceProcess && !DuplicateHandle(serviceProcess, reinterpret_cast<HANDLE>(service->stopEvent),
                                               GetCurrentProcess(), &stop, SYNCHRONIZE, FALSE, 0)) {
            stop = nullptr;
        }
    } else {
        stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (!stop) {
        Log(L"login: no stop event (%lu)", GetLastError());
        if (serviceProcess) CloseHandle(serviceProcess);
        return 2;
    }
    if (!byService) {
        g_consoleStop = stop;
        SetConsoleCtrlHandler(&OnConsoleCtrl, TRUE);
    }

    try {
        Gfx::Get().Init();
    } catch (...) {
        Log(L"login: no graphics device");
        return 3;
    }
    // A lost device takes every texture and encoder with it; the service
    // starts a fresh helper.
    HANDLE lost = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Gfx::Get().SetDeviceLostHandler([lost] {
        Log(L"login: graphics device lost; exiting for a fresh start");
        SetEvent(lost);
    });

    StreamServer server;
    DuplicationCapture capture;
    server.SetFrameRequester([&capture](uint32_t id) {
        if (id == kLoginScreenMirrorId) capture.Repush();
    });
    capture.Start(
        [&server](ID3D11Texture2D* frame) {
            D3D11_TEXTURE2D_DESC d{};
            frame->GetDesc(&d);
            server.SubmitFrame(kLoginScreenMirrorId, frame,
                               RECT{ 0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height) });
        },
        [&server](SIZE size) {
            // The whole virtual screen, so it can be controlled where control is built.
            server.SetMirrorList({ { kLoginScreenMirrorId, L"Sign-in screen", static_cast<UINT>(size.cx),
                                     static_cast<UINT>(size.cy), RVM_REMOTE_CONTROL != 0 } });
        },
        [&server] { return server.Watched(kLoginScreenMirrorId); }, settings.fps);
    if (!byService) {
        wprintf(L"Streaming this session's screen on UDP port %u. Ctrl+C stops.\n", settings.port);
    }

    // The app may still hold the port for a moment after a sign-out, or for
    // good if someone left it running in a disconnected session: keep trying.
    const HANDLE waits[] = { stop, lost, serviceProcess };
    const DWORD waitCount = serviceProcess ? 3 : 2;
    bool warned = false;
    for (;;) {
        if (!server.Running()) {
            if (server.Start(settings)) {
                Log(L"login: serving the sign-in screen on udp %u", server.Port());
                warned = false;
            } else if (!warned) {
                Log(L"login: udp %u is not free; trying again every few seconds", settings.port);
                warned = true;
            }
        }
        const DWORD woke = WaitForMultipleObjects(waitCount, waits, FALSE, server.Running() ? INFINITE : 3000);
        if (woke != WAIT_TIMEOUT) break;
    }
    const bool deviceLost = WaitForSingleObject(lost, 0) == WAIT_OBJECT_0;

    capture.Stop();
    server.Stop();   // Says goodbye: clients reconnect to whatever serves the port next.
    Log(L"login: helper stopped");
    CloseHandle(lost);
    CloseHandle(stop);
    if (serviceProcess) CloseHandle(serviceProcess);
    if (SUCCEEDED(com)) CoUninitialize();
    return deviceLost ? 4 : 0;
}

}  // namespace rvm::login
