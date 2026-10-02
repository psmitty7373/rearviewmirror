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
#include <random>
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
constexpr UINT WM_RVM_DEVICE_LOST = WM_APP + 11;
constexpr UINT WM_RVM_SHOW_MANAGER = WM_APP + 12;   // A second launch asks the first for its window.
constexpr UINT WM_RVM_MIRROR_IDLE    = WM_APP + 30;   // wParam: mirror id. Hidden, and a frame nobody wanted.
constexpr UINT WM_RVM_MIRROR_RESTART = WM_APP + 31;   // wParam: mirror id. Needs RestartCapture().
constexpr UINT WM_RVM_CROP_RESHAPED  = WM_APP + 32;   // To a mirror window: the drawn crop changed size.

// Passed, with the old process id, to a relaunch after a lost graphics device.
constexpr wchar_t kRestartArg[] = L"--after-device-loss";

// Starts a fresh copy of this executable that waits for this one to exit.
bool RelaunchSelf();

// True in a RelaunchSelf child, after waiting (bounded) for the old process.
bool WaitForPreviousInstance();

constexpr wchar_t kAppName[]      = L"Rear View Mirror";
constexpr wchar_t kAppWindowClass[] = L"RvmAppWindow";

// D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION; every window size is capped here.
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

// The DWM bounds, without the invisible resize border: what capture shows.
RECT ExtendedFrameBounds(HWND hwnd);

// True for top-level windows a user would plausibly want to mirror.
bool IsCapturableWindow(HWND hwnd);

std::wstring WindowTitle(HWND hwnd);
std::wstring WindowClassName(HWND hwnd);
std::wstring Ellipsize(std::wstring s, size_t maxChars);
std::wstring Plural(int n, const wchar_t* singular, const wchar_t* plural);

RECT WorkAreaFor(HMONITOR monitor);
RECT WorkAreaFor(HWND hwnd);

// Frameless resizable windows (mirrors, pop-outs). The scaled edges resize;
// the rest is client area, where a press starts a caption drag.
LRESULT ResizeBorderHitTest(HWND hwnd, LPARAM lp);
void BeginWindowDrag(HWND hwnd, LPARAM lp);
// Their picture is always shown whole and unstretched, so the window keeps
// native's shape. `native` must not be empty.
// native scaled, then kept within minimum and kMaxExtent without bending.
SIZE ScaleNative(SIZE native, double scale, SIZE minimum);
// Fits a WM_SIZING rect, sticking at native size unless Ctrl is down. True
// when it lands on native size.
bool ConstrainToNative(WPARAM edge, RECT* rect, SIZE native, SIZE minimum);
// For WM_WINDOWPOSCHANGING: any other new size (a snap, a restored place)
// shrinks to the shape inside it.
void HoldAspect(WINDOWPOS* pos, SIZE native, SIZE minimum);

// Shared, cached per size; never destroy the result.
HICON LoadAppIcon(int size);

// A monitor's effective DPI scale (1.0 = 96 DPI).
float DpiScaleFor(HMONITOR monitor);

// Reads a dialog field holding a secret; the stack copy is wiped.
std::wstring GetSecretText(HWND dlg, int id);

// Shows or masks a password edit control's text (a "Show" checkbox).
void RevealEditText(HWND edit, bool reveal);

// Matches the title bar to the Windows app theme; call again on WM_SETTINGCHANGE.
void ApplyTitleBarTheme(HWND hwnd);

// Diagnostic log at ConfigDir()\<name>.log, truncated on open. Lines carry a
// millisecond tick and thread id. No-op until LogOpen is called.
void LogOpen(const wchar_t* name);
void Log(const wchar_t* fmt, ...);
// Forces logged lines out of the system's cache onto the disk.
void LogFlush();

// Per-call-site sampling for per-frame events: the first few, then one in N.
#define RVM_LOG_SAMPLED(n, ...)                                          \
    do {                                                                 \
        static std::atomic<uint32_t> rvmLogCount_{ 0 };                  \
        const uint32_t rvmLogK_ = rvmLogCount_++;                        \
        if (rvmLogK_ < 5 || rvmLogK_ % (n) == 0) ::rvm::Log(__VA_ARGS__); \
    } while (0)

// Pumps one message for a nested loop. False when the loop must end; a
// WM_QUIT is re-posted so the outer loop still sees it.
bool PumpNestedMessage();

// Base for anything with a window procedure. The thunk keeps the object in
// GWLP_USERDATA and stops exceptions, which cannot unwind through the kernel.
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
