#include "manager.h"
#include "app.h"

namespace rvm {

namespace {

constexpr wchar_t kManagerClass[] = L"RvmManagerWindow";

// Layout metrics in unscaled pixels; S() applies DPI.
constexpr float kPad        = 14.0f;
constexpr float kHeaderH    = 52.0f;
constexpr float kCardH      = 156.0f;
constexpr float kCardGap    = 10.0f;
constexpr float kCardRadius = 10.0f;
constexpr float kRowH       = 24.0f;
constexpr float kToggleW    = 38.0f;
constexpr float kToggleH    = 21.0f;
constexpr float kButtonH    = 26.0f;
constexpr float kSliderHit  = 11.0f;   // Vertical grab slop around a track.
constexpr float kValueW     = 46.0f;
constexpr float kLabelW     = 52.0f;

constexpr float kScaleMin = 0.25f;
constexpr float kScaleMax = 3.0f;

// Same palette family as the picker and region overlays.
const D2D1_COLOR_F kBg      = { 0.067f, 0.075f, 0.094f, 1.0f };
const D2D1_COLOR_F kCard    = { 0.102f, 0.114f, 0.141f, 1.0f };
const D2D1_COLOR_F kCardHot = { 0.129f, 0.145f, 0.180f, 1.0f };
const D2D1_COLOR_F kStroke  = { 1.0f, 1.0f, 1.0f, 0.07f };
const D2D1_COLOR_F kText    = { 0.910f, 0.918f, 0.941f, 1.0f };
const D2D1_COLOR_F kDim     = { 0.541f, 0.565f, 0.627f, 1.0f };
const D2D1_COLOR_F kTrack   = { 1.0f, 1.0f, 1.0f, 0.11f };
const D2D1_COLOR_F kAccent  = { kAccentR, kAccentG, kAccentB, 1.0f };
const D2D1_COLOR_F kDanger  = { 0.937f, 0.325f, 0.314f, 1.0f };
const D2D1_COLOR_F kOff     = { 1.0f, 1.0f, 1.0f, 0.18f };

D2D1_RECT_F Expand(const D2D1_RECT_F& r, float dy) {
    return D2D1::RectF(r.left, r.top - dy, r.right, r.bottom + dy);
}

bool Contains(const D2D1_RECT_F& r, POINT p) {
    const float x = static_cast<float>(p.x), y = static_cast<float>(p.y);
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

std::wstring Percent(float value) {
    return std::to_wstring(static_cast<int>(std::lround(value * 100.0f))) + L"%";
}

// Linear, which keeps 100% easy to hit.
float ScaleToT(float scale) {
    return Clampf((scale - kScaleMin) / (kScaleMax - kScaleMin), 0.0f, 1.0f);
}
float TToScale(float t) {
    return kScaleMin + Clampf(t, 0.0f, 1.0f) * (kScaleMax - kScaleMin);
}

}  // namespace

bool ManagerWindow::IsOpen() const {
    return Hwnd() != nullptr && IsWindowVisible(Hwnd());
}

void ManagerWindow::Open(App* app) {
    app_ = app;

    if (Hwnd()) {
        ShowWindow(Hwnd(), IsIconic(Hwnd()) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(Hwnd());
        Refresh();
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    const RECT work = WorkAreaFor(monitor);

    dpiScale_ = DpiScaleFor(monitor);
    const int w = static_cast<int>(S(600.0f));
    const int h = static_cast<int>(S(560.0f));
    RECT bounds{ (work.left + work.right) / 2 - w / 2, (work.top + work.bottom) / 2 - h / 2, 0, 0 };
    bounds.right  = bounds.left + w;
    bounds.bottom = bounds.top + h;

    if (!CreateStyled(kManagerClass, kAppName, bounds, WS_OVERLAPPEDWINDOW,
                      WS_EX_NOREDIRECTIONBITMAP)) {
        return;
    }

    if (HICON bigIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON))) {
        SendMessageW(Hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    }
    if (HICON smallIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON))) {
        SendMessageW(Hwnd(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }

    dpiScale_ = static_cast<float>(GetDpiForWindow(Hwnd())) / 96.0f;
    ApplyTitleBarTheme(Hwnd());
    EnsureFonts();

    ShowWindow(Hwnd(), SW_SHOW);
    SetForegroundWindow(Hwnd());
    Render();
}

void ManagerWindow::Refresh() {
    if (!IsOpen()) return;
    Render();
}

void ManagerWindow::EnsureFonts() {
    auto& g = Gfx::Get();
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight,
                    winrt::com_ptr<IDWriteTextFormat>& out) {
        out = nullptr;
        if (FAILED(g.dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              S(size), L"en-us", out.put()))) {
            return;
        }
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    make(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFont_);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL,    bodyFont_);
    make(11.5f, DWRITE_FONT_WEIGHT_NORMAL,    smallFont_);
}

// Snapshot the model on the UI thread before the device lock is taken.
void ManagerWindow::PrepareDraw() {
    cards_.clear();
    if (!app_) return;

    streamingAvailable_ = app_->StreamingAvailable();
    streamingOn_     = app_->StreamingOn();
    streamClients_   = app_->StreamClients();
    for (size_t i = 0; i < app_->MirrorCount(); ++i) {
        const Mirror* m = app_->MirrorAt(i);
        if (!m) continue;

        CardView card;
        card.id           = m->Id();
        card.name         = m->DisplayName();
        card.enabled      = m->Enabled();
        card.clickThrough = m->ClickThrough();
        card.hidden       = m->Hidden();
        card.opacity      = m->Opacity();
        card.scale        = m->CurrentScale();

        const SIZE native = m->NativeSize();
        card.subtitle = m->SourceName();
        if (native.cx > 0) {
            if (!card.subtitle.empty()) card.subtitle += L"  ·  ";
            card.subtitle += std::to_wstring(native.cx) + L" × " + std::to_wstring(native.cy);
        }
        if (!card.enabled)        card.subtitle += L"  ·  off";
        else if (m->Orphaned())   card.subtitle += m->IsDesktop() ? L"  ·  waiting for the monitors"
                                                          : L"  ·  waiting for its window";
        else if (card.hidden)     card.subtitle += L"  ·  hidden, still capturing";
        cards_.push_back(std::move(card));
    }
    ClampScroll();
}

int ManagerWindow::CardIndex(uint32_t id) const {
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (cards_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

float ManagerWindow::ViewportHeight() const {
    return static_cast<float>(Height()) - S(kHeaderH);
}

float ManagerWindow::ContentHeight() const {
    if (cards_.empty()) return 0.0f;
    return cards_.size() * S(kCardH + kCardGap) + S(kPad);
}

void ManagerWindow::ClampScroll() {
    const float overflow = ContentHeight() - ViewportHeight();
    scroll_ = (overflow > 0.0f) ? Clampf(scroll_, 0.0f, overflow) : 0.0f;
}

D2D1_RECT_F ManagerWindow::NewMirrorButton() const {
    const float w = S(104.0f);
    const float right = static_cast<float>(Width()) - S(kPad);
    const float cy = S(kHeaderH) * 0.5f;
    return D2D1::RectF(right - w, cy - S(kButtonH) * 0.5f, right, cy + S(kButtonH) * 0.5f);
}

D2D1_RECT_F ManagerWindow::StreamingButton() const {
    const D2D1_RECT_F next = NewMirrorButton();
    const float w = S(104.0f);
    return D2D1::RectF(next.left - S(8.0f) - w, next.top, next.left - S(8.0f), next.bottom);
}

ManagerWindow::CardRects ManagerWindow::LayoutCard(int index) const {
    CardRects r{};
    const float top = S(kHeaderH) + S(kPad) + index * S(kCardH + kCardGap) - scroll_;
    const float left = S(kPad);
    const float right = static_cast<float>(Width()) - S(kPad);

    r.card = D2D1::RectF(left, top, right, top + S(kCardH));

    const float innerL = left + S(kPad);
    const float innerR = right - S(kPad);

    const float row1 = top + S(kPad) + S(kRowH) * 0.5f;
    r.toggle = D2D1::RectF(innerL, row1 - S(kToggleH) * 0.5f,
                           innerL + S(kToggleW), row1 + S(kToggleH) * 0.5f);

    const float btnW = S(74.0f);
    r.closeButton  = D2D1::RectF(innerR - btnW, row1 - S(kButtonH) * 0.5f,
                                 innerR, row1 + S(kButtonH) * 0.5f);
    r.regionButton = D2D1::RectF(r.closeButton.left - S(8.0f) - btnW,
                                 r.closeButton.top, r.closeButton.left - S(8.0f),
                                 r.closeButton.bottom);

    r.title = D2D1::RectF(r.toggle.right + S(10.0f), top + S(kPad) - S(2.0f),
                          r.regionButton.left - S(10.0f), top + S(kPad) + S(18.0f));
    r.subtitle = D2D1::RectF(r.title.left, r.title.bottom - S(2.0f),
                             r.title.right, r.title.bottom + S(16.0f));

    const float row2 = top + S(kPad) + S(kRowH) + S(20.0f);
    const float row3 = row2 + S(28.0f);
    const float trackL = innerL + S(kLabelW);
    const float trackR = innerR - S(kValueW);

    r.opacityTrack = D2D1::RectF(trackL, row2 - S(2.0f), trackR, row2 + S(2.0f));
    r.opacityValue = D2D1::RectF(trackR + S(8.0f), row2 - S(9.0f), innerR, row2 + S(9.0f));
    r.scaleTrack   = D2D1::RectF(trackL, row3 - S(2.0f), trackR, row3 + S(2.0f));
    r.scaleValue   = D2D1::RectF(trackR + S(8.0f), row3 - S(9.0f), innerR, row3 + S(9.0f));

    const float row4 = row3 + S(30.0f);
    const float modeW = S(108.0f);
    r.clickThroughButton = D2D1::RectF(innerL, row4 - S(kButtonH) * 0.5f,
                                       innerL + modeW, row4 + S(kButtonH) * 0.5f);
    r.hiddenButton = D2D1::RectF(r.clickThroughButton.right + S(8.0f), r.clickThroughButton.top,
                                 r.clickThroughButton.right + S(8.0f) + modeW,
                                 r.clickThroughButton.bottom);
    return r;
}

ManagerWindow::Hit ManagerWindow::HitTest(POINT pt) const {
    Hit hit;
    if (Contains(NewMirrorButton(), pt)) {
        hit.part = Part::NewMirror;
        return hit;
    }
    if (streamingAvailable_ && Contains(StreamingButton(), pt)) {
        hit.part = Part::Streaming;
        return hit;
    }
    if (static_cast<float>(pt.y) < S(kHeaderH)) return hit;

    for (size_t i = 0; i < cards_.size(); ++i) {
        const CardRects r = LayoutCard(static_cast<int>(i));
        if (!Contains(r.card, pt)) continue;
        hit.id = cards_[i].id;

        if (Contains(r.toggle, pt))                  hit.part = Part::Toggle;
        else if (Contains(r.regionButton, pt))       hit.part = Part::Region;
        else if (Contains(r.closeButton, pt))        hit.part = Part::Close;
        else if (Contains(r.clickThroughButton, pt)) hit.part = Part::ClickThrough;
        else if (Contains(r.hiddenButton, pt))       hit.part = Part::Hidden;
        else if (Contains(Expand(r.opacityTrack, S(kSliderHit)), pt)) hit.part = Part::Opacity;
        else if (Contains(Expand(r.scaleTrack, S(kSliderHit)), pt))   hit.part = Part::Scale;
        return hit;
    }
    return hit;
}

void ManagerWindow::EndSliderDrag() {
    dragging_ = false;   // First, so the WM_CAPTURECHANGED from releasing is ignored.
    ReleaseCapture();
    // Commit the dragged value to the settings file exactly once.
    if (app_) {
        if (Mirror* m = app_->FindMirror(active_.id)) {
            if (active_.part == Part::Opacity) m->SetOpacity(m->Opacity());
            else                               m->SetZoom(m->CurrentScale());
        }
    }
    active_ = Hit{};
    Render();
}

void ManagerWindow::ApplySliderDrag(POINT pt) {
    if (!app_) return;
    Mirror* m = app_->FindMirror(active_.id);
    const int index = CardIndex(active_.id);
    if (!m || index < 0) {   // Its mirror went away mid-drag.
        dragging_ = false;
        ReleaseCapture();
        active_ = Hit{};
        return;
    }

    const CardRects r = LayoutCard(index);
    const D2D1_RECT_F track = (active_.part == Part::Opacity) ? r.opacityTrack : r.scaleTrack;
    const float span = track.right - track.left;
    if (span <= 1.0f) return;

    const float t = Clampf((static_cast<float>(pt.x) - track.left) / span, 0.0f, 1.0f);
    if (active_.part == Part::Opacity) {
        m->SetOpacity(0.05f + t * 0.95f, /*persist=*/false);
    } else {
        float scale = TToScale(t);
        if (std::fabs(scale - 1.0f) < 0.04f) scale = 1.0f;   // Same detent as edge resize.
        m->SetZoom(scale, /*persist=*/false);
    }
    Render();
}

LRESULT ManagerWindow::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        const UINT w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0) {
            ResizeSurface(w, h);
            Render();
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = static_cast<LONG>(S(560.0f));
        mmi->ptMinTrackSize.y = static_cast<LONG>(S(260.0f));
        return 0;
    }

    case WM_SETTINGCHANGE:
        // Sent with "ImmersiveColorSet" when the Windows theme changes.
        if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
            ApplyTitleBarTheme(Hwnd());
        }
        break;

    case WM_DPICHANGED: {
        dpiScale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
        EnsureFonts();
        UpdateChipScale();
        const RECT* suggested = reinterpret_cast<RECT*>(lp);
        SetWindowPos(Hwnd(), nullptr, suggested->left, suggested->top,
                     RectW(*suggested), RectH(*suggested), SWP_NOZORDER | SWP_NOACTIVATE);
        Render();
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!mouseTracked_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, Hwnd(), 0 };
            TrackMouseEvent(&tme);
            mouseTracked_ = true;
        }
        if (dragging_) {
            ApplySliderDrag(pt);
            return 0;
        }
        const Hit hit = HitTest(pt);
        if (!(hit == hot_)) {
            hot_ = hit;
            Render();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        mouseTracked_ = false;
        hot_ = Hit{};
        Render();
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        active_ = HitTest(pt);
        if (active_.part == Part::Opacity || active_.part == Part::Scale) {
            dragging_ = true;
            SetCapture(Hwnd());
            ApplySliderDrag(pt);
        } else if (active_.part != Part::None) {
            Render();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        // Alt+Tab, a dialog or another app took the mouse mid-drag: keep
        // what the slider shows, as a release would.
        if (dragging_ && reinterpret_cast<HWND>(lp) != Hwnd()) EndSliderDrag();
        return 0;

    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (dragging_) {
            EndSliderDrag();
            return 0;
        }

        const Hit hit = HitTest(pt);
        const Hit pressed = active_;
        active_ = Hit{};

        if (hit == pressed && app_) {
            switch (hit.part) {
            case Part::NewMirror:
                app_->RequestNewMirror();
                break;
            case Part::Streaming:
                app_->ShowStreamSettings();
                break;
            case Part::ClickThrough:
                if (Mirror* m = app_->FindMirror(hit.id)) m->SetClickThrough(!m->ClickThrough());
                break;
            case Part::Hidden:
                if (Mirror* m = app_->FindMirror(hit.id)) m->SetHidden(!m->Hidden());
                break;
            case Part::Toggle:
                if (Mirror* m = app_->FindMirror(hit.id)) {
                    if (!app_->SetMirrorEnabled(*m, !m->Enabled())) {
                        MessageBoxW(Hwnd(),
                                    m->IsDesktop()
                                        ? L"The desktop could not be captured, so that mirror "
                                          L"cannot be switched on."
                                        : L"That mirror's source window is not open, so it "
                                          L"cannot be switched on yet.",
                                    kAppName, MB_OK | MB_ICONINFORMATION);
                    }
                }
                break;
            case Part::Region:
                if (Mirror* m = app_->FindMirror(hit.id)) m->ReselectRegion();
                break;
            case Part::Close:
                app_->CloseMirror(hit.id);
                break;
            default:
                break;
            }
        }
        if (!Hwnd()) return 0;   // Destroyed during a nested loop above.
        PrepareDraw();
        hot_ = HitTest(pt);
        Render();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const float overflow = ContentHeight() - ViewportHeight();
        if (overflow > 0.0f) {
            scroll_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * S(60.0f);
            ClampScroll();
            Render();
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(Hwnd(), SW_HIDE);
        return 0;

    case WM_CLOSE:
        ShowWindow(Hwnd(), SW_HIDE);   // A control panel; closing it never exits.
        return 0;

    default:
        break;
    }
    return D2DOverlay::OnMessage(msg, wp, lp);
}

void ManagerWindow::DrawLabel(ID2D1DeviceContext* dc, const std::wstring& text,
                              const D2D1_RECT_F& rect, IDWriteTextFormat* format,
                              const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align) {
    if (text.empty() || !format) return;
    format->SetTextAlignment(align);
    brush_->SetColor(color);
    dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                  brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void ManagerWindow::DrawSlider(ID2D1DeviceContext* dc, const D2D1_RECT_F& track,
                               float t, bool hot, bool showDetent) {
    const float radius = (track.bottom - track.top) * 0.5f;
    brush_->SetColor(kTrack);
    dc->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), brush_.get());

    const float knobX = track.left + (track.right - track.left) * Clampf(t, 0.0f, 1.0f);

    if (showDetent) {
        const float x = track.left + (track.right - track.left) * ScaleToT(1.0f);
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f));
        dc->FillRectangle(D2D1::RectF(x - S(0.75f), track.top - S(5.0f),
                                      x + S(0.75f), track.bottom + S(5.0f)), brush_.get());
    }

    brush_->SetColor(kAccent);
    dc->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(track.left, track.top, (std::max)(knobX, track.left),
                                      track.bottom), radius, radius), brush_.get());

    const float knobR = hot ? S(8.0f) : S(6.5f);
    const float cy = (track.top + track.bottom) * 0.5f;
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), knobR, knobR), brush_.get());
    brush_->SetColor(kAccent);
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), knobR, knobR), brush_.get(), S(2.0f));
}

void ManagerWindow::DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& r,
                               const std::wstring& label, bool hot, bool danger, bool on) {
    const float radius = S(6.0f);
    const D2D1_COLOR_F accent = danger ? kDanger : kAccent;

    if (on) {
        brush_->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, hot ? 1.0f : 0.88f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get());
        DrawLabel(dc, label, r, bodyFont_.get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                  DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    brush_->SetColor(hot ? D2D1::ColorF(accent.r, accent.g, accent.b, 0.20f)
                         : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get());

    brush_->SetColor(hot ? D2D1::ColorF(accent.r, accent.g, accent.b, 0.75f) : kStroke);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get(), S(1.0f));

    DrawLabel(dc, label, r, bodyFont_.get(), hot ? accent : kText, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void ManagerWindow::DrawCard(ID2D1DeviceContext* dc, const CardView& card, const CardRects& r) {
    const bool cardHot = (hot_.id == card.id && hot_.part != Part::None) ||
                         (hot_.id == card.id && card.id != 0);
    brush_->SetColor(cardHot ? kCardHot : kCard);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r.card, S(kCardRadius), S(kCardRadius)),
                             brush_.get());
    brush_->SetColor(kStroke);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r.card, S(kCardRadius), S(kCardRadius)),
                             brush_.get(), S(1.0f));

    const auto hot = [&](Part p) { return hot_.id == card.id && hot_.part == p; };

    const bool on = card.enabled;
    const float tr = (r.toggle.bottom - r.toggle.top) * 0.5f;
    brush_->SetColor(on ? kAccent : kOff);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r.toggle, tr, tr), brush_.get());
    if (hot(Part::Toggle)) {
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r.toggle, tr, tr), brush_.get(), S(1.5f));
    }
    const float knobR = tr - S(3.0f);
    const float knobX = on ? (r.toggle.right - tr) : (r.toggle.left + tr);
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, on ? 1.0f : 0.65f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, (r.toggle.top + r.toggle.bottom) * 0.5f),
                                  knobR, knobR), brush_.get());

    DrawLabel(dc, card.name, r.title, titleFont_.get(), on ? kText : kDim,
              DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawLabel(dc, card.subtitle, r.subtitle, smallFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_LEADING);

    DrawButton(dc, r.regionButton, L"Region…", hot(Part::Region), false);
    DrawButton(dc, r.closeButton, L"Remove", hot(Part::Close), true);
    DrawButton(dc, r.clickThroughButton, L"Click-through", hot(Part::ClickThrough), false,
               card.clickThrough);
    DrawButton(dc, r.hiddenButton, card.hidden ? L"Hidden" : L"Hide", hot(Part::Hidden),
               false, card.hidden);

    const D2D1_RECT_F opacityLabel =
        D2D1::RectF(r.card.left + S(kPad), r.opacityTrack.top - S(9.0f),
                    r.opacityTrack.left - S(6.0f), r.opacityTrack.bottom + S(9.0f));
    DrawLabel(dc, L"Opacity", opacityLabel, smallFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawSlider(dc, r.opacityTrack, (card.opacity - 0.05f) / 0.95f, hot(Part::Opacity), false);
    DrawLabel(dc, Percent(card.opacity), r.opacityValue, smallFont_.get(), kText,
              DWRITE_TEXT_ALIGNMENT_TRAILING);

    const D2D1_RECT_F scaleLabel =
        D2D1::RectF(r.card.left + S(kPad), r.scaleTrack.top - S(9.0f),
                    r.scaleTrack.left - S(6.0f), r.scaleTrack.bottom + S(9.0f));
    DrawLabel(dc, L"Size", scaleLabel, smallFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawSlider(dc, r.scaleTrack, ScaleToT(card.scale), hot(Part::Scale), true);
    DrawLabel(dc, Percent(card.scale), r.scaleValue, smallFont_.get(), kText,
              DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void ManagerWindow::OnDraw(ID2D1DeviceContext* dc) {
    if (!titleFont_) EnsureFonts();
    if (!titleFont_ || !bodyFont_ || !smallFont_) return;

    const float w = static_cast<float>(Width());
    const float h = static_cast<float>(Height());

    brush_->SetColor(kBg);
    dc->FillRectangle(D2D1::RectF(0, 0, w, h), brush_.get());

    dc->PushAxisAlignedClip(D2D1::RectF(0, S(kHeaderH), w, h), D2D1_ANTIALIAS_MODE_ALIASED);
    if (cards_.empty()) {
        DrawLabel(dc, L"No mirrors",
                  D2D1::RectF(0, S(kHeaderH), w, h), bodyFont_.get(), kDim,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        for (size_t i = 0; i < cards_.size(); ++i) {
            const CardRects r = LayoutCard(static_cast<int>(i));
            if (r.card.bottom < S(kHeaderH) || r.card.top > h) continue;
            DrawCard(dc, cards_[i], r);
        }
    }
    dc->PopAxisAlignedClip();

    // Header sits above the cards so they scroll under it.
    brush_->SetColor(kBg);
    dc->FillRectangle(D2D1::RectF(0, 0, w, S(kHeaderH)), brush_.get());
    brush_->SetColor(kStroke);
    dc->FillRectangle(D2D1::RectF(0, S(kHeaderH) - S(1.0f), w, S(kHeaderH)), brush_.get());

    const float headingRight = (streamingAvailable_ ? StreamingButton() : NewMirrorButton()).left;
    const D2D1_RECT_F heading = D2D1::RectF(S(kPad), 0, headingRight - S(10.0f), S(kHeaderH));
    DrawLabel(dc, Plural(static_cast<int>(cards_.size()), L"mirror", L"mirrors"), heading,
              titleFont_.get(), kText, DWRITE_TEXT_ALIGNMENT_LEADING);

    if (streamingAvailable_) {   // No button in a build without streaming.
        std::wstring streaming = L"Streaming";
        if (streamingOn_ && streamClients_ > 0) {
            streaming += L" (" + std::to_wstring(streamClients_) + L")";
        }
        DrawButton(dc, StreamingButton(), streaming, hot_.part == Part::Streaming, false,
                   streamingOn_);
    }
    DrawButton(dc, NewMirrorButton(), L"New mirror", hot_.part == Part::NewMirror, false);

    const float overflow = ContentHeight() - ViewportHeight();
    if (overflow > 0.0f) {
        const float viewport = ViewportHeight();
        const float thumb = (std::max)(viewport * (viewport / ContentHeight()), S(28.0f));
        const float travel = viewport - thumb;
        const float y = S(kHeaderH) + travel * Clampf(scroll_ / overflow, 0.0f, 1.0f);
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(w - S(6.0f), y, w - S(3.0f), y + thumb),
                              S(1.5f), S(1.5f)), brush_.get());
    }
}

}  // namespace rvm
