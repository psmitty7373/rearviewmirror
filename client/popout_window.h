#pragma once
#include "overlay.h"

namespace rvm {

// Posted to the window given to Create() when the user acts on a pop-out.
// wParam is a PopoutEvent, lParam the token given to Create().
constexpr UINT WM_RVM_POPOUT_EVENT = WM_APP + 21;
enum class PopoutEvent : WPARAM { ReturnToCanvas = 1, Changed };

// A stream in its own borderless always-on-top window, handled like the app's
// local mirror window. The client window feeds it frames.
class PopoutWindow : public D2DOverlay {
public:
    struct Settings {
        RECT  rect{};                // Empty means "place it".
        float opacity = 1.0f;
        bool  clickThrough = false;
        bool  aspectLocked = true;
    };

    bool Create(HWND notify, LPARAM token, const Settings& settings, UINT nativeW, UINT nativeH,
                HWND placeNear);
    Settings CurrentSettings() const;

    // Call Invalidate() afterwards. The 100% size follows the stream.
    void SetFrame(const winrt::com_ptr<ID3D11Texture2D>& texture, UINT width, UINT height,
                  uint64_t frames);
    // Shown until the first frame arrives.
    void SetWaitingText(std::wstring_view text) {
        if (waitingText_ != text) waitingText_.assign(text);
    }
    void SetClickThrough(bool on);
    bool ClickThrough() const { return clickThrough_; }

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;

protected:
    void OnDraw(ID2D1DeviceContext* dc) override;

private:
    void PlaceInitially(HWND placeNear);
    void ConstrainSizing(WPARAM edge, RECT* rect);
    void SetZoom(float factor);
    void ShowContextMenu(POINT screenPt);
    void ApplyClickThroughStyle();
    void Notify(PopoutEvent event);

    HWND   notify_ = nullptr;
    LPARAM token_ = 0;

    UINT  nativeW_ = 0, nativeH_ = 0;
    float opacity_ = 1.0f;
    bool  clickThrough_ = false;
    bool  aspectLocked_ = true;

    winrt::com_ptr<ID3D11Texture2D> texture_;
    UINT     frameW_ = 0, frameH_ = 0;
    uint64_t frames_ = 0;
    std::wstring waitingText_ = L"Waiting for the first frame…";
    ID3D11Texture2D* bitmapFor_ = nullptr;
    winrt::com_ptr<ID2D1Bitmap1> bitmap_;

    bool hovered_ = false;
    bool snapped_ = false;
};

}  // namespace rvm
