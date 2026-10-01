#include "duplication.h"

namespace rvm::login {

namespace {

// One monitor: wait on it, so a still screen costs nothing. Several: poll
// each briefly in turn.
constexpr UINT kSingleOutputWaitMs = 100;
constexpr UINT kMultiOutputWaitMs  = 8;
constexpr DWORD kRetryMs = 500;

// The pointer shape as premultiplied BGRA. Duplication reports three kinds;
// the monochrome and masked ones can invert what is under them, which a
// drawn bitmap cannot, so inverting pixels are drawn black.
std::vector<uint32_t> PointerPixels(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si, const uint8_t* shape,
                                    UINT& width, UINT& height) {
    width = si.Width;
    height = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? si.Height / 2 : si.Height;
    std::vector<uint32_t> px(static_cast<size_t>(width) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            uint32_t out = 0;
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
                const bool andBit = (shape[y * si.Pitch + x / 8] & bit) != 0;
                const bool xorBit = (shape[(y + height) * si.Pitch + x / 8] & bit) != 0;
                if (!andBit) out = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
                else if (xorBit) out = 0xFF000000u;   // Inverts the screen.
            } else {
                const uint32_t c = reinterpret_cast<const uint32_t*>(shape + y * si.Pitch)[x];
                const uint32_t a = c >> 24;
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    const auto premultiply = [a](uint32_t v) { return (v * a + 127) / 255; };
                    out = (a << 24) | (premultiply((c >> 16) & 0xFF) << 16) |
                          (premultiply((c >> 8) & 0xFF) << 8) | premultiply(c & 0xFF);
                } else if (a == 0) {
                    out = 0xFF000000u | (c & 0xFFFFFFu);   // Masked colour: replaces the screen.
                } else if (c & 0xFFFFFFu) {
                    out = 0xFF000000u;                     // XORs it: inverts, near enough.
                }
            }
            px[static_cast<size_t>(y) * width + x] = out;
        }
    }
    return px;
}

}  // namespace

DuplicationCapture::~DuplicationCapture() {
    Stop();
}

bool DuplicationCapture::Start(FrameCallback onFrame, SizeCallback onSize) {
    Stop();
    onFrame_ = std::move(onFrame);
    onSize_ = std::move(onSize);
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void DuplicationCapture::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    // The thread no longer uses its desktop, so the handle can go.
    if (desktop_) {
        CloseDesktop(desktop_);
        desktop_ = nullptr;
    }
    std::lock_guard lock(Gfx::Get().deviceMutex);
    pointer_ = nullptr;
    target_ = nullptr;
    d2d_ = nullptr;
    frame_ = nullptr;
    composite_ = nullptr;
    havePicture_ = false;
}

void DuplicationCapture::Repush() {
    std::lock_guard lock(Gfx::Get().deviceMutex);
    if (havePicture_ && frame_ && onFrame_) onFrame_(frame_.get());
}

// Duplication works only for a thread on the input desktop, so the thread
// moves to it before every attach: the sign-in screen, the desktop of whoever
// signed in, a UAC prompt.
bool DuplicationCapture::FollowInputDesktop() {
    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!input) {
        RVM_LOG_SAMPLED(50, L"login: cannot open the input desktop (%lu)", GetLastError());
        return false;
    }
    wchar_t name[64]{};
    DWORD needed = 0;
    GetUserObjectInformationW(input, UOI_NAME, name, sizeof(name), &needed);
    if (!SetThreadDesktop(input)) {
        RVM_LOG_SAMPLED(50, L"login: cannot move to the '%s' desktop (%lu)", name, GetLastError());
        CloseDesktop(input);
        return false;
    }
    if (desktop_) CloseDesktop(desktop_);   // No longer this thread's desktop.
    desktop_ = input;
    if (desktopName_ != name) {
        Log(L"login: on the '%s' desktop", name);
        desktopName_ = name;
    }
    return true;
}

// Under Gfx::deviceMutex.
bool DuplicationCapture::EnsureTextures(UINT width, UINT height) {
    if (composite_ && static_cast<UINT>(size_.cx) == width && static_cast<UINT>(size_.cy) == height) {
        return true;
    }
    auto& g = Gfx::Get();
    pointer_ = nullptr;
    target_ = nullptr;
    frame_ = nullptr;
    composite_ = nullptr;
    havePicture_ = false;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = width; d.Height = height; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc = { 1, 0 };
    d.Usage = D3D11_USAGE_DEFAULT;
    // Render target: for clearing, for Direct2D to draw the pointer, and for
    // the video processor to read it directly. Shader resource: for the packer.
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g.d3d->CreateTexture2D(&d, nullptr, composite_.put());
    if (SUCCEEDED(hr)) hr = g.d3d->CreateTexture2D(&d, nullptr, frame_.put());
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    if (SUCCEEDED(hr)) hr = g.d3d->CreateRenderTargetView(composite_.get(), nullptr, rtv.put());
    if (SUCCEEDED(hr) && !d2d_) {
        hr = g.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put());
    }
    if (SUCCEEDED(hr)) {
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = d2d_->CreateBitmapFromDxgiSurface(frame_.as<IDXGISurface>().get(), &props, target_.put());
    }
    if (FAILED(hr)) {
        Log(L"login: textures %ux%u failed (0x%08X)", width, height, static_cast<unsigned>(hr));
        g.CheckDevice(hr);
        target_ = nullptr;
        frame_ = nullptr;
        composite_ = nullptr;
        return false;
    }
    // Monitors of other adapters, and gaps between monitors, stay black.
    const float black[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    g.ctx->ClearRenderTargetView(rtv.get(), black);
    return true;
}

bool DuplicationCapture::Attach() {
    FollowInputDesktop();   // Attaching is tried even if this fails; the log says why it then fails.
    auto& g = Gfx::Get();

    // The picture is the whole virtual screen, as the app's desktop mirrors
    // are: remote input addresses it as such.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const UINT width = static_cast<UINT>(GetSystemMetrics(SM_CXVIRTUALSCREEN)) & ~1u;
    const UINT height = static_cast<UINT>(GetSystemMetrics(SM_CYVIRTUALSCREEN)) & ~1u;
    if (width == 0 || height == 0) return false;

    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(g.dxgi->GetAdapter(adapter.put()))) return false;
    outputs_.clear();
    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(i, output.put())) || !output) break;   // NOT_FOUND ends the list.
        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        if (!desc.AttachedToDesktop) continue;

        winrt::com_ptr<IDXGIOutputDuplication> dup;
        HRESULT hr = E_NOINTERFACE;
        if (auto output5 = output.try_as<IDXGIOutput5>()) {
            const DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            hr = output5->DuplicateOutput1(g.d3d.get(), 0, ARRAYSIZE(formats), formats, dup.put());
        }
        if (FAILED(hr)) {
            if (auto output1 = output.try_as<IDXGIOutput1>()) hr = output1->DuplicateOutput(g.d3d.get(), dup.put());
        }
        if (FAILED(hr)) {
            // E_ACCESSDENIED: a secure desktop, without SYSTEM. DXGI_ERROR_UNSUPPORTED:
            // a remote session or a driver without duplication.
            RVM_LOG_SAMPLED(50, L"login: cannot duplicate %s (0x%08X)", desc.DeviceName,
                            static_cast<unsigned>(hr));
            continue;
        }
        outputs_.push_back({ std::move(dup), POINT{ desc.DesktopCoordinates.left - vx,
                                                    desc.DesktopCoordinates.top - vy } });
    }
    if (outputs_.empty()) {
        RVM_LOG_SAMPLED(50, L"login: no monitor could be duplicated; trying again");
        return false;
    }

    {
        std::lock_guard lock(g.deviceMutex);
        if (!EnsureTextures(width, height)) {
            outputs_.clear();
            return false;
        }
    }
    Log(L"login: duplicating %zu monitor(s), %ux%u", outputs_.size(), width, height);
    if (size_.cx != static_cast<LONG>(width) || size_.cy != static_cast<LONG>(height)) {
        size_ = SIZE{ static_cast<LONG>(width), static_cast<LONG>(height) };
        if (onSize_) onSize_(size_);
    }
    return true;
}

void DuplicationCapture::Detach() {
    outputs_.clear();
}

// Under Gfx::deviceMutex, between AcquireNextFrame and ReleaseFrame.
void DuplicationCapture::TakePointer(const Output& out, size_t index, const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        if (info.PointerPosition.Visible) {
            pointerVisible_ = true;
            pointerOwner_ = index;
            pointerAt_ = POINT{ out.at.x + info.PointerPosition.Position.x,
                                out.at.y + info.PointerPosition.Position.y };
        } else if (pointerOwner_ == index) {
            pointerVisible_ = false;   // Left this monitor; another one reports it if it is shown.
        }
    }
    if (info.PointerShapeBufferSize == 0) return;

    shapeBuffer_.resize(info.PointerShapeBufferSize);
    UINT used = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    if (FAILED(out.dup->GetFramePointerShape(static_cast<UINT>(shapeBuffer_.size()), shapeBuffer_.data(),
                                             &used, &si)) ||
        si.Width == 0 || si.Height == 0) {
        return;
    }
    UINT w = 0, h = 0;
    const auto px = PointerPixels(si, shapeBuffer_.data(), w, h);
    pointer_ = nullptr;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (d2d_) d2d_->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &props, pointer_.put());
}

// Under Gfx::deviceMutex: the screen, the pointer over it, and out.
void DuplicationCapture::Compose() {
    auto& g = Gfx::Get();
    g.ctx->CopyResource(frame_.get(), composite_.get());
    if (pointerVisible_ && pointer_ && d2d_ && target_) {
        const D2D1_SIZE_U size = pointer_->GetPixelSize();
        const float x = static_cast<float>(pointerAt_.x), y = static_cast<float>(pointerAt_.y);
        d2d_->SetTarget(target_.get());
        d2d_->BeginDraw();
        d2d_->DrawBitmap(pointer_.get(), D2D1::RectF(x, y, x + size.width, y + size.height), 1.0f,
                         D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        const HRESULT hr = d2d_->EndDraw();
        d2d_->SetTarget(nullptr);
        if (FAILED(hr)) g.CheckDevice(hr);
    }
    if (onFrame_) onFrame_(frame_.get());
}

void DuplicationCapture::Loop() {
    while (running_) {
        if (outputs_.empty() && !Attach()) {
            for (DWORD waited = 0; waited < kRetryMs && running_; waited += 50) Sleep(50);
            continue;
        }
        const UINT waitMs = outputs_.size() == 1 ? kSingleOutputWaitMs : kMultiOutputWaitMs;
        bool changed = false, lost = false;
        auto& g = Gfx::Get();
        for (size_t i = 0; i < outputs_.size() && running_; ++i) {
            Output& out = outputs_[i];
            DXGI_OUTDUPL_FRAME_INFO info{};
            winrt::com_ptr<IDXGIResource> resource;
            const HRESULT hr = out.dup->AcquireNextFrame(waitMs, &info, resource.put());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) {
                // DXGI_ERROR_ACCESS_LOST: the desktop switched or the mode changed.
                Log(L"login: duplication ended (0x%08X); attaching again", static_cast<unsigned>(hr));
                g.CheckDevice(hr);
                lost = true;
                break;
            }
            {
                std::lock_guard lock(g.deviceMutex);
                if (info.LastPresentTime.QuadPart != 0 && resource) {
                    if (auto texture = resource.try_as<ID3D11Texture2D>()) {
                        D3D11_TEXTURE2D_DESC td{};
                        texture->GetDesc(&td);
                        const LONG right = (std::min)(static_cast<LONG>(td.Width), size_.cx - out.at.x);
                        const LONG bottom = (std::min)(static_cast<LONG>(td.Height), size_.cy - out.at.y);
                        if (out.at.x >= 0 && out.at.y >= 0 && right > 0 && bottom > 0) {
                            const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(right), static_cast<UINT>(bottom), 1 };
                            g.ctx->CopySubresourceRegion(composite_.get(), 0, static_cast<UINT>(out.at.x),
                                                         static_cast<UINT>(out.at.y), 0, texture.get(), 0, &box);
                            havePicture_ = true;
                            changed = true;
                        }
                    }
                }
                if (info.LastMouseUpdateTime.QuadPart != 0 || info.PointerShapeBufferSize > 0) {
                    TakePointer(out, i, info);
                    changed = true;
                }
            }
            out.dup->ReleaseFrame();
        }
        if (lost) {
            Detach();
            continue;
        }
        if (changed) {
            std::lock_guard lock(g.deviceMutex);
            if (havePicture_) Compose();
        }
    }
    Detach();
}

}  // namespace rvm::login
