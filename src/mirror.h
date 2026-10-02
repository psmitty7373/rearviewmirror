#pragma once
#include "capture.h"
#include "persist.h"
#include "renderer.h"

namespace rvm {

// Mirror to app, wParam the mirror's id.
constexpr UINT WM_RVM_MIRROR_IDLE    = WM_APP + 30;   // Hidden, and a frame nobody wanted.
constexpr UINT WM_RVM_MIRROR_RESTART = WM_APP + 31;   // Needs RestartCapture().

// One floating always-on-top view of a rectangular slice of another window,
// or of the whole desktop.
class Mirror : public WindowHost {
public:
    explicit Mirror(uint32_t id) : id_(id) {}
    ~Mirror();

    // Stable identity for menus, messages and the manager: vector indices
    // shift whenever a mirror is removed inside a nested message loop.
    uint32_t Id() const { return id_; }

    // `notify` receives lifecycle messages. `target` may be null only for a
    // mirror restored disabled, which has nothing to bind to yet.
    bool Create(const MirrorState& state, HWND target, HWND notify);
    void Destroy();

    // Off stops the capture entirely, so it costs nothing. False if it could not
    // be turned back on because the source window is gone.
    // `exclude`: windows other groups of mirrors are showing. `preferred`: the
    // window this mirror's own group is showing, if any; used as is.
    bool SetEnabled(bool enabled, const std::vector<HWND>& exclude = {}, HWND preferred = nullptr);
    bool Enabled() const { return state_.enabled; }

    // The source window went away. Capture stops and the window hides, but the
    // mirror keeps its place and settings and waits for a matching window.
    // The mirror reports the loss to `notify` and the app calls this.
    void Orphan();
    bool Orphaned() const { return orphaned_; }

    // Bind an orphaned mirror to a window that has since appeared. Windows in
    // `exclude` are already someone else's source.
    bool TryRebind(const std::vector<HWND>& exclude, HWND preferred = nullptr,
                   ExeNameCache* exes = nullptr);

    // Hidden with nobody watching, a mirror sleeps: capture stops until it is
    // shown or watched. It reports such a frame to `notify`; the app, which
    // knows who watches, calls Sleep.
    void Sleep();
    bool Sleeping() const { return sleeping_; }
    // Whether an unwanted frame was reported since last asked.
    bool TakeIdleReport() { return idleReported_.exchange(false); }

    // Captures afresh: a sleeping mirror wakes, and a running one starts over
    // so a still source sends a whole new frame. The mirror asks for this
    // through `notify`, as starting a capture must wait for any other.
    void RestartCapture();

    std::wstring DisplayName() const;
    MirrorState SaveState() const;
    HWND Target() const { return target_; }
    uint32_t Group() const { return state_.group; }

    void SetClickThrough(bool enabled);
    bool ClickThrough() const { return state_.clickThrough; }

    // Hidden keeps streaming with no window on screen, capturing only while
    // watched (see Sleep). Since a hidden mirror cannot be right-clicked, the
    // manager and tray bring it back.
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

    // Monitors were added, removed or changed resolution: a desktop mirror
    // starts over on the new layout. False if that failed; it then waits and
    // retries like an orphan.
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
    // Capture is starting or stopping, which pumps messages; nothing nested
    // may start or stop it meanwhile.
    bool transitioning_ = false;
};

}  // namespace rvm
