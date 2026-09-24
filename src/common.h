#pragma once

// Winsock must precede windows.h, and ws2tcpip.h must precede anything that
// declares a global named `Error` (its inline helpers shadow one otherwise).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <d3dcompiler.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define HR(expr) winrt::check_hresult(expr)

namespace rvm {

constexpr UINT WM_RVM_MIRROR_CLOSED = WM_APP + 1;   // wParam: mirror id
constexpr UINT WM_RVM_TARGET_LOST   = WM_APP + 2;
constexpr UINT WM_RVM_TRAY          = WM_APP + 3;
constexpr UINT WM_RVM_NEW_MIRROR    = WM_APP + 4;
constexpr UINT WM_RVM_STATE_CHANGED = WM_APP + 5;
constexpr UINT WM_RVM_HOVER         = WM_APP + 7;
constexpr UINT WM_RVM_MIRROR_ORPHANED = WM_APP + 8;   // wParam: mirror id
constexpr UINT WM_RVM_FOREGROUND_CHANGED = WM_APP + 9;
constexpr UINT WM_RVM_STREAM_WANT_FRAME = WM_APP + 10;   // wParam: mirror id

constexpr wchar_t kAppName[]      = L"Rear View Mirror";
constexpr wchar_t kAppWindowClass[] = L"RvmAppWindow";

// D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION: the largest swapchain or texture
// extent the device will create. Everything that sizes a window is capped here.
constexpr int kMaxExtent = 16384;

// How the crop follows the source window when that window gets resized.
enum class TrackMode {
    Anchored,      // Fixed pixel offset from the source's top-left.
    Proportional,  // Scales with the source, staying over the same content.
};

constexpr float kAccentR = 0.361f;
constexpr float kAccentG = 0.612f;
constexpr float kAccentB = 1.000f;

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int   ClampI(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }

inline int RectW(const RECT& r) { return r.right - r.left; }
inline int RectH(const RECT& r) { return r.bottom - r.top; }

inline RECT NormalizeRect(POINT a, POINT b) {
    return RECT{ (std::min)(a.x, b.x), (std::min)(a.y, b.y),
                 (std::max)(a.x, b.x), (std::max)(a.y, b.y) };
}

// The DWM-rendered bounds: what Windows Graphics Capture actually hands back,
// excluding the invisible resize border. Basis for screen-to-texture mapping.
RECT ExtendedFrameBounds(HWND hwnd);

// True for top-level windows a user would plausibly want to mirror.
bool IsCapturableWindow(HWND hwnd);

std::wstring WindowTitle(HWND hwnd);
std::wstring WindowClassName(HWND hwnd);
std::wstring Ellipsize(std::wstring s, size_t maxChars);
std::wstring Plural(int n, const wchar_t* singular, const wchar_t* plural);

RECT WorkAreaFor(HMONITOR monitor);
RECT WorkAreaFor(HWND hwnd);

HICON LoadAppIcon(int size);

// Diagnostic log at %APPDATA%\RearViewMirror\<name>.log, truncated on open.
// Lines carry a millisecond tick and thread id. No-op until LogOpen is called.
void LogOpen(const wchar_t* name);
void Log(const wchar_t* fmt, ...);

// Per-call-site sampling for per-frame events: the first few, then one in N.
#define RVM_LOG_SAMPLED(n, ...)                                          \
    do {                                                                 \
        static std::atomic<uint32_t> rvmLogCount_{ 0 };                  \
        const uint32_t rvmLogK_ = rvmLogCount_++;                        \
        if (rvmLogK_ < 5 || rvmLogK_ % (n) == 0) ::rvm::Log(__VA_ARGS__); \
    } while (0)

// Pumps one message for a nested loop. Returns false when the loop must end:
// GetMessage returns 0 on WM_QUIT, which is re-posted so the outer loop still
// sees it rather than the app becoming a windowless zombie.
bool PumpNestedMessage();

// Base for anything with a window procedure. The thunk stores the object in
// GWLP_USERDATA at WM_NCCREATE and dispatches to it, and it stops exceptions
// here: a C++ exception cannot unwind through the kernel callback that invokes
// a window procedure, and would terminate the process.
class WindowHost {
public:
    HWND Hwnd() const { return hwnd_; }

protected:
    HWND hwnd_ = nullptr;

    template <class T, LRESULT (T::*Handler)(UINT, WPARAM, LPARAM)>
    friend LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
};

template <class T, LRESULT (T::*Handler)(UINT, WPARAM, LPARAM)>
LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    T* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<T*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        static_cast<WindowHost*>(self)->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return (self->*Handler)(msg, wp, lp);
    } catch (...) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace rvm
