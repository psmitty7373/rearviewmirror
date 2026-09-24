#include "region.h"

namespace rvm {

namespace {

constexpr int kMinSelection = 12;   // In overlay pixels; anything smaller is a click.
constexpr ULONGLONG kArmDelayMs = 200;   // A double-click on the picker must not fall through.

class RegionOverlay : public D2DOverlay {
public:
    bool  dragging = false;
    bool  hasSelection = false;
    POINT anchor{};
    POINT cursor{};
    RECT  selection{};
    bool  done = false;
    bool  cancelled = false;
    float scaleX = 1.0f, scaleY = 1.0f;   // Overlay pixels -> capture pixels.
    ULONGLONG shownAt = 0;

    RECT WholeWindow() const {
        return RECT{ 0, 0, static_cast<LONG>(Width()), static_cast<LONG>(Height()) };
    }

    RECT CurrentRect() const {
        if (dragging) return NormalizeRect(anchor, cursor);
        if (hasSelection) return selection;
        return RECT{};
    }

    void Accept(const RECT& r) {
        selection = r;
        hasSelection = true;
        done = true;
    }

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;

        case WM_LBUTTONDOWN:
            if (GetTickCount64() - shownAt < kArmDelayMs) return 0;
            SetCapture(Hwnd());
            dragging = true;
            anchor = cursor = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            Render();
            return 0;

        case WM_MOUSEMOVE:
            if (dragging) {
                cursor = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                Render();
            }
            return 0;

        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                ReleaseCapture();
                cursor = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                const RECT r = NormalizeRect(anchor, cursor);
                // A drag narrows the mirror to a region; a click, the same
                // gesture that chose the window, takes all of it.
                if (RectW(r) >= kMinSelection && RectH(r) >= kMinSelection) Accept(r);
                else                                                       Accept(WholeWindow());
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                cancelled = true;
                done = true;
            } else if (wp == VK_RETURN) {
                Accept(WholeWindow());
            }
            return 0;

        case WM_RBUTTONUP:
        case WM_KILLFOCUS:   // Losing focus mid-selection would leave a stuck overlay.
            cancelled = true;
            done = true;
            return 0;

        default:
            break;
        }
        return D2DOverlay::OnMessage(msg, wp, lp);
    }

protected:
    void OnDraw(ID2D1DeviceContext* dc) override {
        const float w = static_cast<float>(Width());
        const float h = static_cast<float>(Height());

        brush_->SetColor(D2D1::ColorF(0.02f, 0.03f, 0.05f, 0.55f));

        const RECT sel = CurrentRect();
        const bool haveRect = RectW(sel) > 0 && RectH(sel) > 0;

        if (!haveRect) {
            dc->FillRectangle(D2D1::RectF(0, 0, w, h), brush_.get());
        } else {
            // Dim around the selection so the chosen content stays legible.
            const float l = static_cast<float>(sel.left),  t = static_cast<float>(sel.top);
            const float r = static_cast<float>(sel.right), b = static_cast<float>(sel.bottom);
            dc->FillRectangle(D2D1::RectF(0, 0, w, t), brush_.get());
            dc->FillRectangle(D2D1::RectF(0, b, w, h), brush_.get());
            dc->FillRectangle(D2D1::RectF(0, t, l, b), brush_.get());
            dc->FillRectangle(D2D1::RectF(r, t, w, b), brush_.get());

            brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.95f));
            dc->DrawRectangle(D2D1::RectF(l + 0.5f, t + 0.5f, r - 0.5f, b - 0.5f), brush_.get(), 1.5f);

            // Corner ticks give the selection a tactile, resizable feel.
            const float tick = (std::min)(18.0f, (std::min)(r - l, b - t) * 0.3f);
            const float th = 3.0f;
            const D2D1_RECT_F ticks[] = {
                { l, t, l + tick, t + th }, { l, t, l + th, t + tick },
                { r - tick, t, r, t + th }, { r - th, t, r, t + tick },
                { l, b - th, l + tick, b }, { l, b - tick, l + th, b },
                { r - tick, b - th, r, b }, { r - th, b - tick, r, b },
            };
            for (const auto& tr : ticks) dc->FillRectangle(tr, brush_.get());

            const int cw = static_cast<int>(std::lround(RectW(sel) * scaleX));
            const int ch = static_cast<int>(std::lround(RectH(sel) * scaleY));
            const std::wstring label = std::to_wstring(cw) + L" × " + std::to_wstring(ch);
            // Above the frame, else below it, else inside its bottom-left
            // corner when the frame fills the window.
            if (t > 34.0f)          DrawChip(dc, label, l, t - 8.0f, 0, 2);
            else if (b + 34.0f < h) DrawChip(dc, label, l, b + 8.0f, 0, 0);
            else                    DrawChip(dc, label, l + 8.0f, b - 8.0f, 0, 2);
        }

        if (!dragging) {
            DrawChip(dc, L"Click for the whole window   ·   Drag to choose a region   ·   Esc to cancel",
                     w * 0.5f, 20.0f, 1, 0);
        }
    }
};

// A minimised window reports the off-screen -32000 rect, which would put the
// overlay somewhere the user can never see. Restore it and wait for DWM.
bool EnsureVisible(HWND target) {
    if (!IsIconic(target)) return true;
    ShowWindow(target, SW_RESTORE);
    for (int i = 0; i < 20 && IsIconic(target); ++i) Sleep(25);
    if (IsIconic(target)) return false;
    DwmFlush();
    Sleep(50);
    return true;
}

}  // namespace

bool SelectRegion(HWND target, SIZE captureSize, RECT initial, RECT& out) {
    if (!IsWindow(target) || captureSize.cx <= 0 || captureSize.cy <= 0) return false;
    if (!EnsureVisible(target)) return false;

    // Make sure the user can actually see what they are selecting.
    SetForegroundWindow(target);
    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    const RECT bounds = ExtendedFrameBounds(target);
    if (RectW(bounds) <= 0 || RectH(bounds) <= 0 || bounds.left <= -30000) return false;

    RegionOverlay overlay;
    if (!overlay.Create(L"RvmRegionOverlay", bounds, /*clickThrough=*/false)) return false;

    overlay.scaleX = static_cast<float>(captureSize.cx) / static_cast<float>(RectW(bounds));
    overlay.scaleY = static_cast<float>(captureSize.cy) / static_cast<float>(RectH(bounds));

    // The frame starts on what a click would give: the previous region when
    // reselecting, otherwise the whole window, carrying the picker's highlight
    // straight over.
    overlay.hasSelection = true;
    if (RectW(initial) > 0 && RectH(initial) > 0) {
        overlay.selection = RECT{
            static_cast<LONG>(initial.left   / overlay.scaleX),
            static_cast<LONG>(initial.top    / overlay.scaleY),
            static_cast<LONG>(initial.right  / overlay.scaleX),
            static_cast<LONG>(initial.bottom / overlay.scaleY),
        };
    } else {
        overlay.selection = overlay.WholeWindow();
    }

    overlay.shownAt = GetTickCount64();
    overlay.Show();
    SetForegroundWindow(overlay.Hwnd());
    SetFocus(overlay.Hwnd());
    overlay.Render();

    while (!overlay.done && PumpNestedMessage()) {}
    if (!overlay.done) overlay.cancelled = true;   // The loop was ended by a quit.

    const bool ok = !overlay.cancelled && overlay.hasSelection;
    const RECT sel = overlay.selection;
    overlay.Destroy();
    if (!ok) return false;

    RECT mapped{
        static_cast<LONG>(std::lround(sel.left   * overlay.scaleX)),
        static_cast<LONG>(std::lround(sel.top    * overlay.scaleY)),
        static_cast<LONG>(std::lround(sel.right  * overlay.scaleX)),
        static_cast<LONG>(std::lround(sel.bottom * overlay.scaleY)),
    };
    mapped.left   = ClampI(mapped.left,   0, captureSize.cx);
    mapped.top    = ClampI(mapped.top,    0, captureSize.cy);
    mapped.right  = ClampI(mapped.right,  0, captureSize.cx);
    mapped.bottom = ClampI(mapped.bottom, 0, captureSize.cy);
    if (RectW(mapped) < 4 || RectH(mapped) < 4) return false;

    out = mapped;
    return true;
}

}  // namespace rvm
