#include "picker.h"
#include "capture.h"

namespace rvm {

namespace {

// The shell's own surfaces: the wallpaper (with its icons) and the taskbars.
// Pointing at them means "the desktop itself", not any one window.
bool IsDesktopSurface(HWND root) {
    if (!root) return false;
    const std::wstring cls = WindowClassName(root);
    for (const wchar_t* s : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" }) {
        if (cls == s) return true;
    }
    return false;
}

bool IsOwnWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// Front-to-back walk for what WindowFromPoint cannot answer: it reports our
// own highlight, which covers whatever is being pointed at. That highlight,
// anything else of ours, and anything the mouse passes through are skipped.
struct WalkState {
    POINT pt;
    PickResult hit;
};

BOOL CALLBACK WalkProc(HWND hwnd, LPARAM lp) {
    auto* st = reinterpret_cast<WalkState*>(lp);
    if (!IsWindowVisible(hwnd) || IsOwnWindow(hwnd)) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) return TRUE;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }

    if (IsDesktopSurface(hwnd)) {
        RECT r{};
        GetWindowRect(hwnd, &r);
        if (!PtInRect(&r, st->pt)) return TRUE;
        st->hit = PickResult{ nullptr, true };
        return FALSE;
    }
    if (!IsCapturableWindow(hwnd)) return TRUE;
    const RECT b = ExtendedFrameBounds(hwnd);
    if (!PtInRect(&b, st->pt)) return TRUE;
    st->hit = PickResult{ hwnd, false };   // Front-to-back, so the first hit wins.
    return FALSE;
}

PickResult SourceAtPoint(POINT pt) {
    // One call covers the usual case: a window that is not ours.
    if (HWND direct = WindowFromPoint(pt)) {
        direct = GetAncestor(direct, GA_ROOT);
        if (!IsOwnWindow(direct)) {
            if (IsDesktopSurface(direct)) return PickResult{ nullptr, true };
            if (IsCapturableWindow(direct)) return PickResult{ direct, false };
        }
    }
    WalkState st{ pt, {} };
    EnumWindows(&WalkProc, reinterpret_cast<LPARAM>(&st));
    return st.hit;
}

void UpdateHover(POINT pt);

// Draws the accent frame and labels over the hovered window, or over every
// screen when the desktop itself is pointed at.
class Highlight : public D2DOverlay {
public:
    std::wstring title;
    bool desktop = false;
    // Where the labels go, in overlay pixels: the whole window, or for the
    // desktop the monitor under the pointer, so they never straddle a seam.
    RECT labelArea{};

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_RVM_HOVER) {
            POINT pt{};
            GetCursorPos(&pt);
            UpdateHover(pt);
            return 0;
        }
        // Not even there for WindowFromPoint, which asks windows on its own
        // thread (ours) rather than going by WS_EX_TRANSPARENT.
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return D2DOverlay::OnMessage(msg, wp, lp);
    }

protected:
    void OnDraw(ID2D1DeviceContext* dc) override {
        const float w = static_cast<float>(Width());
        const float h = static_cast<float>(Height());
        if (w < 2.0f || h < 2.0f) return;

        brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.12f));
        const float inset = 1.5f;
        const D2D1_ROUNDED_RECT rr{ { inset, inset, w - inset, h - inset }, 6.0f, 6.0f };
        dc->FillRoundedRectangle(rr, brush_.get());

        brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.95f));
        dc->DrawRoundedRectangle(rr, brush_.get(), 3.0f);

        const bool area = RectW(labelArea) > 0 && RectH(labelArea) > 0;
        const float l = area ? static_cast<float>(labelArea.left) : 0.0f;
        const float t = area ? static_cast<float>(labelArea.top) : 0.0f;
        const float r = area ? static_cast<float>(labelArea.right) : w;
        const float b = area ? static_cast<float>(labelArea.bottom) : h;
        DrawChip(dc, Ellipsize(title, 64), l + 14.0f, t + 14.0f, 0, 0);
        DrawChip(dc, desktop
                     ? L"Click to mirror the whole desktop   ·   Esc to cancel"
                     : L"Click to mirror this window   ·   D for the whole desktop   ·   Esc to cancel",
                 (l + r) * 0.5f, b - 18.0f, 1, 2);
    }
};

struct PickSession {
    Highlight highlight;
    HWND  hovered = nullptr;
    bool  hoveredDesktop = false;
    HMONITOR hoveredMonitor = nullptr;   // For the desktop's labels.
    POINT clickPoint{};
    bool  clicked = false;   // Else cancelled.
    bool  desktop = false;   // D was pressed: the whole desktop, wherever the pointer is.
    bool  done    = false;
    bool  pressed = false;
    bool  hoverQueued = false;
    HHOOK mouseHook = nullptr;
    HHOOK keyHook   = nullptr;
};

PickSession* g_session = nullptr;

// Unhooks and clears the session pointer on every exit path, including an
// exception thrown out of the nested loop: a hook left installed would keep
// reading a dead stack frame and swallowing clicks.
struct PickGuard {
    PickSession& session;
    ~PickGuard() {
        if (session.mouseHook) UnhookWindowsHookEx(session.mouseHook);
        if (session.keyHook)   UnhookWindowsHookEx(session.keyHook);
        session.highlight.Destroy();
        g_session = nullptr;
    }
};

void UpdateHover(POINT pt) {
    if (!g_session) return;
    g_session->hoverQueued = false;

    const PickResult source = SourceAtPoint(pt);
    Highlight& highlight = g_session->highlight;

    if (source.desktop) {
        // Every screen lights up; the labels follow the pointer's monitor.
        const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (g_session->hoveredDesktop && monitor == g_session->hoveredMonitor) return;
        const bool alreadyShown = g_session->hoveredDesktop;
        g_session->hovered = nullptr;
        g_session->hoveredDesktop = true;
        g_session->hoveredMonitor = monitor;

        const RECT bounds = DesktopCapture::Bounds();
        MONITORINFO mi{ sizeof(mi) };
        RECT labels{};
        if (GetMonitorInfoW(monitor, &mi)) {
            labels = mi.rcWork;   // Above the taskbar, not on it.
            OffsetRect(&labels, -bounds.left, -bounds.top);
        }
        highlight.desktop = true;
        highlight.title = L"Entire desktop";
        highlight.labelArea = labels;
        // Crossing to another monitor only moves the labels. Re-placing a
        // topmost window over every screen, taskbars included, makes the
        // shell re-lay out the taskbar.
        if (!alreadyShown) {
            highlight.SetBounds(bounds);
            highlight.Show();
        }
        highlight.Render();
        return;
    }

    HWND target = source.window;
    if (target == g_session->hovered && !g_session->hoveredDesktop) return;
    g_session->hovered = target;
    g_session->hoveredDesktop = false;
    g_session->hoveredMonitor = nullptr;

    if (!target) {
        highlight.Hide();
        return;
    }
    highlight.desktop = false;
    highlight.title = WindowTitle(target);
    highlight.labelArea = RECT{};
    highlight.SetBounds(ExtendedFrameBounds(target));
    highlight.Show();
    highlight.Render();
}

// The low-level hook serialises every mouse event on the desktop, so it must
// return immediately. Resolving the window happens on our own turn, and a
// burst of moves collapses into one lookup.
void QueueHover() {
    if (!g_session || g_session->hoverQueued) return;
    g_session->hoverQueued = true;
    PostMessageW(g_session->highlight.Hwnd(), WM_RVM_HOVER, 0, 0);
}

// Only records the outcome: the window under a click is looked up once the
// loop wakes, outside the hook.
void Finish(bool clicked, POINT pt = {}) {
    if (!g_session) return;
    g_session->clicked = clicked;
    g_session->clickPoint = pt;
    g_session->done = true;
    PostMessageW(g_session->highlight.Hwnd(), WM_NULL, 0, 0);   // Wake the loop.
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_session && !g_session->done) {
        auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        switch (wp) {
        case WM_MOUSEMOVE:
            QueueHover();
            break;
        case WM_LBUTTONDOWN:
            g_session->pressed = true;
            return 1;   // Swallow so the target app never sees the click.
        case WM_LBUTTONUP:
            if (g_session->pressed) {
                g_session->pressed = false;
                Finish(true, info->pt);
                return 1;
            }
            break;
        case WM_RBUTTONDOWN:
            return 1;
        case WM_RBUTTONUP:
            Finish(false);
            return 1;
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_session && !g_session->done &&
        (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        const DWORD key = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp)->vkCode;
        if (key == VK_ESCAPE) {
            Finish(false);
            return 1;
        }
        // Plain D only: a shortcut held with it belongs to someone else.
        const bool modified = (GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) |
                               GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000;
        if (key == 'D' && !modified) {
            g_session->desktop = true;
            Finish(false);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace

PickResult PickSource() {
    if (g_session) return {};   // Already picking.

    PickSession session;
    g_session = &session;
    PickGuard guard{ session };

    if (!session.highlight.Create(L"RvmPickerOverlay", RECT{ 0, 0, 1, 1 }, /*clickThrough=*/true)) {
        return {};
    }

    POINT pt{};
    GetCursorPos(&pt);
    UpdateHover(pt);

    session.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHook, GetModuleHandleW(nullptr), 0);
    session.keyHook   = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyHook, GetModuleHandleW(nullptr), 0);

    while (!session.done && PumpNestedMessage()) {}
    PickResult result;
    if (!session.done) return result;
    if (session.clicked) result = SourceAtPoint(session.clickPoint);
    if (session.desktop) result = PickResult{ nullptr, true };
    return result;
}

}  // namespace rvm
