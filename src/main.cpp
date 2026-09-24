#include "app.h"

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Physical pixels everywhere, so textures, window rects and overlays agree.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Only one instance: a second launch just asks the first for a new mirror.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\RearViewMirror.Instance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(rvm::kAppWindowClass, nullptr)) {
            PostMessageW(existing, rvm::WM_RVM_NEW_MIRROR, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    int result = 1;
    try {
        rvm::App app;
        result = app.Run();
    } catch (const winrt::hresult_error& e) {
        MessageBoxW(nullptr, e.message().c_str(), rvm::kAppName, MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"An unexpected error occurred.", rvm::kAppName, MB_OK | MB_ICONERROR);
    }

    if (mutex) CloseHandle(mutex);
    return result;
}
