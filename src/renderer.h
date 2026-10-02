#pragma once
#include "gfx.h"

namespace rvm {

// Draws a cropped sub-rectangle of a captured window or the desktop. Frames
// arrive on capture threads; the UI thread changes parameters and redraws.
class MirrorRenderer {
public:
    bool Init(HWND hwnd, UINT width, UINT height);
    void Shutdown();

    void Resize(UINT width, UINT height);

    // Crop in capture pixels, and the capture size it was chosen against. Only
    // the crop is kept of each frame, so both return true when the crop now
    // reaches pixels never kept: the capture must restart to send them.
    bool SetCrop(const RECT& crop, const SIZE& baseSize);
    bool SetTracking(TrackMode mode);

    // The crop actually being drawn, after tracking and clamping. Takes the
    // renderer lock, so never call it from a Direct2D draw (see Gfx::deviceMutex).
    RECT EffectiveCrop() const;

    // Lock-free: WM_SIZING and the manager's draw need this without contention.
    // A draw that changes it posts WM_RVM_CROP_RESHAPED to the window.
    SIZE EffectiveCropSize() const;

    void SetOpacity(float opacity);
    void SetBorder(float r, float g, float b, float a);

    // Off while the window is hidden: frames are still cached and streamed,
    // but nothing is drawn or presented.
    void SetPresenting(bool on) { presenting_.store(on); }

    // On a capture thread: a frame whose top-left `width` x `height` is the
    // part at `at` of a source `full` in size (all of a window, or one monitor
    // of the desktop). False if nobody wanted it: not presenting, not streamed.
    bool SubmitFrame(ID3D11Texture2D* source, UINT width, UINT height, POINT at, SIZE full);

    // After SubmitFrame, once the capture frame is released: draws and presents
    // whatever has arrived. Frames from other threads meanwhile join that draw.
    void PresentFrames();

    // Frees the frame textures; `blank` also presents an empty picture.
    void DropFrames(bool blank);

    // Re-present the last frame after a resize or a parameter change.
    void Redraw();

    // Tee for streaming. `wanted` is asked for every frame and must be cheap;
    // `sink` gets the cache texture and the effective crop, under the renderer
    // and device locks.
    using FrameSink = std::function<void(ID3D11Texture2D* cache, const RECT& crop)>;
    void SetFrameSink(FrameSink sink, std::function<bool()> wanted);

    // Offers the last frame to the sink again: capture sends frames only on
    // change, so a new stream of a still source would get none.
    void RepushFrame();

private:
    bool EnsureCache(UINT width, UINT height, bool clear);
    bool EnsureCrop(UINT width, UINT height);
    bool EnsureRenderTarget();
    bool Minifying(const RECT& crop) const;
    // cropTex_ holds the newest pixels of lastCrop_; copies them to the cache.
    void SyncCacheLocked();

    // Draws without presenting. Callers hold presentMutex_, mutex_ and
    // Gfx::deviceMutex, in that order, and drop the last two before presenting.
    bool RenderLocked();
    void Present(UINT syncInterval);
    RECT ComputeCropLocked() const;
    bool CropReachesNewLocked(const RECT& before) const;

    mutable std::mutex mutex_;
    std::mutex presentMutex_;   // One draw-and-present, or resize, at a time.
    CompSurface comp_;
    HWND hwnd_ = nullptr;

    // The cache holds the effective crop (and a pixel round it, for filtering)
    // of the latest frames. When minifying, frames go straight into the top of
    // cropTex_'s mip chain instead, and the cache catches up only if needed.
    winrt::com_ptr<ID3D11Texture2D>          cacheTex_;
    winrt::com_ptr<ID3D11ShaderResourceView> cacheSrv_;
    winrt::com_ptr<ID3D11Texture2D>          cropTex_;
    winrt::com_ptr<ID3D11ShaderResourceView> cropSrv_;
    winrt::com_ptr<ID3D11RenderTargetView>   rtv_;

    UINT cacheW_ = 0, cacheH_ = 0;   // Kept when the textures are dropped.
    UINT cropTexW_ = 0, cropTexH_ = 0;
    RECT crop_{};
    SIZE baseSize_{};
    TrackMode track_ = TrackMode::Anchored;

    RECT lastCrop_{};          // What cropTex_ holds.
    bool cropDirty_ = true;    // The cache has newer pixels than cropTex_.
    bool cacheBehind_ = false; // cropTex_ has newer pixels than the cache.
    bool mipsStale_ = false;
    bool drawPending_ = false; // A frame arrived that is not drawn yet.
    bool drawQueued_ = false;  // A thread is on its way to draw it.

    float opacity_ = 1.0f;
    float border_[4]{ kAccentR, kAccentG, kAccentB, 0.0f };

    std::atomic<int32_t> effCropW_{ 0 };
    std::atomic<int32_t> effCropH_{ 0 };
    std::atomic<bool>    presenting_{ true };

    FrameSink sink_;
    std::function<bool()> sinkWanted_;
};

}  // namespace rvm
