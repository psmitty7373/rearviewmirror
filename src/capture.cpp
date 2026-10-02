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
    return SIZE{ shared_->poolW.load(), shared_->poolH.load() };
}

bool WindowCapture::Start(HWND target, FrameCallback onFrame, AfterFrame afterFrame,
                          std::function<void()> onClosed) {
    Stop();
    try {
        auto item = CreateItemForWindow(target);
        if (!item) return false;
        // The window's pixels, not the pointer hovering over them.
        return StartItem(item, /*cursor=*/false, std::move(onFrame), std::move(afterFrame),
                         std::move(onClosed));
    } catch (...) {
        return false;
    }
}

bool WindowCapture::StartMonitor(HMONITOR monitor, FrameCallback onFrame, AfterFrame afterFrame) {
    Stop();
    try {
        auto item = CreateItemForMonitor(monitor);
        if (!item) return false;
        // Someone watching a screen needs to see where the pointer is.
        return StartItem(item, /*cursor=*/true, std::move(onFrame), std::move(afterFrame), nullptr);
    } catch (...) {
        return false;
    }
}

// Built in locals and kept only once running: these calls can pump messages,
// and a nested Stop must not find a half-built capture.
bool WindowCapture::StartItem(wgc::GraphicsCaptureItem item, bool cursor, FrameCallback onFrame,
                              AfterFrame afterFrame, std::function<void()> onClosed) {
    auto& g = Gfx::Get();

    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    try {
        auto shared = std::make_shared<Shared>();
        shared->onFrame    = std::move(onFrame);
        shared->afterFrame = std::move(afterFrame);
        shared->onClosed   = std::move(onClosed);
        const auto size = item.Size();
        shared->poolW = size.Width;
        shared->poolH = size.Height;

        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g.winrtDevice, kFormat, kBufferCount, size);
        session = pool.CreateCaptureSession(item);

        // No yellow capture frame; the pointer only where asked for.
        if (SessionHasProperty(L"IsCursorCaptureEnabled")) {
            try { session.IsCursorCaptureEnabled(cursor); } catch (...) {}
        }
        if (SessionHasProperty(L"IsBorderRequired")) {
            try { session.IsBorderRequired(false); } catch (...) {}
        }
        // Windows caps capture at one frame per 16 ms unless told otherwise.
        if (SessionHasProperty(L"MinUpdateInterval")) {
            try { session.MinUpdateInterval(std::chrono::milliseconds(1)); } catch (...) {}
        }

        std::weak_ptr<Shared> weak = shared;

        auto frameArrived = pool.FrameArrived(winrt::auto_revoke,
            [weak](wgc::Direct3D11CaptureFramePool const& framePool, auto&&) {
                if (auto state = weak.lock()) OnFrame(*state, framePool);
            });

        auto closed = item.Closed(winrt::auto_revoke,
            [weak](auto&&, auto&&) {
                auto state = weak.lock();
                if (!state) return;
                std::lock_guard lock(state->mutex);
                if (state->onClosed) state->onClosed();
            });

        session.StartCapture();

        item_         = std::move(item);
        pool_         = std::move(pool);
        session_      = std::move(session);
        frameArrived_ = std::move(frameArrived);
        closed_       = std::move(closed);
        shared_       = std::move(shared);
        return true;
    } catch (...) {
        if (session) { try { session.Close(); } catch (...) {} }
        if (pool) { try { pool.Close(); } catch (...) {} }
        return false;
    }
}

void WindowCapture::Stop() {
    // Taken out first: closing makes cross-process calls during which this
    // thread pumps messages, and a nested Stop or Start must find nothing.
    auto frameArrived = std::move(frameArrived_);
    auto closed       = std::move(closed_);
    auto shared       = std::exchange(shared_, nullptr);
    auto session      = std::exchange(session_, nullptr);
    auto pool         = std::exchange(pool_, nullptr);
    auto item         = std::exchange(item_, nullptr);

    frameArrived.revoke();
    closed.revoke();

    if (shared) {
        // Blocks until any frame already inside the callbacks has finished.
        {
            std::lock_guard lock(shared->mutex);
            shared->onFrame = nullptr;
            shared->onClosed = nullptr;
        }
        std::lock_guard lock(shared->afterMutex);
        shared->afterFrame = nullptr;
    }

    if (session) {
        try { session.Close(); } catch (...) {}
    }
    if (pool) {
        try { pool.Close(); } catch (...) {}
    }
}

void WindowCapture::OnFrame(Shared& state, wgc::Direct3D11CaptureFramePool const& pool) {
    bool needsResize = false;
    winrt::Windows::Graphics::SizeInt32 newSize{};

    try {
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        const auto contentSize = frame.ContentSize();

        // Recorded only once the pool really is that size, so a failed
        // recreate is tried again on the next frame.
        if (contentSize.Width != state.poolW.load() || contentSize.Height != state.poolH.load()) {
            needsResize = true;
            newSize = contentSize;
        }

        {
            std::lock_guard lock(state.mutex);
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
            state.poolW = newSize.Width;
            state.poolH = newSize.Height;
        } catch (const winrt::hresult_error& e) {
            Gfx::Get().CheckDevice(e.code());
        } catch (...) {}
    }

    std::lock_guard lock(state.afterMutex);
    if (state.afterFrame) state.afterFrame();
}

DesktopCapture::~DesktopCapture() {
    Stop();
}

RECT DesktopCapture::Bounds() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return RECT{ x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                 y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}

bool DesktopCapture::Start(FrameCallback onFrame, WindowCapture::AfterFrame afterFrame) {
    Stop();

    const RECT bounds = Bounds();
    const int width = RectW(bounds), height = RectH(bounds);
    if (width <= 0 || height <= 0 || width > kMaxExtent || height > kMaxExtent) {
        Log(L"capture: desktop %dx%d cannot be captured as one texture", width, height);
        return false;
    }
    const SIZE size{ width, height };

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

    // Kept only once complete, as in WindowCapture::StartItem.
    std::vector<std::unique_ptr<WindowCapture>> captures;
    for (const auto& [monitor, rect] : monitors) {
        const LONG atX = rect.left - bounds.left;
        const LONG atY = rect.top - bounds.top;
        if (atX < 0 || atY < 0 || atX >= width || atY >= height) continue;

        auto capture = std::make_unique<WindowCapture>();
        const bool started = capture->StartMonitor(monitor,
            [onFrame, atX, atY, size](ID3D11Texture2D* frame, UINT contentW, UINT contentH) {
                D3D11_TEXTURE2D_DESC fd{};
                frame->GetDesc(&fd);
                const UINT w = (std::min)({ contentW, fd.Width, static_cast<UINT>(size.cx - atX) });
                const UINT h = (std::min)({ contentH, fd.Height, static_cast<UINT>(size.cy - atY) });
                if (w > 0 && h > 0) onFrame(DesktopFrame{ frame, w, h, POINT{ atX, atY }, size });
            },
            afterFrame);
        if (started) {
            captures.push_back(std::move(capture));
        } else {
            Log(L"capture: monitor at %ld,%ld could not be captured", rect.left, rect.top);
        }
    }

    if (captures.empty()) return false;
    Log(L"capture: desktop %dx%d from %zu monitor(s)", width, height, captures.size());
    size_ = size;
    monitors_ = std::move(captures);
    return true;
}

void DesktopCapture::Stop() {
    // Taken out first, as in WindowCapture::Stop. Each monitor's Stop waits
    // out a frame already in its callbacks.
    auto monitors = std::exchange(monitors_, {});
    size_ = SIZE{};
    for (auto& m : monitors) m->Stop();
}

}  // namespace rvm
