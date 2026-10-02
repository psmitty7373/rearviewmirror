#pragma once
#include "gfx.h"

#include <string_view>
#include <unordered_map>

namespace rvm {

// A window drawn with Direct2D onto a DirectComposition swapchain.
class D2DOverlay : public WindowHost {
public:
    virtual ~D2DOverlay();

    // `clickThrough` makes the overlay ignore hit-testing entirely.
    bool Create(const wchar_t* className, RECT bounds, bool clickThrough);

    // The same surface, but as an ordinary framed window. `classStyle` adds to
    // the window class, e.g. CS_DBLCLKS.
    bool CreateStyled(const wchar_t* className, const wchar_t* title, RECT bounds,
                      DWORD style, DWORD exStyle, UINT classStyle = 0);

    void Destroy();

    void SetBounds(const RECT& bounds);
    void Show();
    void Hide();

    // Draws now. Presents wait for the display, so a burst of these is paced
    // to its refresh; for anything driven by input or frames, use Invalidate.
    void Render();
    // Draws once the queue is empty, however many times it is asked.
    void Invalidate();

    UINT Width()  const { return comp_.Width(); }
    UINT Height() const { return comp_.Height(); }

    virtual LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);

protected:
    // Runs before Render takes the device lock. Anything that needs syscalls
    // or other locks belongs here, so OnDraw touches nothing shared.
    virtual void PrepareDraw() {}
    // Must cover every pixel: the back buffer holds an older frame.
    virtual void OnDraw(ID2D1DeviceContext* dc) = 0;

    // Rounded pill with a label, used for hints and size read-outs. `alignX`
    // is 0 for left, 1 for centre, 2 for right; the same for `alignY`.
    D2D1_SIZE_F DrawChip(ID2D1DeviceContext* dc, std::wstring_view text,
                         float x, float y, int alignX, int alignY,
                         float bgAlpha = 0.92f);

    // Text clipped to `rect`; layouts are cached.
    void DrawLabel(ID2D1DeviceContext* dc, std::wstring_view text, const D2D1_RECT_F& rect,
                   IDWriteTextFormat* format, const D2D1_COLOR_F& color,
                   DWRITE_TEXT_ALIGNMENT align);

    void ResizeSurface(UINT width, UINT height);

    // Re-reads the window's DPI for chips; call on WM_DPICHANGED.
    bool UpdateChipScale();
    float chipScale_ = 1.0f;

    winrt::com_ptr<ID2D1DeviceContext> dc_;
    winrt::com_ptr<IDWriteTextFormat>  font_;

    // Recoloured per use rather than made per draw.
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;

    CompSurface comp_;
    RECT bounds_{};

private:
    struct CachedText {
        std::wstring text;
        winrt::com_ptr<IDWriteTextFormat> format;   // Held so its address stays unique.
        int w = 0, h = 0, align = 0;                 // Box in 1/16 px.
        winrt::com_ptr<IDWriteTextLayout> layout;
        D2D1_SIZE_F size{};
        uint64_t usedFrame = 0;
    };
    const CachedText* Layout(std::wstring_view text, IDWriteTextFormat* format,
                             float width, float height, DWRITE_TEXT_ALIGNMENT align);

    bool EnsureTarget();
    void DropTarget();

    winrt::com_ptr<ID2D1Bitmap1> targetBitmap_;
    std::unordered_map<size_t, CachedText> texts_;
    uint64_t frame_ = 0;
    bool invalidated_ = false;
};

}  // namespace rvm
