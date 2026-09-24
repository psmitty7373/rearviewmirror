#include "picker.h"

namespace rvm {

namespace {

struct EnumState {
    POINT pt;
    HWND  hit;
};

BOOL CALLBACK HitTestProc(HWND hwnd, LPARAM lp) {
    auto* st = reinterpret_cast<EnumState*>(lp);
    if (!IsCapturableWindow(hwnd)) return TRUE;
    const RECT b = ExtendedFrameBounds(hwnd);
    if (PtInRect(&b, st->pt)) {
        st->hit = hwnd;   // EnumWindows walks front-to-back, so the first hit wins.
        return FALSE;
    }
    return TRUE;
}

HWND WindowAtPoint(POINT pt) {
    // WindowFromPoint skips our WS_EX_TRANSPARENT highlight and is one call;
    // the full walk is only the fallback for windows it will not report.
    if (HWND direct = WindowFromPoint(pt)) {
        direct = GetAncestor(direct, GA_ROOT);
        if (IsCapturableWindow(direct)) return direct;
    }
    EnumState st{ pt, nullptr };
    EnumWindows(&HitTestProc, reinterpret_cast<LPARAM>(&st));
    return st.hit;
}

void UpdateHover(POINT pt);

// Draws the accent frame and labels over the hovered window.
class Highlight : public D2DOverlay {
public:
    std::wstring title;

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_RVM_HOVER) {
            POINT pt{};
            GetCursorPos(&pt);
            UpdateHover(pt);
            return 0;
        }
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

        DrawChip(dc, Ellipsize(title, 64), 14.0f, 14.0f, 0, 0);
        DrawChip(dc, L"Click to mirror this window   ·   Esc to cancel",
                 w * 0.5f, h - 18.0f, 1, 2);
    }
};

struct PickSession {
    Highlight highlight;
    HWND  hovered = nullptr;
    HWND  result  = nullptr;
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

    HWND target = WindowAtPoint(pt);
    if (target == g_session->hovered) return;
    g_session->hovered = target;

    if (!target) {
        g_session->highlight.Hide();
        return;
    }
    g_session->highlight.title = WindowTitle(target);
    g_session->highlight.SetBounds(ExtendedFrameBounds(target));
    g_session->highlight.Show();
    g_session->highlight.Render();
}

// The low-level hook serialises every mouse event on the desktop, so it must
// return immediately. Resolving the window happens on our own turn, and a
// burst of moves collapses into one lookup.
void QueueHover() {
    if (!g_session || g_session->hoverQueued) return;
    g_session->hoverQueued = true;
    PostMessageW(g_session->highlight.Hwnd(), WM_RVM_HOVER, 0, 0);
}

void Finish(HWND result) {
    if (!g_session) return;
    g_session->result = result;
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
                Finish(WindowAtPoint(info->pt));
                return 1;
            }
            break;
        case WM_RBUTTONDOWN:
            return 1;
        case WM_RBUTTONUP:
            Finish(nullptr);
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
        if (reinterpret_cast<KBDLLHOOKSTRUCT*>(lp)->vkCode == VK_ESCAPE) {
            Finish(nullptr);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace

HWND PickWindow() {
    if (g_session) return nullptr;   // Already picking.

    PickSession session;
    g_session = &session;
    PickGuard guard{ session };

    if (!session.highlight.Create(L"RvmPickerOverlay", RECT{ 0, 0, 1, 1 }, /*clickThrough=*/true)) {
        return nullptr;
    }

    POINT pt{};
    GetCursorPos(&pt);
    UpdateHover(pt);

    session.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHook, GetModuleHandleW(nullptr), 0);
    session.keyHook   = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyHook, GetModuleHandleW(nullptr), 0);

    while (!session.done && PumpNestedMessage()) {}
    return session.done ? session.result : nullptr;
}

}  // namespace rvm
