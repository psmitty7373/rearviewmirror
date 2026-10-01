#pragma once
#include "gfx.h"

#include <thread>

namespace rvm::login {

// The whole virtual screen through DXGI Desktop Duplication: the one capture
// API that works on the secure desktops (sign-in, Ctrl+Alt+Del, UAC), for a
// process running as SYSTEM on them. Each monitor of the device's adapter is
// copied to its place in one texture; the pointer, which duplication reports
// separately, is drawn on top.
//
// Duplication ends whenever the input desktop switches or the display mode
// changes. The thread then moves itself to the new input desktop and starts
// over, so it follows the screen wherever it goes.
class DuplicationCapture {
public:
    // Both on the capture thread. `onFrame` gets the composed picture under
    // Gfx::deviceMutex, as the renderer's frame tee would; `onSize` the
    // picture's size when it changes, with no lock held.
    using FrameCallback = std::function<void(ID3D11Texture2D* frame)>;
    using SizeCallback  = std::function<void(SIZE size)>;

    ~DuplicationCapture();

    bool Start(FrameCallback onFrame, SizeCallback onSize);
    void Stop();

    // Offers the last picture again, from any thread: duplication reports
    // nothing while the screen is still.
    void Repush();

private:
    struct Output {
        winrt::com_ptr<IDXGIOutputDuplication> dup;
        POINT at{};   // Top-left within the picture.
    };

    void Loop();
    bool Attach();
    void Detach();
    bool FollowInputDesktop();
    bool EnsureTextures(UINT width, UINT height);
    void TakePointer(const Output& out, size_t index, const DXGI_OUTDUPL_FRAME_INFO& info);
    void Compose();

    std::thread thread_;
    std::atomic<bool> running_{ false };
    FrameCallback onFrame_;
    SizeCallback  onSize_;

    // Capture thread only.
    std::vector<Output> outputs_;
    HDESK desktop_ = nullptr;
    std::wstring desktopName_;
    SIZE size_{};
    std::vector<uint8_t> shapeBuffer_;

    // Under Gfx::deviceMutex.
    winrt::com_ptr<ID3D11Texture2D>    composite_;   // The screen as duplicated.
    winrt::com_ptr<ID3D11Texture2D>    frame_;       // The same with the pointer drawn in.
    winrt::com_ptr<ID2D1DeviceContext> d2d_;
    winrt::com_ptr<ID2D1Bitmap1>       target_;
    winrt::com_ptr<ID2D1Bitmap1>       pointer_;
    POINT pointerAt_{};
    size_t pointerOwner_ = SIZE_MAX;   // The output that last reported it visible.
    bool  pointerVisible_ = false;
    bool  havePicture_ = false;
};

}  // namespace rvm::login
