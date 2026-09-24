#include "capture.h"

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace rvm {

namespace {

constexpr int kBufferCount = 2;   // WGC only pushes on change, so latency wins.
constexpr auto kFormat = wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;

wgc::GraphicsCaptureItem CreateItemForWindow(HWND hwnd) {
    auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                        winrt::put_abi(item)))) {
        return nullptr;
    }
    return item;
}

wgc::GraphicsCaptureItem CreateItemForMonitor(HMONITOR monitor) {
    auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    if (FAILED(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                         winrt::put_abi(item)))) {
        return nullptr;
    }
    return item;
}

bool SessionHasProperty(const wchar_t* name) {
    try {
        return ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", name);
    } catch (...) {
        return false;
    }
}

}  // namespace

bool CaptureSupported() {
    try {
        return wgc::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

SIZE CaptureItemSize(HWND hwnd) {
    try {
        auto item = CreateItemForWindow(hwnd);
        if (!item) return SIZE{ 0, 0 };
        const auto s = item.Size();
        return SIZE{ s.Width, s.Height };
    } catch (...) {
        return SIZE{ 0, 0 };
    }
}

WindowCapture::~WindowCapture() {
    Stop();
}

SIZE WindowCapture::ContentSize() const {
    if (!shared_) return SIZE{ 0, 0 };
    std::lock_guard lock(shared_->mutex);
    return SIZE{ shared_->poolSize.Width, shared_->poolSize.Height };
}

bool WindowCapture::Start(HWND target, FrameCallback onFrame, std::function<void()> onClosed) {
    Stop();
    try {
        auto item = CreateItemForWindow(target);
        if (!item) return false;
        // The window's pixels, not the pointer hovering over them.
        return StartItem(item, /*cursor=*/false, std::move(onFrame), std::move(onClosed));
    } catch (...) {
        return false;
    }
}

bool WindowCapture::StartMonitor(HMONITOR monitor, FrameCallback onFrame) {
    Stop();
    try {
        auto item = CreateItemForMonitor(monitor);
        if (!item) return false;
        // Someone watching a screen needs to see where the pointer is.
        return StartItem(item, /*cursor=*/true, std::move(onFrame), nullptr);
    } catch (...) {
        return false;
    }
}

bool WindowCapture::StartItem(wgc::GraphicsCaptureItem item, bool cursor, FrameCallback onFrame,
                              std::function<void()> onClosed) {
    auto& g = Gfx::Get();

    try {
        item_ = std::move(item);

        shared_ = std::make_shared<Shared>();
        shared_->onFrame  = std::move(onFrame);
        shared_->onClosed = std::move(onClosed);
        shared_->poolSize = item_.Size();

        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g.winrtDevice, kFormat, kBufferCount, shared_->poolSize);
        session_ = pool_.CreateCaptureSession(item_);

        // No yellow capture frame; the pointer only where asked for.
        if (SessionHasProperty(L"IsCursorCaptureEnabled")) {
            try { session_.IsCursorCaptureEnabled(cursor); } catch (...) {}
        }
        if (SessionHasProperty(L"IsBorderRequired")) {
            try { session_.IsBorderRequired(false); } catch (...) {}
        }
        // Windows throttles capture to one frame per 16 ms unless told
        // otherwise, which holds every mirror and stream to about 60 fps
        // however fast the source and the display are. Frames still only
        // arrive when the window changes, so a still source costs nothing.
        if (SessionHasProperty(L"MinUpdateInterval")) {
            try {
                const auto before = session_.MinUpdateInterval();
                session_.MinUpdateInterval(std::chrono::milliseconds(1));
                RVM_LOG_SAMPLED(20, L"capture: minimum update interval %.2f ms -> %.2f ms",
                                before.count() / 10000.0, session_.MinUpdateInterval().count() / 10000.0);
            } catch (...) {}
        }

        std::weak_ptr<Shared> weak = shared_;

        frameArrived_ = pool_.FrameArrived(winrt::auto_revoke,
            [weak](wgc::Direct3D11CaptureFramePool const& pool, auto&&) {
                if (auto state = weak.lock()) OnFrame(*state, pool);
            });

        closed_ = item_.Closed(winrt::auto_revoke,
            [weak](auto&&, auto&&) {
                auto state = weak.lock();
                if (!state) return;
                std::lock_guard lock(state->mutex);
                if (state->onClosed) state->onClosed();
            });

        session_.StartCapture();
        return true;
    } catch (...) {
        Stop();
        return false;
    }
}

void WindowCapture::Stop() {
    frameArrived_.revoke();
    closed_.revoke();

    if (shared_) {
        // Blocks until any frame already inside the callback has finished.
        std::lock_guard lock(shared_->mutex);
        shared_->onFrame = nullptr;
        shared_->onClosed = nullptr;
    }
    shared_.reset();

    if (session_) {
        try { session_.Close(); } catch (...) {}
        session_ = nullptr;
    }
    if (pool_) {
        try { pool_.Close(); } catch (...) {}
        pool_ = nullptr;
    }
    item_ = nullptr;
}

void WindowCapture::OnFrame(Shared& state, wgc::Direct3D11CaptureFramePool const& pool) {
    bool needsResize = false;
    winrt::Windows::Graphics::SizeInt32 newSize{};

    try {
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        const auto contentSize = frame.ContentSize();

        {
            std::lock_guard lock(state.mutex);
            if (contentSize.Width != state.poolSize.Width ||
                contentSize.Height != state.poolSize.Height) {
                // Recorded only once the pool really is that size, so a failed
                // recreate is tried again on the next frame.
                needsResize = true;
                newSize = contentSize;
            }

            if (state.onFrame) {
                auto access = frame.Surface().as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> texture;
                HR(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
                state.onFrame(texture.get(),
                              static_cast<UINT>((std::max)(contentSize.Width, 1)),
                              static_cast<UINT>((std::max)(contentSize.Height, 1)));
            }
        }

        frame.Close();   // Must precede Recreate.
    } catch (const winrt::hresult_error& e) {
        Gfx::Get().CheckDevice(e.code());
        return;
    } catch (...) {
        return;
    }

    if (needsResize && newSize.Width > 0 && newSize.Height > 0) {
        try {
            pool.Recreate(Gfx::Get().winrtDevice, kFormat, kBufferCount, newSize);
            std::lock_guard lock(state.mutex);
            state.poolSize = newSize;
        } catch (const winrt::hresult_error& e) {
            Gfx::Get().CheckDevice(e.code());
        } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// Desktop

DesktopCapture::~DesktopCapture() {
    Stop();
}

RECT DesktopCapture::Bounds() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return RECT{ x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                 y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}

SIZE DesktopCapture::ContentSize() const {
    if (!shared_) return SIZE{ 0, 0 };
    std::lock_guard lock(shared_->mutex);
    return shared_->size;
}

bool DesktopCapture::Start(FrameCallback onFrame) {
    Stop();
    auto& g = Gfx::Get();

    const RECT bounds = Bounds();
    const int width = RectW(bounds), height = RectH(bounds);
    if (width <= 0 || height <= 0 || width > kMaxExtent || height > kMaxExtent) {
        Log(L"capture: desktop %dx%d cannot be captured as one texture", width, height);
        return false;
    }

    auto shared = std::make_shared<Shared>();
    shared->onFrame = std::move(onFrame);
    shared->size = SIZE{ width, height };

    // Opaque black to start with: the gaps between monitors stay that way,
    // and a monitor shows black until its first frame, never garbage.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width      = static_cast<UINT>(width);
    desc.Height     = static_cast<UINT>(height);
    desc.MipLevels  = 1;
    desc.ArraySize  = 1;
    desc.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    desc.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    {
        std::lock_guard device(g.deviceMutex);
        HRESULT hr = g.d3d->CreateTexture2D(&desc, nullptr, shared->composite.put());
        winrt::com_ptr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(hr)) hr = g.d3d->CreateRenderTargetView(shared->composite.get(), nullptr, rtv.put());
        if (FAILED(hr)) {
            g.CheckDevice(hr);
            Log(L"capture: desktop texture %dx%d failed 0x%08X", width, height,
                static_cast<unsigned>(hr));
            return false;
        }
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g.ctx->ClearRenderTargetView(rtv.get(), black);
    }

    std::vector<std::pair<HMONITOR, RECT>> monitors;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(monitor, &mi)) {
                reinterpret_cast<std::vector<std::pair<HMONITOR, RECT>>*>(lp)->emplace_back(
                    monitor, mi.rcMonitor);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    shared_ = shared;
    std::weak_ptr<Shared> weak = shared;
    for (const auto& [monitor, rect] : monitors) {
        const LONG atX = rect.left - bounds.left;
        const LONG atY = rect.top - bounds.top;
        if (atX < 0 || atY < 0 || atX >= width || atY >= height) continue;

        auto capture = std::make_unique<WindowCapture>();
        const bool started = capture->StartMonitor(monitor,
            [weak, atX, atY](ID3D11Texture2D* frame, UINT contentW, UINT contentH) {
                auto s = weak.lock();
                if (!s) return;
                // One monitor at a time: the copy and the hand-on are a unit,
                // so each frame passed on includes every copy before it.
                std::lock_guard lock(s->mutex);
                if (!s->onFrame || !s->composite) return;

                D3D11_TEXTURE2D_DESC fd{};
                frame->GetDesc(&fd);
                const UINT w = (std::min)({ contentW, fd.Width, static_cast<UINT>(s->size.cx - atX) });
                const UINT h = (std::min)({ contentH, fd.Height, static_cast<UINT>(s->size.cy - atY) });
                if (w == 0 || h == 0) return;
                {
                    auto& gfx = Gfx::Get();
                    std::lock_guard device(gfx.deviceMutex);
                    const D3D11_BOX box{ 0, 0, 0, w, h, 1 };
                    gfx.ctx->CopySubresourceRegion(s->composite.get(), 0, static_cast<UINT>(atX),
                                                   static_cast<UINT>(atY), 0, frame, 0, &box);
                }
                s->onFrame(s->composite.get(), static_cast<UINT>(s->size.cx),
                           static_cast<UINT>(s->size.cy));
            });
        if (started) {
            monitors_.push_back(std::move(capture));
        } else {
            Log(L"capture: monitor at %ld,%ld could not be captured", rect.left, rect.top);
        }
    }

    if (monitors_.empty()) {
        Stop();
        return false;
    }
    Log(L"capture: desktop %dx%d from %zu monitor(s)", width, height, monitors_.size());
    return true;
}

void DesktopCapture::Stop() {
    // Each monitor's Stop waits out a frame already in its callback, which
    // may be waiting for the shared lock; so that lock is not held here.
    for (auto& m : monitors_) m->Stop();
    monitors_.clear();
    if (shared_) {
        std::lock_guard lock(shared_->mutex);
        shared_->onFrame = nullptr;
        shared_->composite = nullptr;
    }
    shared_.reset();
}

}  // namespace rvm
