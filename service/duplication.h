#pragma once
#include "gfx.h"

#include <chrono>
#include <set>
#include <thread>

namespace rvm::login {

// The whole virtual screen through DXGI Desktop Duplication, the one capture
// API that works on the secure desktops (as SYSTEM). One thread per monitor of
// the device's adapter; the pointer is drawn on top. Follows the input
// desktop across switches.
class DuplicationCapture {
public:
    // `onFrame`: the composed picture, under Gfx::deviceMutex, on a capture
    // thread or Repush's. `onSize`: on a size change, no lock held. Composed
    // only while `wanted`, at most `fps` times a second.
    using FrameCallback  = std::function<void(ID3D11Texture2D* frame)>;
    using SizeCallback   = std::function<void(SIZE size)>;
    using WantedCallback = std::function<bool()>;

    ~DuplicationCapture();

    bool Start(FrameCallback onFrame, SizeCallback onSize, WantedCallback wanted, UINT fps);
    void Stop();

    // Any thread: offers the latest picture again (a still screen reports nothing).
    void Repush();

private:
    struct Output {
        winrt::com_ptr<IDXGIOutputDuplication> dup;
        POINT at{};          // Top-left within the picture.
        bool  whole = true;  // Its first frame is copied entire.
        std::vector<DXGI_OUTDUPL_MOVE_RECT> moves;
        std::vector<RECT>    dirty;
        std::vector<uint8_t> shape;
    };

    void Loop();
    bool Attach();
    bool FollowInputDesktop();
    void RunOutput(size_t index);
    static bool ReadChanges(Output& out, UINT bytes);
    UINT ComposeIfDue();

    // Under Gfx::deviceMutex.
    bool EnsureTextures(UINT width, UINT height);
    void ClearComposite();
    void CopyChanges(Output& out, ID3D11Texture2D* texture, bool whole);
    void Damage(const RECT& r);
    void TakePointer(const Output& out, size_t index, const DXGI_OUTDUPL_FRAME_INFO& info,
                     const std::vector<uint32_t>& shape, UINT shapeW, UINT shapeH);
    void Compose();

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> lost_{ false };   // A monitor's duplication ended: all attach again.
    FrameCallback  onFrame_;
    SizeCallback   onSize_;
    WantedCallback wanted_;
    std::chrono::steady_clock::duration interval_{};

    // Changed only by the capture thread while no output thread runs.
    std::vector<Output> outputs_;
    std::vector<RECT> layout_;
    std::set<std::wstring> rotated_;
    HDESK desktop_ = nullptr;
    std::wstring desktopName_;
    SIZE size_{};

    // Under Gfx::deviceMutex.
    winrt::com_ptr<ID3D11Texture2D>    composite_;   // The screen as duplicated.
    winrt::com_ptr<ID3D11Texture2D>    frame_;       // The same with the pointer drawn in.
    winrt::com_ptr<ID2D1DeviceContext> d2d_;
    winrt::com_ptr<ID2D1Bitmap1>       target_;
    winrt::com_ptr<ID2D1Bitmap1>       pointer_;
    RECT  bounds_{};                   // Of both textures.
    POINT pointerAt_{};
    size_t pointerOwner_ = SIZE_MAX;   // The output that last reported it visible.
    bool  pointerVisible_ = false;
    bool  havePicture_ = false;
    // What frame_ lacks from composite_ and the pointer since it was composed.
    bool  stale_ = false;
    bool  damageAll_ = true;
    std::vector<RECT> damage_;
    RECT  pointerDrawn_{};             // Where frame_ has the pointer.
    std::chrono::steady_clock::time_point nextCompose_{};
};

}  // namespace rvm::login
