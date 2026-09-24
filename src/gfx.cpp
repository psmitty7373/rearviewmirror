#include "gfx.h"
#include "persist.h"
#include "resource.h"

#include <cstdarg>
#include <shellscalingapi.h>

namespace rvm {

namespace {
std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
uint64_t g_logBytes = 0;
bool g_logFull = false;

// Whatever a peer on the network provokes, the log cannot fill the disk.
constexpr uint64_t kMaxLogBytes = 16ull * 1024 * 1024;
}  // namespace

void LogOpen(const wchar_t* name) {
    std::lock_guard lock(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
    const std::wstring path = ConfigDir() + L"\\" + name + L".log";
    g_logFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    g_logBytes = 0;
    g_logFull = false;
    if (g_logFile != INVALID_HANDLE_VALUE) {
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(g_logFile, bom, sizeof(bom), &written, nullptr);
    }
}

void Log(const wchar_t* fmt, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;

    wchar_t line[1024];
    const int prefix = _snwprintf_s(line, _TRUNCATE, L"%8llu  t%-6lu  ",
                                    static_cast<unsigned long long>(GetTickCount64() % 100000000ull),
                                    GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    // Room is kept for the line ending: wcscat_s on a full buffer would
    // invoke the invalid-parameter handler and end the process.
    const size_t room = ARRAYSIZE(line) - static_cast<size_t>((std::max)(prefix, 0)) - 2;
    _vsnwprintf_s(line + (std::max)(prefix, 0), room, _TRUNCATE, fmt, args);
    va_end(args);
    wcscat_s(line, L"\r\n");

    char utf8[2048];
    const int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n <= 1) return;

    std::lock_guard lock(g_logMutex);
    if (g_logFull || g_logFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (g_logBytes + static_cast<uint64_t>(n - 1) > kMaxLogBytes) {
        static const char kFull[] = "-- log size limit reached; further lines dropped --\r\n";
        WriteFile(g_logFile, kFull, sizeof(kFull) - 1, &written, nullptr);
        g_logFull = true;
    } else {
        WriteFile(g_logFile, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
        g_logBytes += written;
    }
    FlushFileBuffers(g_logFile);
}

RECT ExtendedFrameBounds(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
        RectW(r) <= 0 || RectH(r) <= 0) {
        GetWindowRect(hwnd, &r);
    }
    return r;
}

std::wstring WindowTitle(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    const int written = GetWindowTextW(hwnd, s.data(), len + 1);
    s.resize(static_cast<size_t>((std::max)(written, 0)));
    return s;
}

std::wstring WindowClassName(HWND hwnd) {
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return cls;
}

std::wstring Ellipsize(std::wstring s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    s.resize(maxChars);
    s += L"…";
    return s;
}

std::wstring Plural(int n, const wchar_t* singular, const wchar_t* plural) {
    return std::to_wstring(n) + L" " + (n == 1 ? singular : plural);
}

RECT WorkAreaFor(HMONITOR monitor) {
    MONITORINFO mi{ sizeof(mi) };
    if (monitor && GetMonitorInfoW(monitor, &mi)) return mi.rcWork;
    return RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

RECT WorkAreaFor(HWND hwnd) {
    return WorkAreaFor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY));
}

HICON LoadAppIcon(int size) {
    // One icon per size for the life of the process: windows and the tray
    // hold on to these handles, so they are never destroyed.
    static std::mutex lock;
    static std::vector<std::pair<int, HICON>> cache;
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& [s, icon] : cache) {
        if (s == size) return icon;
    }
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                               MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               size, size, LR_DEFAULTCOLOR));
    if (icon) cache.emplace_back(size, icon);
    return icon;
}

float DpiScaleFor(HMONITOR monitor) {
    UINT dpiX = 96, dpiY = 96;
    if (!monitor || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return static_cast<float>(GetDpiForSystem()) / 96.0f;
    }
    return static_cast<float>(dpiX) / 96.0f;
}

std::wstring GetSecretText(HWND dlg, int id) {
    wchar_t buffer[512]{};
    GetDlgItemTextW(dlg, id, buffer, ARRAYSIZE(buffer));
    std::wstring text = buffer;
    SecureZeroMemory(buffer, sizeof(buffer));
    return text;
}

void RevealEditText(HWND edit, bool reveal) {
    // U+25CF is the mask the version 6 edit control uses by default.
    SendMessageW(edit, EM_SETPASSWORDCHAR, reveal ? 0 : 0x25CF, 0);
    InvalidateRect(edit, nullptr, TRUE);
}

void ApplyTitleBarTheme(HWND hwnd) {
    if (!hwnd) return;
    // Windows' "app mode" setting: 0 is dark. Missing means light.
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    const BOOL dark = light == 0 ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE; Windows 10 builds before 20H1 used 19.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
}

bool PumpNestedMessage() {
    MSG msg;
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got == 0) {
        PostQuitMessage(static_cast<int>(msg.wParam));
        return false;
    }
    if (got < 0) return false;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    return true;
}

bool IsCapturableWindow(HWND hwnd) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

    // Never offer our own overlays or mirrors as a capture source.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;

    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;

    // UWP hosts keep invisible placeholder windows around; DWM flags them cloaked.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    if (WindowTitle(hwnd).empty()) return false;

    const std::wstring cls = WindowClassName(hwnd);
    for (const wchar_t* s : { L"Progman", L"WorkerW", L"Shell_TrayWnd",
                              L"Windows.UI.Core.CoreWindow" }) {
        if (cls == s) return false;
    }

    const RECT b = ExtendedFrameBounds(hwnd);
    return RectW(b) > 8 && RectH(b) > 8;
}

Gfx& Gfx::Get() {
    static Gfx g;
    return g;
}

void Gfx::Init() {
    if (d3d) return;

    // VIDEO_SUPPORT enables the video processor and DXVA that streaming uses.
    // Some drivers (and remote sessions) refuse it; mirrors work without it,
    // so a device without it beats none.
    constexpr UINT kBase = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    constexpr UINT kVideo = kBase | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    auto create = [&](D3D_DRIVER_TYPE type, UINT flags) {
        d3d = nullptr;
        ctx = nullptr;
        return D3D11CreateDevice(nullptr, type, nullptr, flags, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, d3d.put(), nullptr, ctx.put());
    };
    HRESULT hr = S_OK;
    for (const D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
        hr = create(type, kVideo);
        if (FAILED(hr)) {
            hr = create(type, kBase);
            if (SUCCEEDED(hr)) Log(L"gfx: device has no video support; streaming will not work");
        }
        if (SUCCEEDED(hr)) break;
    }
    HR(hr);

    if (auto mt = ctx.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);

    dxgi = d3d.as<IDXGIDevice>();
    if (auto dxgi1 = dxgi.try_as<IDXGIDevice1>()) dxgi1->SetMaximumFrameLatency(1);

    winrt::com_ptr<IDXGIAdapter> adapter;
    HR(dxgi->GetAdapter(adapter.put()));
    HR(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

    D2D1_FACTORY_OPTIONS opts{};
    HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                         &opts, d2dFactory.put_void()));
    HR(d2dFactory->CreateDevice(dxgi.get(), d2dDevice.put()));

    HR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(dwrite.put())));

    winrt::com_ptr<::IInspectable> inspectable;
    HR(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    winrtDevice = inspectable.as<
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

bool CompSurface::Create(HWND hwnd, UINT width, UINT height) {
    auto& g = Gfx::Get();
    width_  = ClampI(static_cast<int>(width), 1, kMaxExtent);
    height_ = ClampI(static_cast<int>(height), 1, kMaxExtent);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = width_;
    desc.Height      = height_;
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc  = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;

    if (FAILED(g.factory->CreateSwapChainForComposition(g.d3d.get(), &desc, nullptr, swap_.put())) ||
        FAILED(DCompositionCreateDevice(g.dxgi.get(), __uuidof(IDCompositionDevice), dcomp_.put_void())) ||
        FAILED(dcomp_->CreateTargetForHwnd(hwnd, TRUE, target_.put())) ||
        FAILED(dcomp_->CreateVisual(visual_.put())) ||
        FAILED(visual_->SetContent(swap_.get())) ||
        FAILED(target_->SetRoot(visual_.get())) ||
        FAILED(dcomp_->Commit())) {
        swap_ = nullptr;
        return false;
    }
    return true;
}

bool CompSurface::Resize(UINT width, UINT height) {
    width  = ClampI(static_cast<int>(width), 1, kMaxExtent);
    height = ClampI(static_cast<int>(height), 1, kMaxExtent);
    if (!swap_) return false;
    if (width == width_ && height == height_) return true;
    if (FAILED(swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) return false;
    width_  = width;
    height_ = height;
    return true;
}

void CompSurface::Present(UINT syncInterval) {
    if (swap_) Gfx::Get().CheckDevice(swap_->Present(syncInterval, 0));
}

bool RelaunchSelf() {
    wchar_t exe[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (n == 0 || n >= ARRAYSIZE(exe)) return false;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + kRestartArg + L" " +
                       std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool WaitForPreviousInstance() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    DWORD pid = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (lstrcmpiW(argv[i], kRestartArg) == 0) pid = wcstoul(argv[i + 1], nullptr, 10);
    }
    LocalFree(argv);
    if (pid == 0) return false;
    if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(previous, 15000);
        CloseHandle(previous);
    }
    return true;
}

void Gfx::SetDeviceLostHandler(std::function<void()> handler) {
    std::lock_guard lock(lostMutex_);
    onLost_ = std::move(handler);
}

void Gfx::CheckDevice(HRESULT hr) {
    if (SUCCEEDED(hr) || lost_.load()) return;
    const bool removed = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                         hr == DXGI_ERROR_DEVICE_HUNG || (d3d && FAILED(d3d->GetDeviceRemovedReason()));
    if (!removed || lost_.exchange(true)) return;

    Log(L"gfx: graphics device lost (0x%08X, reason 0x%08X)", static_cast<unsigned>(hr),
        static_cast<unsigned>(d3d ? d3d->GetDeviceRemovedReason() : S_OK));
    std::function<void()> handler;
    {
        std::lock_guard lock(lostMutex_);
        handler = onLost_;
    }
    if (handler) handler();
}

}  // namespace rvm
