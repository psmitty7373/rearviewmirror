#pragma once
#include "gfx.h"

namespace rvm {

// Size of a window's capture surface, without waiting for a frame. {0,0} if the
// window cannot be captured.
SIZE CaptureItemSize(HWND hwnd);

bool CaptureSupported();

// Captures one window with Windows Graphics Capture. Frames arrive as GPU
// textures on a pool thread and are never read back to the CPU.
class WindowCapture {
public:
    // The texture is only valid for the duration of the call.
    using FrameCallback = std::function<void(ID3D11Texture2D*, UINT, UINT)>;

    ~WindowCapture();

    bool Start(HWND target, FrameCallback onFrame, std::function<void()> onClosed);
    void Stop();

    SIZE ContentSize() const;

private:
    // The event handlers hold this too, via a weak_ptr, and take its mutex
    // across the callback. Stop() takes the same mutex, so it cannot return
    // while a frame is still inside the owner's callback.
    struct Shared {
        std::mutex mutex;
        FrameCallback onFrame;
        std::function<void()> onClosed;
        winrt::Windows::Graphics::SizeInt32 poolSize{};
    };

    static void OnFrame(Shared& state,
                        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& pool);

    std::shared_ptr<Shared> shared_;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem        item_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession     session_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker closed_;
};

}  // namespace rvm
