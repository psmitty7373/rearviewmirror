#pragma once
#include "gfx.h"

namespace rvm {

// Draws a cropped sub-rectangle of a captured window. Frames are submitted from
// the capture thread; the UI thread only changes parameters or asks for repaints.
class MirrorRenderer {
public:
    bool Init(HWND hwnd, UINT width, UINT height);
    void Shutdown();

    void Resize(UINT width, UINT height);

    // Crop in capture-texture pixels, plus the capture size it was chosen against:
    // proportional tracking needs the base to know how far the source has moved.
    void SetCrop(const RECT& crop, const SIZE& baseSize);
    void SetTracking(TrackMode mode);

    // The crop actually being drawn, after tracking and clamping. Takes the
    // renderer lock, so never call it from a Direct2D draw (see Gfx::deviceMutex).
    RECT EffectiveCrop() const;

    // Lock-free: WM_SIZING and the manager's draw need this without contention.
    SIZE EffectiveCropSize() const;

    void SetOpacity(float opacity);
    void SetBorder(float r, float g, float b, float a);

    // Off while the window is hidden: frames are still cached and teed to
    // the stream, but nothing is drawn or presented.
    void SetPresenting(bool on) { presenting_.store(on); }

    // Called on the capture thread with a freshly arrived frame.
    void SubmitFrame(ID3D11Texture2D* source, UINT contentWidth, UINT contentHeight);

    // Re-present the last frame after a resize or a parameter change.
    void Redraw();

    // Tee for streaming: called after every new frame with the cache texture
    // and the effective crop, on the capture thread, under both the renderer
    // and device locks. Must be quick; the crop copy is a single GPU blit.
    using FrameSink = std::function<void(ID3D11Texture2D* cache, const RECT& crop)>;
    void SetFrameSink(FrameSink sink);

    // Offers the last frame to the sink again. Capture only delivers frames
    // when the source changes, so a stream that starts while the source is
    // still would otherwise never get a first frame.
    void RepushFrame();

private:
    bool EnsureCache(UINT width, UINT height);
    bool EnsureCrop(UINT width, UINT height);
    bool EnsureRenderTarget();

    // Draws without presenting. Callers hold mutex_ and Gfx::deviceMutex, then
    // release both before presenting, so no lock spans the vsync wait.
    bool RenderLocked();
    void Present(UINT syncInterval);
    RECT ComputeCropLocked() const;

    mutable std::mutex mutex_;
    std::mutex presentMutex_;   // Present against ResizeBuffers only.
    CompSurface comp_;

    winrt::com_ptr<ID3D11Texture2D>          cacheTex_;
    winrt::com_ptr<ID3D11ShaderResourceView> cacheSrv_;
    winrt::com_ptr<ID3D11Texture2D>          cropTex_;
    winrt::com_ptr<ID3D11ShaderResourceView> cropSrv_;
    winrt::com_ptr<ID3D11RenderTargetView>   rtv_;

    UINT cacheW_ = 0, cacheH_ = 0;
    UINT cropTexW_ = 0, cropTexH_ = 0;
    RECT crop_{};
    SIZE baseSize_{};
    TrackMode track_ = TrackMode::Anchored;

    // The mipped crop only needs rebuilding when the frame or the crop changed,
    // not for every opacity or border repaint.
    RECT lastCrop_{};
    bool cropDirty_ = true;

    float opacity_ = 1.0f;
    float border_[4]{ kAccentR, kAccentG, kAccentB, 0.0f };

    std::atomic<int32_t> effCropW_{ 0 };
    std::atomic<int32_t> effCropH_{ 0 };
    std::atomic<bool>    presenting_{ true };

    FrameSink sink_;
};

}  // namespace rvm
