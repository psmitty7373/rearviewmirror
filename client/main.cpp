#include "client_window.h"

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    rvm::LogOpen(L"client");
    try {
        rvm::Gfx::Get().Init();
    } catch (...) {
        MessageBoxW(nullptr, L"Could not initialise Direct3D.", rvm::kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    rvm::ClientWindow window;
    if (!window.Create()) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
