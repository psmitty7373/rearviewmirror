#include "mirror.h"
#include "region.h"

namespace rvm {

namespace {

constexpr wchar_t kMirrorClass[] = L"RvmMirrorWindow";
constexpr int kResizeBorder = 7;
constexpr int kMinWidth  = 64;
constexpr int kMinHeight = 48;
constexpr int kScreenMargin = 24;
constexpr int kSnapPx = 14;   // Grab range around 100%.

enum MenuId : UINT {
    kIdReselectRegion = 100,
    kIdAspectLock,
    kIdClickThrough,
    kIdHide,
    kIdClose,
    kIdTrackAnchored,
    kIdTrackProportional,
    kIdZoom50 = 200, kIdZoom100, kIdZoom150, kIdZoom200,
    kIdOpacity100 = 300, kIdOpacity90, kIdOpacity75, kIdOpacity50, kIdOpacity25,
};

struct OpacityLevel { UINT id; const wchar_t* label; float value; };
constexpr OpacityLevel kOpacityLevels[] = {
    { kIdOpacity100, L"100%", 1.00f }, { kIdOpacity90, L"90%", 0.90f },
    { kIdOpacity75,  L"75%",  0.75f }, { kIdOpacity50, L"50%", 0.50f },
    { kIdOpacity25,  L"25%",  0.25f },
};

void RegisterMirrorClass() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = &WndProcThunk<Mirror, &Mirror::WndProc>;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kMirrorClass;
        RegisterClassExW(&wc);
    });
}

int ClampExtent(int v, int minimum) {
    return ClampI(v, minimum, kMaxExtent);
}

}  // namespace

Mirror::~Mirror() {
    Destroy();
}

// Resizing scales the same slice of the source; it never reveals more of it.
// The drag is held to the crop's aspect ratio and sticks at 1:1.
void Mirror::ConstrainSizing(WPARAM edge, RECT* rect) {
    const SIZE native = renderer_.EffectiveCropSize();
    if (native.cx <= 0 || native.cy <= 0) return;

    int w = RectW(*rect);
    int h = RectH(*rect);

    const bool horizontal = (edge == WMSZ_LEFT || edge == WMSZ_RIGHT);
    const bool vertical   = (edge == WMSZ_TOP  || edge == WMSZ_BOTTOM);
    const double aspect   = static_cast<double>(native.cx) / static_cast<double>(native.cy);

    if (state_.aspectLocked) {
        if (horizontal) {
            h = static_cast<int>(std::lround(w / aspect));
        } else if (vertical) {
            w = static_cast<int>(std::lround(h * aspect));
        } else {
            // Corner drag: project onto the aspect line so diagonal movement
            // tracks and shrinking feels the same as growing.
            const double projected =
                (w + h / aspect) * (aspect * aspect) / (aspect * aspect + 1.0);
            w = static_cast<int>(std::lround(projected));
            h = static_cast<int>(std::lround(projected / aspect));
        }
    }

    // Ctrl slides straight past 100%.
    if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (state_.aspectLocked) {
            if (std::abs(w - native.cx) <= kSnapPx) {
                w = native.cx;
                h = native.cy;
            }
        } else {
            if (std::abs(w - native.cx) <= kSnapPx) w = native.cx;
            if (std::abs(h - native.cy) <= kSnapPx) h = native.cy;
        }
    }

    w = ClampExtent(w, kMinWidth);
    h = ClampExtent(h, kMinHeight);

    const bool nowSnapped = (w == native.cx && h == native.cy);
    if (nowSnapped != snapped_) {
        snapped_ = nowSnapped;
        UpdateBorder();   // The WM_SIZE that follows repaints.
    }

    // Keep whichever edges the user is not dragging pinned.
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

void Mirror::PlaceInitially() {
    const RECT work = WorkAreaFor(target_ ? target_ : hwnd_);
    const int maxW = static_cast<int>(RectW(work) * 0.6);
    const int maxH = static_cast<int>(RectH(work) * 0.6);

    int w = (std::max)(RectW(state_.crop), kMinWidth);
    int h = (std::max)(RectH(state_.crop), kMinHeight);

    const float scale = (std::min)(1.0f, (std::min)(static_cast<float>(maxW) / w,
                                                    static_cast<float>(maxH) / h));
    w = ClampExtent(static_cast<int>(w * scale), kMinWidth);
    h = ClampExtent(static_cast<int>(h * scale), kMinHeight);

    // Top-right corner of the target's monitor, out of the way.
    SetWindowPos(hwnd_, HWND_TOPMOST, work.right - w - kScreenMargin,
                 work.top + kScreenMargin, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

bool Mirror::StartCapture() {
    if (IsDesktop()) {
        return desktop_.Start(
            [this](ID3D11Texture2D* tex, UINT w, UINT h) { renderer_.SubmitFrame(tex, w, h); });
    }
    HWND self = hwnd_;
    return capture_.Start(
        target_,
        [this](ID3D11Texture2D* tex, UINT w, UINT h) { renderer_.SubmitFrame(tex, w, h); },
        [self] { PostMessageW(self, WM_RVM_TARGET_LOST, 0, 0); });
}

void Mirror::StopCapture() {
    capture_.Stop();
    desktop_.Stop();
}

SIZE Mirror::ContentSize() const {
    return IsDesktop() ? desktop_.ContentSize() : capture_.ContentSize();
}

bool Mirror::Create(const MirrorState& state, HWND target, HWND notify) {
    RegisterMirrorClass();

    state_  = state;
    target_ = IsDesktop() ? nullptr : target;
    notify_ = notify;

    if (IsDesktop()) {
        if (state_.baseSize.cx <= 0 || state_.baseSize.cy <= 0) {
            const RECT desk = DesktopCapture::Bounds();
            state_.baseSize = SIZE{ RectW(desk), RectH(desk) };
        }
    } else {
        if (!target && state_.enabled) return false;   // Only a disabled mirror may start unbound.
        if (target && (state_.baseSize.cx <= 0 || state_.baseSize.cy <= 0)) {
            state_.baseSize = CaptureItemSize(target);
        }
    }

    const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP |
                     WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(ex, kMirrorClass, kAppName, WS_POPUP | WS_THICKFRAME,
                            0, 0, ClampExtent(RectW(state_.crop), kMinWidth),
                            ClampExtent(RectH(state_.crop), kMinHeight),
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    // A desktop mirror on screen would otherwise capture itself, and itself
    // inside that, down a hall of mirrors.
    if (IsDesktop() && !SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        Log(L"mirror: could not exclude the desktop mirror from capture (%lu)", GetLastError());
    }

    if (RectW(state_.placement) >= kMinWidth && RectH(state_.placement) >= kMinHeight) {
        const RECT p = ClampToVisibleMonitor(state_.placement);
        SetWindowPos(hwnd_, HWND_TOPMOST, p.left, p.top,
                     ClampExtent(RectW(p), kMinWidth), ClampExtent(RectH(p), kMinHeight),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        PlaceInitially();
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!renderer_.Init(hwnd_, static_cast<UINT>(RectW(client)), static_cast<UINT>(RectH(client)))) {
        Destroy();
        return false;
    }
    renderer_.SetCrop(state_.crop, state_.baseSize);
    renderer_.SetTracking(state_.track);
    renderer_.SetOpacity(state_.opacity);
    if (state_.clickThrough) ApplyClickThroughStyle();

    if (state_.enabled && !StartCapture()) {
        Destroy();
        return false;
    }
    UpdateVisibility();
    return true;
}

void Mirror::UpdateVisibility() {
    if (!hwnd_) return;
    const bool visible = state_.enabled && !orphaned_ && !state_.hidden;
    renderer_.SetPresenting(visible);
    if (visible) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        renderer_.Redraw();
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

bool Mirror::SetEnabled(bool enabled, const std::vector<HWND>& exclude, HWND preferred) {
    if (!hwnd_) return false;
    if (state_.enabled == enabled) return true;

    if (!enabled) {
        StopCapture();
        state_.enabled = false;
        orphaned_ = false;   // Off is a deliberate state; nothing to wait for.
        UpdateVisibility();
        NotifyStateChanged();
        return true;
    }

    // The source may have closed or restarted while we were off. Another
    // mirror of the same window already knows where it is now.
    if (!IsDesktop() && !IsWindow(target_)) {
        HWND found = (preferred && IsWindow(preferred)) ? preferred : FindMatchingWindow(state_, exclude);
        if (!found) return false;
        target_ = found;
        if (state_.baseSize.cx <= 0 || state_.baseSize.cy <= 0) {
            state_.baseSize = CaptureItemSize(target_);
        }
    }

    if (!StartCapture()) return false;

    state_.enabled = true;
    UpdateVisibility();
    NotifyStateChanged();
    return true;
}

void Mirror::SetHidden(bool hidden) {
    if (!hwnd_ || state_.hidden == hidden) return;
    state_.hidden = hidden;
    UpdateVisibility();
    NotifyStateChanged();
}

void Mirror::Orphan() {
    StopCapture();
    target_ = nullptr;
    orphaned_ = true;
    UpdateVisibility();
}

bool Mirror::RestartDesktop() {
    if (!hwnd_ || !IsDesktop() || !state_.enabled) return true;
    StopCapture();
    if (StartCapture()) {
        orphaned_ = false;
        UpdateVisibility();
        return true;
    }
    // Mid-reconfiguration the monitors can refuse for a moment: wait and
    // retry on the same timer as a mirror whose window has gone.
    orphaned_ = true;
    UpdateVisibility();
    if (notify_) PostMessageW(notify_, WM_RVM_MIRROR_ORPHANED, id_, 0);
    return false;
}

bool Mirror::TryRebind(const std::vector<HWND>& exclude, HWND preferred) {
    if (!hwnd_ || !orphaned_) return false;

    if (IsDesktop()) {
        if (!StartCapture()) return false;
        orphaned_ = false;
        UpdateVisibility();
        return true;
    }

    HWND found = (preferred && IsWindow(preferred)) ? preferred : FindMatchingWindow(state_, exclude);
    if (!found) return false;

    target_ = found;
    if (state_.baseSize.cx <= 0 || state_.baseSize.cy <= 0) {
        state_.baseSize = CaptureItemSize(target_);
    }
    if (!StartCapture()) {
        target_ = nullptr;
        return false;
    }

    orphaned_ = false;
    UpdateVisibility();
    return true;
}

std::wstring Mirror::DisplayName() const {
    if (IsDesktop()) {
        const bool whole = state_.crop.left <= 0 && state_.crop.top <= 0 &&
                           state_.crop.right >= state_.baseSize.cx &&
                           state_.crop.bottom >= state_.baseSize.cy;
        return whole ? L"Entire desktop" : L"Desktop region";
    }
    std::wstring name = IsWindow(target_) ? WindowTitle(target_) : state_.title;
    if (name.empty()) name = state_.exeName;
    if (name.empty()) name = L"Untitled window";
    return Ellipsize(std::move(name), 48);
}

void Mirror::Destroy() {
    StopCapture();
    renderer_.Shutdown();
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
}

MirrorState Mirror::SaveState() const {
    MirrorState s = state_;
    if (hwnd_ && IsWindow(hwnd_)) GetWindowRect(hwnd_, &s.placement);
    return s;
}

void Mirror::NotifyStateChanged() {
    if (notify_) PostMessageW(notify_, WM_RVM_STATE_CHANGED, 0, 0);
}

void Mirror::SetClickThrough(bool enabled) {
    if (!hwnd_ || state_.clickThrough == enabled) return;
    state_.clickThrough = enabled;
    ApplyClickThroughStyle();
    NotifyStateChanged();
}

void Mirror::ApplyClickThroughStyle() {
    const bool enabled = state_.clickThrough;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enabled) {
        // WS_EX_TRANSPARENT only diverts the mouse once the window is layered
        // too; on its own the hit test still lands on us.
        ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
    } else {
        ex &= ~(WS_EX_TRANSPARENT | WS_EX_LAYERED);
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);

    // A layered window with no attributes set can stay blank. Full alpha
    // here: the mirror's own opacity is applied by the shader.
    if (enabled) SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    // Ex-style edits take effect when the frame is recalculated.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // No WM_MOUSELEAVE arrives once the mouse is passing through.
    hovered_ = false;
    snapped_ = false;
    UpdateBorder();
    renderer_.Redraw();
}

void Mirror::SetOpacity(float opacity, bool persist) {
    state_.opacity = Clampf(opacity, 0.05f, 1.0f);
    renderer_.SetOpacity(state_.opacity);
    renderer_.Redraw();
    if (persist) NotifyStateChanged();
}

SIZE Mirror::NativeSize() const {
    return renderer_.EffectiveCropSize();
}

float Mirror::CurrentScale() const {
    const SIZE native = renderer_.EffectiveCropSize();
    if (!hwnd_ || native.cx <= 0) return 1.0f;
    RECT r{};
    GetWindowRect(hwnd_, &r);
    return static_cast<float>(RectW(r)) / static_cast<float>(native.cx);
}

void Mirror::SetTracking(TrackMode mode) {
    state_.track = mode;
    renderer_.SetTracking(mode);
    renderer_.Redraw();
    NotifyStateChanged();
}

void Mirror::SetZoom(float factor, bool persist) {
    if (!hwnd_) return;
    const SIZE native = renderer_.EffectiveCropSize();
    if (native.cx <= 0 || native.cy <= 0) return;
    const int w = ClampExtent(static_cast<int>(native.cx * factor), kMinWidth);
    const int h = ClampExtent(static_cast<int>(native.cy * factor), kMinHeight);
    SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (persist) NotifyStateChanged();
}

void Mirror::ReselectRegion() {
    if (!IsDesktop() && !IsWindow(target_)) return;
    const SIZE content = ContentSize();
    if (content.cx <= 0 || content.cy <= 0) return;

    // Get out of the way so the source is fully visible.
    ShowWindow(hwnd_, SW_HIDE);
    RECT picked{};
    const bool ok = IsDesktop()
        ? SelectScreenRegion(DesktopCapture::Bounds(), content, renderer_.EffectiveCrop(), picked)
        : SelectRegion(target_, content, renderer_.EffectiveCrop(), picked);
    if (!hwnd_) return;   // Retired while the selector was up.
    UpdateVisibility();
    if (!ok) return;

    state_.crop     = picked;
    state_.baseSize = content;
    renderer_.SetCrop(state_.crop, state_.baseSize);
    SetZoom(1.0f, /*persist=*/false);
    renderer_.Redraw();
    NotifyStateChanged();
}

void Mirror::UpdateBorder() {
    if (snapped_) {
        renderer_.SetBorder(1.0f, 1.0f, 1.0f, 0.95f);   // The detent, visible not just felt.
    } else {
        renderer_.SetBorder(kAccentR, kAccentG, kAccentB, hovered_ ? 0.85f : 0.0f);
    }
}

LRESULT Mirror::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;   // Client area fills the whole window.
        break;

    case WM_NCHITTEST: {
        // Edges resize through DefWindowProc. The interior is client, so the
        // ordinary mouse messages below are delivered; the drag is started by
        // hand from WM_LBUTTONDOWN.
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

    case WM_MOUSEMOVE:
        if (!hovered_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            hovered_ = true;
            UpdateBorder();
            renderer_.Redraw();
        }
        return 0;

    case WM_MOUSELEAVE:
        hovered_ = false;
        UpdateBorder();
        renderer_.Redraw();
        return 0;

    case WM_LBUTTONDBLCLK:
        SetZoom(1.0f);
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
        if (w > 0 && h > 0) renderer_.Resize(w, h);
        return 0;
    }

    case WM_EXITSIZEMOVE:
        if (snapped_) {   // The detent cue belongs to the drag, not the size.
            snapped_ = false;
            UpdateBorder();
            renderer_.Redraw();
        }
        NotifyStateChanged();
        return 0;

    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd_, &pt);
        ShowContextMenu(pt);
        return 0;
    }

    case WM_RVM_TARGET_LOST:
        // The source closed. Wait for it rather than forgetting the mirror.
        Orphan();
        if (notify_) PostMessageW(notify_, WM_RVM_MIRROR_ORPHANED, id_, 0);
        return 0;

    case WM_CLOSE:
        if (notify_) PostMessageW(notify_, WM_RVM_MIRROR_CLOSED, id_, 0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Mirror::ShowContextMenu(POINT screenPt) {
    HMENU zoom = CreatePopupMenu();
    AppendMenuW(zoom, MF_STRING, kIdZoom50,  L"50%");
    AppendMenuW(zoom, MF_STRING, kIdZoom100, L"100%  (actual size)");
    AppendMenuW(zoom, MF_STRING, kIdZoom150, L"150%");
    AppendMenuW(zoom, MF_STRING, kIdZoom200, L"200%");

    HMENU opacity = CreatePopupMenu();
    for (const auto& lv : kOpacityLevels) {
        const bool on = std::fabs(state_.opacity - lv.value) < 0.01f;
        AppendMenuW(opacity, MF_STRING | (on ? MF_CHECKED : 0), lv.id, lv.label);
    }

    HMENU track = CreatePopupMenu();
    const bool anchored = state_.track == TrackMode::Anchored;
    AppendMenuW(track, MF_STRING | (anchored ? MF_CHECKED : 0), kIdTrackAnchored,
                L"Stay at fixed offset");
    AppendMenuW(track, MF_STRING | (anchored ? 0 : MF_CHECKED), kIdTrackProportional,
                L"Scale with the window");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kIdReselectRegion, L"Reselect region…");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom), L"Zoom");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"Opacity");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(track), L"When source resizes");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (state_.aspectLocked ? MF_CHECKED : 0), kIdAspectLock,
                L"Lock aspect ratio");
    AppendMenuW(menu, MF_STRING | (state_.clickThrough ? MF_CHECKED : 0), kIdClickThrough,
                L"Click-through");
    AppendMenuW(menu, MF_STRING, kIdHide, L"Hide");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdClose, L"Close");

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPt.x, screenPt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (!hwnd_) return;   // Retired while the menu was up.

    switch (cmd) {
    case kIdReselectRegion:    ReselectRegion(); break;
    case kIdAspectLock:        state_.aspectLocked = !state_.aspectLocked;
                               NotifyStateChanged(); break;
    case kIdClickThrough:      SetClickThrough(!state_.clickThrough); break;
    case kIdHide:              SetHidden(true); break;
    case kIdTrackAnchored:     SetTracking(TrackMode::Anchored); break;
    case kIdTrackProportional: SetTracking(TrackMode::Proportional); break;
    case kIdZoom50:  SetZoom(0.5f); break;
    case kIdZoom100: SetZoom(1.0f); break;
    case kIdZoom150: SetZoom(1.5f); break;
    case kIdZoom200: SetZoom(2.0f); break;
    case kIdClose:
        if (notify_) PostMessageW(notify_, WM_RVM_MIRROR_CLOSED, id_, 0);
        break;
    default:
        for (const auto& lv : kOpacityLevels) {
            if (cmd == lv.id) { SetOpacity(lv.value); break; }
        }
        break;
    }
}

}  // namespace rvm
