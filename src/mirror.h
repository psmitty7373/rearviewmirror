#pragma once
#include "capture.h"
#include "persist.h"
#include "renderer.h"

namespace rvm {

// One floating always-on-top view of a rectangular slice of another window.
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
    void Orphan();
    bool Orphaned() const { return orphaned_; }

    // Bind an orphaned mirror to a window that has since appeared. Windows in
    // `exclude` are already someone else's source.
    bool TryRebind(const std::vector<HWND>& exclude, HWND preferred = nullptr);

    std::wstring DisplayName() const;
    MirrorState SaveState() const;
    HWND Target() const { return target_; }
    uint32_t Group() const { return state_.group; }

    void SetClickThrough(bool enabled);
    bool ClickThrough() const { return state_.clickThrough; }

    // Hidden keeps capturing and streaming with no window on screen. Since a
    // hidden mirror cannot be right-clicked, the manager and tray bring it back.
    void SetHidden(bool hidden);
    bool Hidden() const { return state_.hidden; }

    // `persist` is false mid-drag: the file is written once, on release.
    void SetOpacity(float opacity, bool persist = true);
    void SetZoom(float factor, bool persist = true);
    void SetTracking(TrackMode mode);
    void ReselectRegion();

    float Opacity() const { return state_.opacity; }
    const std::wstring& SourceName() const { return state_.exeName; }
    SIZE NativeSize() const;
    float CurrentScale() const;

    void SetFrameSink(MirrorRenderer::FrameSink sink) { renderer_.SetFrameSink(std::move(sink)); }
    void RepushFrame() { renderer_.RepushFrame(); }

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

private:
    bool StartCapture();
    void ShowContextMenu(POINT screenPt);
    void ApplyClickThroughStyle();
    void UpdateVisibility();   // Shown only when on, bound and not hidden.
    void UpdateBorder();
    void PlaceInitially();
    void NotifyStateChanged();
    void ConstrainSizing(WPARAM edge, RECT* rect);

    WindowCapture  capture_;
    MirrorRenderer renderer_;
    MirrorState    state_;

    const uint32_t id_;
    HWND target_ = nullptr;
    HWND notify_ = nullptr;

    bool hovered_  = false;
    bool snapped_  = false;   // Held at 100% by the resize detent.
    bool orphaned_ = false;
};

}  // namespace rvm
