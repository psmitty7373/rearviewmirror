#include "popout_window.h"
#include "persist.h"

namespace rvm {

namespace {

constexpr wchar_t kPopoutClass[] = L"RvmPopoutWindow";
constexpr int kMinWidth  = 64;
constexpr int kMinHeight = 48;
constexpr int kScreenMargin = 24;

enum MenuId : UINT {
    kIdReturn = 100,
    kIdControl,
    kIdClickThrough,
    kIdOpacity100 = 300, kIdOpacity90, kIdOpacity75, kIdOpacity50, kIdOpacity25,
};

struct OpacityLevel { UINT id; const wchar_t* label; float value; };
constexpr OpacityLevel kOpacityLevels[] = {
    { kIdOpacity100, L"100%", 1.00f }, { kIdOpacity90, L"90%", 0.90f },
    { kIdOpacity75,  L"75%",  0.75f }, { kIdOpacity50, L"50%", 0.50f },
    { kIdOpacity25,  L"25%",  0.25f },
};

int ClampExtent(int v, int minimum) {
    return ClampI(v, minimum, kMaxExtent);
}

}  // namespace

bool PopoutWindow::Create(HWND notify, LPARAM token, const Settings& settings, UINT nativeW,
                          UINT nativeH, HWND placeNear) {
    notify_ = notify;
    token_ = token;
    nativeW_ = (std::max)(nativeW, 1u);
    nativeH_ = (std::max)(nativeH, 1u);
    opacity_ = Clampf(settings.opacity, 0.05f, 1.0f);
    clickThrough_ = settings.clickThrough;

    const SIZE size = ScaleNative(Native(), 1.0, { kMinWidth, kMinHeight });
    RECT bounds{ 0, 0, size.cx, size.cy };
    const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE;
    if (!CreateStyled(kPopoutClass, kAppName, bounds, WS_POPUP | WS_THICKFRAME, ex, CS_DBLCLKS)) return false;

    if (clickThrough_) ApplyClickThroughStyle();

    if (RectW(settings.rect) >= kMinWidth && RectH(settings.rect) >= kMinHeight) {
        const RECT p = ClampToVisibleMonitor(settings.rect);
        SetWindowPos(hwnd_, HWND_TOPMOST, p.left, p.top, ClampExtent(RectW(p), kMinWidth),
                     ClampExtent(RectH(p), kMinHeight), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        PlaceInitially(placeNear);
    }
    Render();
    return true;
}

// Top-right of the client's monitor, no larger than 60% of it.
void PopoutWindow::PlaceInitially(HWND placeNear) {
    const RECT work = WorkAreaFor(placeNear ? placeNear : hwnd_);
    const int maxW = static_cast<int>(RectW(work) * 0.6);
    const int maxH = static_cast<int>(RectH(work) * 0.6);

    const double scale = (std::min)(1.0, (std::min)(static_cast<double>(maxW) / nativeW_,
                                                    static_cast<double>(maxH) / nativeH_));
    const SIZE s = ScaleNative(Native(), scale, { kMinWidth, kMinHeight });
    SetWindowPos(hwnd_, HWND_TOPMOST, work.right - s.cx - kScreenMargin, work.top + kScreenMargin,
                 s.cx, s.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

PopoutWindow::Settings PopoutWindow::CurrentSettings() const {
    Settings s;
    if (hwnd_) GetWindowRect(hwnd_, &s.rect);
    s.opacity = opacity_;
    s.clickThrough = clickThrough_;
    return s;
}

void PopoutWindow::SetFrame(const winrt::com_ptr<ID3D11Texture2D>& texture, UINT width,
                            UINT height, uint64_t frames) {
    texture_ = texture;
    frameW_ = width;
    frameH_ = height;
    frames_ = frames;
    if (width > 0 && height > 0 && (width != nativeW_ || height != nativeH_)) {
        nativeW_ = width;
        nativeH_ = height;
        FollowShape();
    }
}

// The window keeps its width and takes the stream's new shape.
void PopoutWindow::FollowShape() {
    if (!hwnd_) return;
    RECT r{};
    GetWindowRect(hwnd_, &r);
    const SIZE s = ScaleNative(Native(), static_cast<double>(RectW(r)) / nativeW_, { kMinWidth, kMinHeight });
    if (s.cx == RectW(r) && std::abs(s.cy - RectH(r)) <= 1) return;   // A size the encoder rounded.
    SetWindowPos(hwnd_, nullptr, 0, 0, s.cx, s.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Notify(PopoutEvent::Changed);
}

void PopoutWindow::SetControlLabel(std::wstring_view label) {
    if (controlLabel_ == label) return;
    controlLabel_.assign(label);
    Invalidate();
}

void PopoutWindow::Notify(PopoutEvent event) {
    if (notify_) PostMessageW(notify_, WM_RVM_POPOUT_EVENT, static_cast<WPARAM>(event), token_);
}

void PopoutWindow::ConstrainSizing(WPARAM edge, RECT* rect) {
    snapped_ = ConstrainToNative(edge, rect, Native(), { kMinWidth, kMinHeight });
}

void PopoutWindow::SetZoom(float factor) {
    if (!hwnd_) return;
    const SIZE s = ScaleNative(Native(), factor, { kMinWidth, kMinHeight });
    SetWindowPos(hwnd_, nullptr, 0, 0, s.cx, s.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Notify(PopoutEvent::Changed);
}

void PopoutWindow::SetClickThrough(bool on) {
    if (!hwnd_ || clickThrough_ == on) return;
    clickThrough_ = on;
    ApplyClickThroughStyle();
    Notify(PopoutEvent::Changed);
}

void PopoutWindow::ApplyClickThroughStyle() {
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (clickThrough_) ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
    else               ex &= ~(WS_EX_TRANSPARENT | WS_EX_LAYERED);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    if (clickThrough_) SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    hovered_ = false;
    snapped_ = false;
    Render();
}

LRESULT PopoutWindow::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (filter_ && filter_(hwnd_, msg, wp, lp)) {
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK ? TRUE : 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;

    case WM_NCHITTEST:
        return ResizeBorderHitTest(hwnd_, lp);

    case WM_LBUTTONDOWN:
        BeginWindowDrag(hwnd_, lp);
        return 0;

    case WM_LBUTTONDBLCLK:
        SetZoom(1.0f);
        return 0;

    case WM_MOUSEMOVE:
        if (!hovered_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            hovered_ = true;
            Invalidate();
        }
        return 0;

    case WM_MOUSELEAVE:
        hovered_ = false;
        Invalidate();
        return 0;

    case WM_DPICHANGED:
        // Chips follow the monitor's DPI; the window stays in stream pixels.
        UpdateChipScale();
        Render();
        return 0;

    case WM_SIZING:
        ConstrainSizing(wp, reinterpret_cast<RECT*>(lp));
        return TRUE;

    case WM_WINDOWPOSCHANGING:
        HoldAspect(reinterpret_cast<WINDOWPOS*>(lp), Native(), { kMinWidth, kMinHeight });
        break;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = kMinWidth;
        mmi->ptMinTrackSize.y = kMinHeight;
        mmi->ptMaxTrackSize.x = kMaxExtent;
        mmi->ptMaxTrackSize.y = kMaxExtent;
        return 0;
    }

    case WM_SIZE: {
        const UINT w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0) {
            ResizeSurface(w, h);
            Render();
        }
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        GetWindowRect(hwnd_, &sizeMoveStart_);
        return 0;

    case WM_EXITSIZEMOVE: {
        if (snapped_) {
            snapped_ = false;
            Render();
        }
        RECT r{};
        GetWindowRect(hwnd_, &r);
        if (!EqualRect(&r, &sizeMoveStart_)) Notify(PopoutEvent::Changed);   // Not for a plain click.
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd_, &pt);
        ShowContextMenu(pt);
        return 0;
    }

    case WM_CLOSE:
        Notify(PopoutEvent::ReturnToCanvas);
        return 0;

    default:
        break;
    }
    return D2DOverlay::OnMessage(msg, wp, lp);
}

void PopoutWindow::ShowContextMenu(POINT screenPt) {

    HMENU opacity = CreatePopupMenu();
    for (const auto& lv : kOpacityLevels) {
        const bool on = std::fabs(opacity_ - lv.value) < 0.01f;
        AppendMenuW(opacity, MF_STRING | (on ? MF_CHECKED : 0), lv.id, lv.label);
    }

    HMENU menu = CreatePopupMenu();
    if (controllable_) {
        AppendMenuW(menu, MF_STRING | (clickThrough_ ? MF_GRAYED : 0), kIdControl, L"Control desktop");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"Opacity");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (clickThrough_ ? MF_CHECKED : 0), kIdClickThrough,
                L"Click-through");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdReturn, L"Return to canvas");

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPt.x, screenPt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (!hwnd_) return;

    switch (cmd) {
    case kIdReturn:       Notify(PopoutEvent::ReturnToCanvas); break;
    case kIdControl:      Notify(PopoutEvent::Control); break;
    case kIdClickThrough: SetClickThrough(!clickThrough_); break;

    default:
        for (const auto& lv : kOpacityLevels) {
            if (cmd == lv.id) {
                opacity_ = lv.value;
                Render();
                Notify(PopoutEvent::Changed);
                break;
            }
        }
        break;
    }
}

void PopoutWindow::OnDraw(ID2D1DeviceContext* dc) {
    const float w = static_cast<float>(Width());
    const float h = static_cast<float>(Height());
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (texture_ && frameW_ > 0 && frameH_ > 0 && frames_ > 0) {
        if (bitmapFor_ != texture_.get()) {
            bitmap_ = nullptr;
            bitmapFor_ = nullptr;
            winrt::com_ptr<IDXGISurface> surface;
            auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (SUCCEEDED(texture_->QueryInterface(IID_PPV_ARGS(surface.put()))) &&
                SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap_.put()))) {
                bitmapFor_ = texture_.get();
            }
        }
        if (bitmap_) {
            const bool shrinking = w < frameW_;
            dc->DrawBitmap(bitmap_.get(), D2D1::RectF(0, 0, w, h), opacity_,
                           shrinking ? D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR
                                     : D2D1_INTERPOLATION_MODE_LINEAR);
        }
    } else {
        brush_->SetColor(D2D1::ColorF(0.02f, 0.03f, 0.05f, 0.85f * opacity_));
        dc->FillRectangle(D2D1::RectF(0, 0, w, h), brush_.get());
        DrawChip(dc, waitingText_, w * 0.5f, h * 0.5f, 1, 1);
    }

    if (!controlLabel_.empty()) {
        DrawChip(dc, controlLabel_, 10.0f * chipScale_, 10.0f * chipScale_, 0, 0, 0.75f);
        brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB));
        const float inset = 1.5f * chipScale_;
        dc->DrawRectangle(D2D1::RectF(inset, inset, w - inset, h - inset), brush_.get(), 3.0f * chipScale_);
    } else if (snapped_) {
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        dc->DrawRectangle(D2D1::RectF(0.75f, 0.75f, w - 0.75f, h - 0.75f), brush_.get(), 1.5f);
    } else if (hovered_) {
        brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.85f));
        dc->DrawRectangle(D2D1::RectF(0.75f, 0.75f, w - 0.75f, h - 0.75f), brush_.get(), 1.5f);
    }
}

}  // namespace rvm
