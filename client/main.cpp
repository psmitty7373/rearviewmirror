#include "client_window.h"
#include "crash.h"

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // A relaunch after a lost graphics device waits for the old process to go.
    const bool relaunched = rvm::WaitForPreviousInstance();

    // Only one client: two would fight over the same saved layout. A second
    // launch brings the first one forward instead.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\RearViewMirror.Client");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(rvm::kClientClass, nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    rvm::LogOpen(L"client");
    rvm::InstallCrashHandler(L"client");
    try {
        rvm::Gfx::Get().Init();
    } catch (...) {
        MessageBoxW(nullptr, L"Could not initialise Direct3D.", rvm::kAppName, MB_OK | MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    int result = 1;
    try {
        rvm::ClientWindow window;
        if (window.Create(relaunched)) {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                window.FreeRetired();   // Only here, never in a nested loop.
            }
            window.FreeRetired();
            result = static_cast<int>(msg.wParam);
        }
    } catch (...) {
        MessageBoxW(nullptr, L"An unexpected error occurred.", rvm::kAppName, MB_OK | MB_ICONERROR);
    }

    if (mutex) CloseHandle(mutex);
    return result;
}
