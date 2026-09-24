#pragma once
#include "gfx.h"

namespace rvm {

// A borderless, transparent, always-on-top window drawn with Direct2D onto a
// DirectComposition swapchain.
class D2DOverlay : public WindowHost {
public:
    virtual ~D2DOverlay();

    // `clickThrough` makes the overlay ignore hit-testing entirely.
    bool Create(const wchar_t* className, RECT bounds, bool clickThrough);

    // The same surface, but as an ordinary framed window.
    bool CreateStyled(const wchar_t* className, const wchar_t* title, RECT bounds,
                      DWORD style, DWORD exStyle);

    void Destroy();

    void SetBounds(const RECT& bounds);
    void Show();
    void Hide();
    void Render();

    UINT Width()  const { return comp_.Width(); }
    UINT Height() const { return comp_.Height(); }

    virtual LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);

protected:
    // Runs before Render takes the device lock. Anything that needs syscalls
    // or other locks belongs here, so OnDraw touches nothing shared.
    virtual void PrepareDraw() {}
    virtual void OnDraw(ID2D1DeviceContext* dc) = 0;

    // Rounded pill with a label, used for hints and size read-outs. `alignX`
    // is 0 for left, 1 for centre, 2 for right; the same for `alignY`.
    D2D1_SIZE_F DrawChip(ID2D1DeviceContext* dc, const std::wstring& text,
                         float x, float y, int alignX, int alignY,
                         float bgAlpha = 0.92f);

    D2D1_SIZE_F MeasureText(const std::wstring& text);

    void ResizeSurface(UINT width, UINT height);

    winrt::com_ptr<ID2D1DeviceContext> dc_;
    winrt::com_ptr<IDWriteTextFormat>  font_;

    // Recoloured per use. Overlays redraw on every mouse move, and a brush per
    // draw would be a COM allocation per frame.
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;

    CompSurface comp_;
    RECT bounds_{};

private:
    bool EnsureTarget();
    void DropTarget();

    winrt::com_ptr<ID2D1Bitmap1> targetBitmap_;
};

}  // namespace rvm
