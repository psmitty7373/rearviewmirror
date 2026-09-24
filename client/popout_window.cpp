#include "popout_window.h"
#include "persist.h"

namespace rvm {

namespace {

constexpr wchar_t kPopoutClass[] = L"RvmPopoutWindow";
constexpr int kResizeBorder = 7;
constexpr int kMinWidth  = 64;
constexpr int kMinHeight = 48;
constexpr int kScreenMargin = 24;
constexpr int kSnapPx = 14;

enum MenuId : UINT {
    kIdReturn = 100,
    kIdAspectLock,
    kIdClickThrough,
    kIdZoom50 = 200, kIdZoom100, kIdZoom150, kIdZoom200,
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
    aspectLocked_ = settings.aspectLocked;

    RECT bounds{ 0, 0, ClampExtent(static_cast<int>(nativeW_), kMinWidth),
                 ClampExtent(static_cast<int>(nativeH_), kMinHeight) };
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

    int w = (std::max)(static_cast<int>(nativeW_), kMinWidth);
    int h = (std::max)(static_cast<int>(nativeH_), kMinHeight);
    const float scale = (std::min)(1.0f, (std::min)(static_cast<float>(maxW) / w,
                                                    static_cast<float>(maxH) / h));
    w = ClampExtent(static_cast<int>(w * scale), kMinWidth);
    h = ClampExtent(static_cast<int>(h * scale), kMinHeight);
    SetWindowPos(hwnd_, HWND_TOPMOST, work.right - w - kScreenMargin, work.top + kScreenMargin,
                 w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

PopoutWindow::Settings PopoutWindow::CurrentSettings() const {
    Settings s;
    if (hwnd_) GetWindowRect(hwnd_, &s.rect);
    s.opacity = opacity_;
    s.clickThrough = clickThrough_;
    s.aspectLocked = aspectLocked_;
    return s;
}

void PopoutWindow::SetFrame(const winrt::com_ptr<ID3D11Texture2D>& texture, UINT width,
                            UINT height, uint64_t frames) {
    texture_ = texture;
    frameW_ = width;
    frameH_ = height;
    frames_ = frames;
    if (width > 0 && height > 0) {
        nativeW_ = width;
        nativeH_ = height;
    }
}

void PopoutWindow::Notify(PopoutEvent event) {
    if (notify_) PostMessageW(notify_, WM_RVM_POPOUT_EVENT, static_cast<WPARAM>(event), token_);
}

// Same rules as the app's mirror window: the drag is held to the stream's
// aspect ratio and sticks at 1:1 unless Ctrl is down.
void PopoutWindow::ConstrainSizing(WPARAM edge, RECT* rect) {
    const int nativeW = static_cast<int>(nativeW_), nativeH = static_cast<int>(nativeH_);
    if (nativeW <= 0 || nativeH <= 0) return;

    int w = RectW(*rect);
    int h = RectH(*rect);
    const bool horizontal = (edge == WMSZ_LEFT || edge == WMSZ_RIGHT);
    const bool vertical   = (edge == WMSZ_TOP  || edge == WMSZ_BOTTOM);
    const double aspect   = static_cast<double>(nativeW) / static_cast<double>(nativeH);

    if (aspectLocked_) {
        if (horizontal) {
            h = static_cast<int>(std::lround(w / aspect));
        } else if (vertical) {
            w = static_cast<int>(std::lround(h * aspect));
        } else {
            const double projected = (w + h / aspect) * (aspect * aspect) / (aspect * aspect + 1.0);
            w = static_cast<int>(std::lround(projected));
            h = static_cast<int>(std::lround(projected / aspect));
        }
    }

    if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (aspectLocked_) {
            if (std::abs(w - nativeW) <= kSnapPx) {
                w = nativeW;
                h = nativeH;
            }
        } else {
            if (std::abs(w - nativeW) <= kSnapPx) w = nativeW;
            if (std::abs(h - nativeH) <= kSnapPx) h = nativeH;
        }
    }

    w = ClampExtent(w, kMinWidth);
    h = ClampExtent(h, kMinHeight);
    snapped_ = (w == nativeW && h == nativeH);

    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
        rect->left = rect->right - w;
    } else {
        rect->right = rect->left + w;
    }
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
        rect->top = rect->bottom - h;
    } else {
        rect->bottom = rect->top + h;
    }
}

void PopoutWindow::SetZoom(float factor) {
    if (!hwnd_) return;
    const int w = ClampExtent(static_cast<int>(nativeW_ * factor), kMinWidth);
    const int h = ClampExtent(static_cast<int>(nativeH_ * factor), kMinHeight);
    SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r{};
        GetWindowRect(hwnd_, &r);
        const int border = MulDiv(kResizeBorder, static_cast<int>(GetDpiForWindow(hwnd_)), 96);
        const bool left   = pt.x < r.left + border;
        const bool right  = pt.x >= r.right - border;
        const bool top    = pt.y < r.top + border;
        const bool bottom = pt.y >= r.bottom - border;
        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)   return HTLEFT;
        if (right)  return HTRIGHT;
        if (top)    return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd_, &pt);
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        SetZoom(1.0f);
        return 0;

    case WM_MOUSEMOVE:
        if (!hovered_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            hovered_ = true;
            Render();
        }
        return 0;

    case WM_MOUSELEAVE:
        hovered_ = false;
        Render();
        return 0;

    case WM_DPICHANGED:
        // Chips follow the monitor's DPI; the window stays in stream pixels.
        UpdateChipScale();
        Render();
        return 0;

    case WM_SIZING:
        ConstrainSizing(wp, reinterpret_cast<RECT*>(lp));
        return TRUE;

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

    case WM_EXITSIZEMOVE:
        snapped_ = false;
        Render();
        Notify(PopoutEvent::Changed);
        return 0;

    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd_, &pt);
        ShowContextMenu(pt);
        return 0;
    }

    case WM_CLOSE:
        Notify(PopoutEvent::ReturnToGrid);
        return 0;

    default:
        break;
    }
    return D2DOverlay::OnMessage(msg, wp, lp);
}

void PopoutWindow::ShowContextMenu(POINT screenPt) {
    HMENU zoom = CreatePopupMenu();
    AppendMenuW(zoom, MF_STRING, kIdZoom50,  L"50%");
    AppendMenuW(zoom, MF_STRING, kIdZoom100, L"100%  (actual size)");
    AppendMenuW(zoom, MF_STRING, kIdZoom150, L"150%");
    AppendMenuW(zoom, MF_STRING, kIdZoom200, L"200%");

    HMENU opacity = CreatePopupMenu();
    for (const auto& lv : kOpacityLevels) {
        const bool on = std::fabs(opacity_ - lv.value) < 0.01f;
        AppendMenuW(opacity, MF_STRING | (on ? MF_CHECKED : 0), lv.id, lv.label);
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom), L"Zoom");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"Opacity");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (aspectLocked_ ? MF_CHECKED : 0), kIdAspectLock,
                L"Lock aspect ratio");
    AppendMenuW(menu, MF_STRING | (clickThrough_ ? MF_CHECKED : 0), kIdClickThrough,
                L"Click-through");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdReturn, L"Return to grid");

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPt.x, screenPt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (!hwnd_) return;

    switch (cmd) {
    case kIdReturn:       Notify(PopoutEvent::ReturnToGrid); break;
    case kIdAspectLock:   aspectLocked_ = !aspectLocked_; Render(); Notify(PopoutEvent::Changed); break;
    case kIdClickThrough: SetClickThrough(!clickThrough_); break;
    case kIdZoom50:  SetZoom(0.5f); break;
    case kIdZoom100: SetZoom(1.0f); break;
    case kIdZoom150: SetZoom(1.5f); break;
    case kIdZoom200: SetZoom(2.0f); break;
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
            D2D1_RECT_F dest = D2D1::RectF(0, 0, w, h);
            if (!aspectLocked_) {
                // Letterbox, since the window may no longer match the stream.
                const float scale = (std::min)(w / frameW_, h / frameH_);
                const float dw = frameW_ * scale, dh = frameH_ * scale;
                dest = D2D1::RectF((w - dw) * 0.5f, (h - dh) * 0.5f, (w + dw) * 0.5f, (h + dh) * 0.5f);
            }
            const bool shrinking = (dest.right - dest.left) < frameW_;
            dc->DrawBitmap(bitmap_.get(), dest, opacity_,
                           shrinking ? D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR
                                     : D2D1_INTERPOLATION_MODE_LINEAR);
        }
    } else {
        brush_->SetColor(D2D1::ColorF(0.02f, 0.03f, 0.05f, 0.85f * opacity_));
        dc->FillRectangle(D2D1::RectF(0, 0, w, h), brush_.get());
        DrawChip(dc, L"Waiting for the first frame…", w * 0.5f, h * 0.5f, 1, 1);
    }

    if (snapped_) {
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        dc->DrawRectangle(D2D1::RectF(0.75f, 0.75f, w - 0.75f, h - 0.75f), brush_.get(), 1.5f);
    } else if (hovered_) {
        brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.85f));
        dc->DrawRectangle(D2D1::RectF(0.75f, 0.75f, w - 0.75f, h - 0.75f), brush_.get(), 1.5f);
    }
}

}  // namespace rvm
