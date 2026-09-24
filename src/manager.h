#pragma once
#include "overlay.h"

namespace rvm {

class App;

// Lists every mirror as a card with its own switch, sliders and buttons.
// Custom-drawn in Direct2D to match the rest of the app.
class ManagerWindow : public D2DOverlay {
public:
    void Open(App* app);
    bool IsOpen() const;

    // Called when mirrors are added, removed or changed elsewhere.
    void Refresh();

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;

protected:
    void PrepareDraw() override;
    void OnDraw(ID2D1DeviceContext* dc) override;

private:
    enum class Part {
        None, Toggle, Opacity, Scale, Region, Close, ClickThrough, Hidden,   // On a card.
        NewMirror, AllClickThrough, Streaming,                                // In the header.
    };

    // A control on a card. Cards are addressed by mirror id, never by position:
    // a mirror can be removed between the press and the release.
    struct Hit {
        uint32_t id = 0;
        Part part = Part::None;
        bool operator==(const Hit& o) const { return id == o.id && part == o.part; }
    };

    // Everything OnDraw needs, gathered by PrepareDraw before the device lock
    // is taken so no syscall or mirror lock runs inside it.
    struct CardView {
        uint32_t     id = 0;
        std::wstring name;
        std::wstring subtitle;
        bool  enabled = true;
        bool  clickThrough = false;
        bool  hidden = false;
        float opacity = 1.0f;
        float scale   = 1.0f;
    };

    struct CardRects {
        D2D1_RECT_F card, toggle, title, subtitle;
        D2D1_RECT_F opacityTrack, opacityValue;
        D2D1_RECT_F scaleTrack, scaleValue;
        D2D1_RECT_F regionButton, closeButton;
        D2D1_RECT_F clickThroughButton, hiddenButton;
    };

    void  EnsureFonts();
    float S(float value) const { return value * dpiScale_; }
    float ContentHeight() const;
    float ViewportHeight() const;
    void  ClampScroll();
    int   CardIndex(uint32_t id) const;

    CardRects   LayoutCard(int index) const;
    D2D1_RECT_F NewMirrorButton() const;
    D2D1_RECT_F AllClickThroughButton() const;
    D2D1_RECT_F StreamingButton() const;
    Hit  HitTest(POINT pt) const;

    void ApplySliderDrag(POINT pt);
    void DrawCard(ID2D1DeviceContext* dc, const CardView& card, const CardRects& r);
    void DrawSlider(ID2D1DeviceContext* dc, const D2D1_RECT_F& track, float t,
                    bool hot, bool showDetent);
    void DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& r,
                    const std::wstring& label, bool hot, bool danger, bool on = false);
    void DrawLabel(ID2D1DeviceContext* dc, const std::wstring& text,
                   const D2D1_RECT_F& rect, IDWriteTextFormat* format,
                   const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align);

    App* app_ = nullptr;

    std::vector<CardView> cards_;
    bool   allClickThrough_ = false;
    bool   streamingOn_ = false;
    size_t streamClients_ = 0;

    winrt::com_ptr<IDWriteTextFormat> titleFont_;
    winrt::com_ptr<IDWriteTextFormat> bodyFont_;
    winrt::com_ptr<IDWriteTextFormat> smallFont_;

    float dpiScale_ = 1.0f;
    float scroll_ = 0.0f;

    Hit  hot_{};
    Hit  active_{};      // Button being pressed or slider being dragged.
    bool dragging_ = false;
    bool mouseTracked_ = false;
};

}  // namespace rvm
