#pragma once
#include "capture.h"
#include "persist.h"
#include "renderer.h"

namespace rvm {

// A floating always-on-top view of a slice of a window or of the desktop.
class Mirror : public WindowHost {
public:
    explicit Mirror(uint32_t id) : id_(id) {}
    ~Mirror();

    // Stable identity: list positions shift as mirrors are removed.
    uint32_t Id() const { return id_; }

    // `notify` gets lifecycle messages. `target` may be null if disabled or desktop.
    bool Create(const MirrorState& state, HWND target, HWND notify);
    void Destroy();

    // False if it could not turn on: source gone. `exclude`: windows other
    // groups show. `preferred`: the window this mirror's group shows, if any.
    bool SetEnabled(bool enabled, const std::vector<HWND>& exclude = {}, HWND preferred = nullptr);
    bool Enabled() const { return state_.enabled; }

    // The source went away: capture stops and the window hides, but the mirror
    // keeps its settings and waits. It reports the loss; the app calls this.
    void Orphan();
    bool Orphaned() const { return orphaned_; }

    // Bind an orphaned mirror to a window that has since appeared. Windows in
    // `exclude` are already someone else's source.
    bool TryRebind(const std::vector<HWND>& exclude, HWND preferred = nullptr,
                   ExeNameCache* exes = nullptr);

    // Hidden and unwatched, a mirror stops capturing until shown or watched. It
    // reports unwanted frames; the app, which knows who watches, calls Sleep.
    void Sleep();
    bool Sleeping() const { return sleeping_; }
    // Whether an unwanted frame was reported since last asked.
    bool TakeIdleReport() { return idleReported_.exchange(false); }

    // Wakes a sleeping mirror, or restarts a running one so a still source
    // sends a whole frame. Requested through `notify` (see App::Transition).
    void RestartCapture();

    std::wstring DisplayName() const;
    MirrorState SaveState() const;
    HWND Target() const { return target_; }
    uint32_t Group() const { return state_.group; }

    void SetClickThrough(bool enabled);
    bool ClickThrough() const { return state_.clickThrough; }

    // No window on screen; still streams, capturing only while watched (see
    // Sleep). Only the manager can show it again.
    void SetHidden(bool hidden);
    bool Hidden() const { return state_.hidden; }

    // `persist` is false mid-drag: the file is written once, on release.
    void SetOpacity(float opacity, bool persist = true);
    void SetZoom(float factor, bool persist = true);
    void SetTracking(TrackMode mode);
    void ReselectRegion();

    float Opacity() const { return state_.opacity; }
    // The application a window mirror watches, or "Desktop".
    std::wstring SourceName() const { return IsDesktop() ? L"Desktop" : state_.exeName; }
    bool IsDesktop() const { return state_.source == SourceKind::Desktop; }
#if RVM_REMOTE_CONTROL
    bool IsFullDesktop() const;
#endif

    // Monitors changed: a desktop mirror starts over on the new layout. False
    // if that failed; it then waits like an orphan.
    bool RestartDesktop();
    SIZE NativeSize() const;
    float CurrentScale() const;

    void SetFrameSink(MirrorRenderer::FrameSink sink, std::function<bool()> wanted) {
        renderer_.SetFrameSink(std::move(sink), std::move(wanted));
    }
    void RepushFrame() { renderer_.RepushFrame(); }

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

private:
    bool StartCapture();
    void StopCapture();
    // A frame nobody wanted, from any thread: tells `notify`, once until asked.
    void ReportIdle();
    void RequestRestart();
    // The source can't be captured now: wait and retry like an orphan.
    void WaitForSource();
    // The crop is the whole desktop as it was when last sized.
    bool WholeDesktop() const;
    // An entire-desktop mirror follows the desktop to the size it now has.
    void FollowDesktopSize(SIZE now);
    SIZE ContentSize() const;
    bool SourceAlive() const;
    void ShowContextMenu(POINT screenPt);
    void ApplyClickThroughStyle();
    void UpdateVisibility();   // Shown only when on, bound, not hidden and awake.
    void UpdateBorder();
    void PlaceInitially();
    void NotifyStateChanged();
    void ConstrainSizing(WPARAM edge, RECT* rect);
    void FollowCropShape();

    WindowCapture  capture_;
    DesktopCapture desktop_;   // Used instead of capture_ for a desktop mirror.
    MirrorRenderer renderer_;
    MirrorState    state_;

    const uint32_t id_;
    HWND target_ = nullptr;
    DWORD targetThread_ = 0, targetProcess_ = 0;   // Whose target_ was, when captured.
    HWND notify_ = nullptr;

    bool hovered_  = false;
    bool snapped_  = false;   // Held at 100% by the resize detent.
    bool orphaned_ = false;
    bool sleeping_ = false;
    std::atomic<bool> idleReported_{ false };
    RECT sizeMoveStart_{};
    bool transitioning_ = false;   // Capture starting or stopping, pumping messages.
};

}  // namespace rvm
