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
    auto& g = Gfx::Get();

    try {
        item_ = CreateItemForWindow(target);
        if (!item_) return false;

        shared_ = std::make_shared<Shared>();
        shared_->onFrame  = std::move(onFrame);
        shared_->onClosed = std::move(onClosed);
        shared_->poolSize = item_.Size();

        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g.winrtDevice, kFormat, kBufferCount, shared_->poolSize);
        session_ = pool_.CreateCaptureSession(item_);

        // We want the window's pixels, not the capture affordances.
        if (SessionHasProperty(L"IsCursorCaptureEnabled")) {
            try { session_.IsCursorCaptureEnabled(false); } catch (...) {}
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

}  // namespace rvm
