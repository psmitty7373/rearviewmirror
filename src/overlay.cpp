#include "overlay.h"

namespace rvm {

namespace {
constexpr float kFontSize = 13.0f;
constexpr float kChipPadX = 10.0f;
constexpr float kChipPadY = 6.0f;
}  // namespace

D2DOverlay::~D2DOverlay() {
    Destroy();
}

LRESULT D2DOverlay::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        Render();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

bool D2DOverlay::Create(const wchar_t* className, RECT bounds, bool clickThrough) {
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    if (clickThrough) ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    return CreateStyled(className, L"", bounds, WS_POPUP, ex);
}

bool D2DOverlay::CreateStyled(const wchar_t* className, const wchar_t* title, RECT bounds,
                              DWORD style, DWORD exStyle, UINT classStyle) {
    auto& g = Gfx::Get();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = classStyle;
    wc.lpfnWndProc   = &WndProcThunk<D2DOverlay, &D2DOverlay::OnMessage>;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);  // Duplicate registration is fine; the class persists.

    bounds_ = bounds;
    hwnd_ = CreateWindowExW(exStyle, className, title, style,
                            bounds.left, bounds.top, RectW(bounds), RectH(bounds),
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    // A framed window's client area is smaller than the window rect.
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!comp_.Create(hwnd_, static_cast<UINT>((std::max)(RectW(client), 1)),
                      static_cast<UINT>((std::max)(RectH(client), 1))) ||
        FAILED(g.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc_.put())) ||
        !UpdateChipScale()) {
        Destroy();
        return false;
    }
    return true;
}

// Chips (hints, size read-outs, name tags) follow the window's DPI like
// everything else drawn in it.
bool D2DOverlay::UpdateChipScale() {
    chipScale_ = hwnd_ ? static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f : 1.0f;
    font_ = nullptr;
    if (FAILED(Gfx::Get().dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                                                  DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, kFontSize * chipScale_,
                                                  L"en-us", font_.put()))) {
        return false;
    }
    font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

void D2DOverlay::DropTarget() {
    if (dc_) dc_->SetTarget(nullptr);
    targetBitmap_ = nullptr;
    brush_ = nullptr;
}

void D2DOverlay::ResizeSurface(UINT width, UINT height) {
    if (!comp_.Valid()) return;
    if (width == comp_.Width() && height == comp_.Height()) return;
    std::lock_guard<std::mutex> deviceLock(Gfx::Get().deviceMutex);
    DropTarget();
    comp_.Resize(width, height);
}

void D2DOverlay::Destroy() {
    DropTarget();
    dc_ = nullptr;
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
}

void D2DOverlay::SetBounds(const RECT& bounds) {
    if (!hwnd_) return;
    const bool resized = RectW(bounds) != RectW(bounds_) || RectH(bounds) != RectH(bounds_);
    bounds_ = bounds;
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds.left, bounds.top, RectW(bounds), RectH(bounds),
                 SWP_NOACTIVATE);
    if (resized) {
        ResizeSurface(static_cast<UINT>(RectW(bounds)), static_cast<UINT>(RectH(bounds)));
    }
}

void D2DOverlay::Show() {
    if (hwnd_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void D2DOverlay::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool D2DOverlay::EnsureTarget() {
    if (targetBitmap_) return true;
    if (!comp_.Valid() || !dc_) return false;

    winrt::com_ptr<IDXGISurface> surface;
    if (FAILED(comp_.Swap()->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()))) return false;

    auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.get(), &props, targetBitmap_.put()))) return false;
    dc_->SetTarget(targetBitmap_.get());

    if (!brush_ && FAILED(dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush_.put()))) {
        DropTarget();
        return false;
    }
    return true;
}

void D2DOverlay::Invalidate() {
    if (!hwnd_ || invalidated_) return;
    invalidated_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DOverlay::Render() {
    if (!hwnd_) return;
    // Any paint pending, ours or the system's, is answered by this one.
    ValidateRect(hwnd_, nullptr);
    invalidated_ = false;
    // Nothing shows while minimized; restoring sends WM_SIZE, which draws.
    if (!dc_ || !comp_.Valid() || IsIconic(hwnd_)) return;

    ++frame_;
    PrepareDraw();
    {
        // No mirror frame may land between BeginDraw and EndDraw.
        std::lock_guard<std::mutex> deviceLock(Gfx::Get().deviceMutex);

        // Retry once so a rebuilt target is not presented blank.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!EnsureTarget()) return;

            dc_->BeginDraw();
            OnDraw(dc_.get());
            const HRESULT hr = dc_->EndDraw();

            if (hr == D2DERR_RECREATE_TARGET) {
                // Either the target alone is stale, or the whole device went.
                Gfx::Get().CheckDevice(hr);
                DropTarget();
                continue;
            }
            Gfx::Get().CheckDevice(hr);
            break;
        }
    }
    // Waits for the display once a frame is already queued: drawing faster
    // than it refreshes would only be thrown away.
    comp_.Present(1);

    if ((frame_ & 63) == 0) {
        std::erase_if(texts_, [&](const auto& t) { return frame_ - t.second.usedFrame > 64; });
    }
}

const D2DOverlay::CachedText* D2DOverlay::Layout(std::wstring_view text, IDWriteTextFormat* format,
                                                 float width, float height,
                                                 DWRITE_TEXT_ALIGNMENT align) {
    if (text.empty() || !format || width <= 0.0f || height <= 0.0f) return nullptr;
    const int w = static_cast<int>(std::lround(width * 16.0f));
    const int h = static_cast<int>(std::lround(height * 16.0f));
    const int a = static_cast<int>(align);

    size_t key = std::hash<std::wstring_view>{}(text);
    for (const size_t part : { reinterpret_cast<size_t>(format), static_cast<size_t>(w),
                               static_cast<size_t>(h), static_cast<size_t>(a) }) {
        key ^= part + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
    }

    CachedText& t = texts_[key];
    if (!t.layout || t.text != text || t.format.get() != format || t.w != w || t.h != h ||
        t.align != a) {
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(Gfx::Get().dwrite->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                                       format, width, height, layout.put()))) {
            texts_.erase(key);
            return nullptr;
        }
        layout->SetTextAlignment(align);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        t.text.assign(text);
        t.format.copy_from(format);
        t.w = w;
        t.h = h;
        t.align = a;
        t.layout = std::move(layout);
        t.size = { m.widthIncludingTrailingWhitespace, m.height };
    }
    t.usedFrame = frame_;
    return &t;
}

void D2DOverlay::DrawLabel(ID2D1DeviceContext* dc, std::wstring_view text, const D2D1_RECT_F& rect,
                           IDWriteTextFormat* format, const D2D1_COLOR_F& color,
                           DWRITE_TEXT_ALIGNMENT align) {
    const CachedText* t = Layout(text, format, rect.right - rect.left, rect.bottom - rect.top, align);
    if (!t || !brush_) return;
    brush_->SetColor(color);
    dc->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), t->layout.get(), brush_.get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

D2D1_SIZE_F D2DOverlay::DrawChip(ID2D1DeviceContext* dc, std::wstring_view text,
                                 float x, float y, int alignX, int alignY, float bgAlpha) {
    const CachedText* t = Layout(text, font_.get(), 4096.0f, 200.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!t || !brush_) return { 0.0f, 0.0f };

    const D2D1_SIZE_F ts = t->size;
    const float padX = kChipPadX * chipScale_, padY = kChipPadY * chipScale_;
    const float w = ts.width + padX * 2.0f;
    const float h = ts.height + padY * 2.0f;

    if (alignX == 1) x -= w * 0.5f;
    else if (alignX == 2) x -= w;
    if (alignY == 1) y -= h * 0.5f;
    else if (alignY == 2) y -= h;

    const D2D1_ROUNDED_RECT rr{ { x, y, x + w, y + h }, h * 0.5f, h * 0.5f };

    brush_->SetColor(D2D1::ColorF(0.055f, 0.063f, 0.086f, bgAlpha));
    dc->FillRoundedRectangle(rr, brush_.get());

    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
    dc->DrawRoundedRectangle(rr, brush_.get(), chipScale_);

    brush_->SetColor(D2D1::ColorF(0.94f, 0.95f, 0.97f, 1.0f));
    dc->DrawTextLayout(D2D1::Point2F(x + padX, y + padY), t->layout.get(), brush_.get());

    return { w, h };
}

}  // namespace rvm
