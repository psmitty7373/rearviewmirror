#include "duplication.h"

namespace rvm::login {

namespace {

// How long a monitor's thread waits for a change when no compose is due: only
// so that it notices being stopped.
constexpr UINT kIdleWaitMs = 250;
constexpr DWORD kRetryMs = 500;
// More changed areas than this between composes: the whole picture is copied.
constexpr size_t kMaxDamageRects = 32;

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

D3D11_BOX Box(const RECT& r) {
    return D3D11_BOX{ static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0,
                      static_cast<UINT>(r.right), static_cast<UINT>(r.bottom), 1 };
}

}  // namespace

DuplicationCapture::~DuplicationCapture() {
    Stop();
}

bool DuplicationCapture::Start(FrameCallback onFrame, SizeCallback onSize, WantedCallback wanted, UINT fps) {
    Stop();
    onFrame_ = std::move(onFrame);
    onSize_ = std::move(onSize);
    wanted_ = std::move(wanted);
    interval_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::microseconds(1'000'000 / (std::max)(fps, 1u)));
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
    layout_.clear();
    std::lock_guard lock(Gfx::Get().deviceMutex);
    pointer_ = nullptr;
    target_ = nullptr;
    d2d_ = nullptr;
    frame_ = nullptr;
    composite_ = nullptr;
    havePicture_ = false;
    stale_ = false;
}

void DuplicationCapture::Repush() {
    std::lock_guard lock(Gfx::Get().deviceMutex);
    if (!havePicture_ || !frame_ || !onFrame_) return;
    if (stale_) {
        Compose();
    } else {
        onFrame_(frame_.get());
    }
}

// Duplication works only for a thread on the input desktop.
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

bool DuplicationCapture::EnsureTextures(UINT width, UINT height) {
    if (composite_ && static_cast<UINT>(bounds_.right) == width && static_cast<UINT>(bounds_.bottom) == height) {
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
    bounds_ = RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    damage_.clear();
    damageAll_ = true;
    pointerDrawn_ = RECT{};
    layout_.clear();   // So that Attach clears them.
    return true;
}

// Monitors of other adapters, rotated ones, and gaps between monitors stay black.
void DuplicationCapture::ClearComposite() {
    auto& g = Gfx::Get();
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = g.d3d->CreateRenderTargetView(composite_.get(), nullptr, rtv.put());
    if (FAILED(hr)) {
        g.CheckDevice(hr);
        return;
    }
    const float black[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    g.ctx->ClearRenderTargetView(rtv.get(), black);
    damageAll_ = true;
    stale_ = true;
}

bool DuplicationCapture::Attach() {
    FollowInputDesktop();   // Attaching is tried even if this fails; the log says why it then fails.
    auto& g = Gfx::Get();

    // The whole virtual screen, which remote input addresses.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const UINT width = static_cast<UINT>(GetSystemMetrics(SM_CXVIRTUALSCREEN)) & ~1u;
    const UINT height = static_cast<UINT>(GetSystemMetrics(SM_CYVIRTUALSCREEN)) & ~1u;
    if (width == 0 || height == 0) return false;

    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(g.dxgi->GetAdapter(adapter.put()))) return false;
    outputs_.clear();
    std::vector<RECT> layout;
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
        // Its frames would come unrotated, sideways over its place.
        DXGI_OUTDUPL_DESC dd{};
        dup->GetDesc(&dd);
        if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            if (rotated_.insert(desc.DeviceName).second) {
                Log(L"login: %s is rotated, which is not supported; it shows black", desc.DeviceName);
            }
            continue;
        }
        rotated_.erase(desc.DeviceName);
        RECT placed = desc.DesktopCoordinates;
        OffsetRect(&placed, -vx, -vy);
        outputs_.push_back({ std::move(dup), POINT{ placed.left, placed.top } });
        layout.push_back(placed);
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
        // A monitor gone, moved or turned leaves no old picture behind.
        if (!std::equal(layout.begin(), layout.end(), layout_.begin(), layout_.end(),
                        [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; })) {
            ClearComposite();
            layout_ = std::move(layout);
        }
    }
    Log(L"login: duplicating %zu monitor(s), %ux%u", outputs_.size(), width, height);
    if (size_.cx != static_cast<LONG>(width) || size_.cy != static_cast<LONG>(height)) {
        size_ = SIZE{ static_cast<LONG>(width), static_cast<LONG>(height) };
        if (onSize_) onSize_(size_);
    }
    return true;
}

// What the frame changed, in the output's coordinates. False: unknown.
bool DuplicationCapture::ReadChanges(Output& out, UINT bytes) {
    if (bytes == 0) return false;
    out.moves.resize(bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT) + 1);
    out.dirty.resize(bytes / sizeof(RECT) + 1);
    UINT moveBytes = 0, dirtyBytes = 0;
    if (FAILED(out.dup->GetFrameMoveRects(static_cast<UINT>(out.moves.size() * sizeof(DXGI_OUTDUPL_MOVE_RECT)),
                                          out.moves.data(), &moveBytes)) ||
        FAILED(out.dup->GetFrameDirtyRects(static_cast<UINT>(out.dirty.size() * sizeof(RECT)), out.dirty.data(),
                                           &dirtyBytes))) {
        return false;
    }
    out.moves.resize(moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT));
    out.dirty.resize(dirtyBytes / sizeof(RECT));
    return true;
}

// The frame is the output's whole new image, so where something moved to is
// copied from it just like a dirty area.
void DuplicationCapture::CopyChanges(Output& out, ID3D11Texture2D* texture, bool whole) {
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    const RECT within{ 0, 0, (std::min)(static_cast<LONG>(td.Width), bounds_.right - out.at.x),
                       (std::min)(static_cast<LONG>(td.Height), bounds_.bottom - out.at.y) };
    if (out.at.x < 0 || out.at.y < 0 || IsRectEmpty(&within)) return;

    auto& g = Gfx::Get();
    const auto copy = [&](const RECT& changed) {
        RECT r{};
        if (!IntersectRect(&r, &changed, &within)) return;
        const D3D11_BOX box = Box(r);
        g.ctx->CopySubresourceRegion(composite_.get(), 0, static_cast<UINT>(out.at.x + r.left),
                                     static_cast<UINT>(out.at.y + r.top), 0, texture, 0, &box);
        OffsetRect(&r, out.at.x, out.at.y);
        Damage(r);
    };
    if (whole) {
        copy(within);
    } else {
        for (const auto& move : out.moves) copy(move.DestinationRect);
        for (const RECT& dirty : out.dirty) copy(dirty);
    }
    out.whole = false;
    havePicture_ = true;
}

void DuplicationCapture::Damage(const RECT& r) {
    stale_ = true;
    if (damageAll_) return;
    if (damage_.size() == kMaxDamageRects) {
        damageAll_ = true;
        damage_.clear();
        return;
    }
    damage_.push_back(r);
}

void DuplicationCapture::TakePointer(const Output& out, size_t index, const DXGI_OUTDUPL_FRAME_INFO& info,
                                     const std::vector<uint32_t>& shape, UINT shapeW, UINT shapeH) {
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        if (info.PointerPosition.Visible) {
            const POINT at{ out.at.x + info.PointerPosition.Position.x, out.at.y + info.PointerPosition.Position.y };
            if (!pointerVisible_ || at.x != pointerAt_.x || at.y != pointerAt_.y) stale_ = true;
            pointerVisible_ = true;
            pointerOwner_ = index;
            pointerAt_ = at;
        } else if (pointerOwner_ == index && pointerVisible_) {
            pointerVisible_ = false;   // Left this monitor; another one reports it if it is shown.
            stale_ = true;
        }
    }
    if (shape.empty() || !d2d_) return;
    pointer_ = nullptr;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    d2d_->CreateBitmap(D2D1::SizeU(shapeW, shapeH), shape.data(), shapeW * 4, &props, pointer_.put());
    stale_ = true;
}

// The screen, the pointer over it, and out. Only what changed is copied into
// frame_, including the area the pointer covered last time.
void DuplicationCapture::Compose() {
    auto& g = Gfx::Get();
    const auto restore = [&](const RECT& r) {
        if (IsRectEmpty(&r)) return;
        const D3D11_BOX box = Box(r);
        g.ctx->CopySubresourceRegion(frame_.get(), 0, static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0,
                                     composite_.get(), 0, &box);
    };
    if (damageAll_) {
        g.ctx->CopyResource(frame_.get(), composite_.get());
    } else {
        for (const RECT& r : damage_) restore(r);
        restore(pointerDrawn_);
    }
    damage_.clear();
    damageAll_ = false;
    stale_ = false;
    pointerDrawn_ = RECT{};

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
        const RECT drawn{ pointerAt_.x, pointerAt_.y, pointerAt_.x + static_cast<LONG>(size.width),
                          pointerAt_.y + static_cast<LONG>(size.height) };
        IntersectRect(&pointerDrawn_, &drawn, &bounds_);
    }
    const auto now = std::chrono::steady_clock::now();
    nextCompose_ = (std::max)(nextCompose_, now - interval_) + interval_;
    if (onFrame_) onFrame_(frame_.get());
}

// Composes if wanted and the frame rate allows. Returns how long the caller
// may wait for a change: a put-off compose only until it is due.
UINT DuplicationCapture::ComposeIfDue() {
    const bool wanted = wanted_ && wanted_();
    std::lock_guard lock(Gfx::Get().deviceMutex);
    if (!wanted || !stale_ || !havePicture_ || !frame_) return kIdleWaitMs;
    const auto now = std::chrono::steady_clock::now();
    if (now < nextCompose_) {
        return static_cast<UINT>(std::chrono::ceil<std::chrono::milliseconds>(nextCompose_ - now).count());
    }
    Compose();
    return kIdleWaitMs;
}

void DuplicationCapture::RunOutput(size_t index) {
    // A new thread starts on the process's desktop, not the input desktop.
    if (index > 0 && desktop_) SetThreadDesktop(desktop_);
    Output& out = outputs_[index];
    auto& g = Gfx::Get();
    UINT waitMs = kIdleWaitMs;
    std::vector<uint32_t> shape;
    while (running_ && !lost_) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        const HRESULT hr = out.dup->AcquireNextFrame(waitMs, &info, resource.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            waitMs = ComposeIfDue();
            continue;
        }
        if (FAILED(hr)) {
            // DXGI_ERROR_ACCESS_LOST: the desktop switched or the mode changed.
            Log(L"login: duplication ended (0x%08X); attaching again", static_cast<unsigned>(hr));
            g.CheckDevice(hr);
            lost_ = true;
            break;
        }

        // Everything that needs no device lock first.
        const bool presented = info.LastPresentTime.QuadPart != 0 && resource;
        const bool whole = presented && (out.whole || !ReadChanges(out, info.TotalMetadataBufferSize));
        UINT shapeW = 0, shapeH = 0;
        shape.clear();
        if (info.PointerShapeBufferSize > 0) {
            out.shape.resize(info.PointerShapeBufferSize);
            UINT used = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
            if (SUCCEEDED(out.dup->GetFramePointerShape(static_cast<UINT>(out.shape.size()), out.shape.data(),
                                                        &used, &si)) &&
                si.Width != 0 && si.Height != 0) {
                shape = PointerPixels(si, out.shape.data(), shapeW, shapeH);
            }
        }
        {
            std::lock_guard lock(g.deviceMutex);
            if (presented) {
                if (auto texture = resource.try_as<ID3D11Texture2D>()) CopyChanges(out, texture.get(), whole);
            }
            if (info.LastMouseUpdateTime.QuadPart != 0 || !shape.empty()) {
                TakePointer(out, index, info, shape, shapeW, shapeH);
            }
        }
        out.dup->ReleaseFrame();
        waitMs = ComposeIfDue();
    }
}

void DuplicationCapture::Loop() {
    while (running_) {
        if (!Attach()) {
            for (DWORD waited = 0; waited < kRetryMs && running_; waited += 50) Sleep(50);
            continue;
        }
        // The first monitor on this thread: one monitor needs no other.
        lost_ = false;
        std::vector<std::thread> others;
        for (size_t i = 1; i < outputs_.size(); ++i) others.emplace_back([this, i] { RunOutput(i); });
        RunOutput(0);
        for (auto& t : others) t.join();
        outputs_.clear();
    }
}

}  // namespace rvm::login
