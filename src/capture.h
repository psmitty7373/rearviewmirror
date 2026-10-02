#pragma once
#include "gfx.h"

namespace rvm {

// A window's capture size without waiting for a frame; {0,0} if it can't be captured.
SIZE CaptureItemSize(HWND hwnd);

bool CaptureSupported();

// Windows Graphics Capture of one window or monitor; frames arrive as GPU
// textures on a pool thread.
class WindowCapture {
public:
    // The texture is only valid for the duration of the call.
    using FrameCallback = std::function<void(ID3D11Texture2D*, UINT, UINT)>;
    // Runs after each frame callback, once the frame is back with capture:
    // for slow work, like a present that waits for vsync.
    using AfterFrame = std::function<void()>;

    ~WindowCapture();

    bool Start(HWND target, FrameCallback onFrame, AfterFrame afterFrame,
               std::function<void()> onClosed);
    // One whole monitor, mouse pointer included.
    bool StartMonitor(HMONITOR monitor, FrameCallback onFrame, AfterFrame afterFrame);
    void Stop();

    SIZE ContentSize() const;

private:
    bool StartItem(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item, bool cursor,
                   FrameCallback onFrame, AfterFrame afterFrame, std::function<void()> onClosed);

    // Event handlers hold this weakly and lock across the callbacks, so Stop()
    // cannot return while a frame is still inside them.
    struct Shared {
        std::mutex mutex;
        FrameCallback onFrame;
        std::function<void()> onClosed;
        std::mutex afterMutex;   // Apart, so a present never holds up the next copy.
        AfterFrame afterFrame;
        std::atomic<int32_t> poolW{ 0 }, poolH{ 0 };
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

// One monitor's new picture, as part of the desktop.
struct DesktopFrame {
    ID3D11Texture2D* texture;   // Valid only during the call.
    UINT  width, height;        // Its top-left part that is the monitor...
    POINT at;                   // ...placed here in the virtual screen...
    SIZE  desktop;              // ...which is this big.
};

// The whole desktop: one capture per monitor, each delivering its own frames
// for the receiver to place. A change of monitors needs a Stop and Start.
class DesktopCapture {
public:
    using FrameCallback = std::function<void(const DesktopFrame&)>;

    ~DesktopCapture();

    bool Start(FrameCallback onFrame, WindowCapture::AfterFrame afterFrame);
    void Stop();

    SIZE ContentSize() const { return size_; }

    // The virtual screen, in screen pixels.
    static RECT Bounds();

private:
    SIZE size_{};
    std::vector<std::unique_ptr<WindowCapture>> monitors_;
};

}  // namespace rvm
