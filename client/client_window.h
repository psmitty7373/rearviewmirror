#pragma once
#include "client_config.h"
#include "connect_dialog.h"
#include "overlay.h"
#include "popout_window.h"
#include "stream_client.h"

#include <memory>

namespace rvm {

constexpr wchar_t kClientClass[] = L"RvmClientWindow";

// The client's one window: a sidebar listing each connected server and its
// mirrors, and a canvas where the chosen mirrors sit as free-form boxes,
// each one movable and resizable, or popped out into its own window. The
// sidebar collapses to a thin handle. Custom-drawn in Direct2D.
class ClientWindow : public D2DOverlay {
public:
    // relaunched: this process replaced one whose graphics device was lost.
    bool Create(bool relaunched = false);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;

    // Frees pop-outs retired during the last message. Call from the outermost
    // message loop only, never from inside a nested one.
    void FreeRetired();

protected:
    void PrepareDraw() override;
    void OnDraw(ID2D1DeviceContext* dc) override;

private:
    enum class Part { None, AddServer, SidebarToggle, ServerRow, RemoveServer, MirrorItem, Tile, Canvas };
    enum Edge : int { kEdgeLeft = 1, kEdgeRight = 2, kEdgeTop = 4, kEdgeBottom = 8 };

    struct TileKey {
        uint32_t server = 0;
        uint32_t id = 0;
        bool operator==(const TileKey& o) const { return server == o.server && id == o.id; }
        bool operator!=(const TileKey& o) const { return !(*this == o); }
    };
    struct Hit {
        Part     part = Part::None;
        uint32_t server = 0;
        uint32_t id = 0;
        int      edges = 0;   // Resize handles under the cursor, for a Tile.
        bool operator==(const Hit& o) const {
            return part == o.part && server == o.server && id == o.id && edges == o.edges;
        }
        TileKey Key() const { return { server, id }; }
    };
    struct Server {
        uint32_t        tag = 0;
        ConnectSettings settings;
        std::wstring    label;
        std::unique_ptr<StreamClient> client;
    };
    // Snapshot of one server for OnDraw, built in PrepareDraw.
    struct ServerView {
        uint32_t     tag = 0;
        std::wstring label;
        std::wstring status;
        bool         connected = false;
        int          rttMs = -1;
        std::vector<RemoteMirror> mirrors;
        std::vector<StreamView>   views;
    };
    struct Row {
        Part        part;
        uint32_t    server;
        uint32_t    id;
        D2D1_RECT_F rect;
    };
    // A box on the canvas, in device-independent pixels from the canvas's
    // top-left, so a layout keeps its size across DPI changes. tiles_ is in
    // back-to-front order.
    struct Tile {
        TileKey key;
        float x = 0, y = 0, w = 320, h = 180;
        std::unique_ptr<PopoutWindow> popout;   // Set while popped out.
        PopoutWindow::Settings popSettings;     // Remembered while docked.
        uint64_t chipSince = 0;   // When the name chip last had a reason to show.
    };
    struct Drag {
        bool    active = false;
        bool    moved = false;
        TileKey key;
        int     edges = 0;
        POINT   start{};
        float   x0 = 0, y0 = 0, w0 = 0, h0 = 0;
        std::vector<float> guideX, guideY;   // Edges snapped to, for drawing.
    };

    void  EnsureFonts();
    float S(float v) const { return v * dpiScale_; }

    // Servers.
    void AddServerFlow();
    void AddServer(const ConnectSettings& settings);
    void RemoveServer(uint32_t tag);
    Server* FindServer(uint32_t tag);
    const ServerView* FindView(uint32_t tag) const;
    std::wstring LabelOf(uint32_t tag) const;
    void SaveConfig() const;
    void SetSidebarHidden(bool hidden);

    // Tiles.
    Tile* FindTile(TileKey key);
    void  BringToFront(TileKey key);
    void  PlaceTile(Tile& tile);
    void  AddTile(TileKey key);
    void  RemoveTile(TileKey key, bool remember);
    void  MirrorsChanged(Server& server);
    TileLayout ToLayout(const Tile& tile) const;
    void  PopOut(Tile& tile);
    void  Dock(Tile& tile);
    void  Retire(std::unique_ptr<PopoutWindow> popout);
    void  FeedPopouts(uint32_t serverTag);
    void  FitToStream(Tile& tile);
    // The size to present a stream at: the decoded picture, reshaped to the
    // mirror's true proportions where the encoder had to pad or squeeze it.
    // Falls back to the listed crop before the first frame. False if neither.
    bool  ShownSize(TileKey key, UINT& w, UINT& h) const;
    void  ShowTileMenu(TileKey key, POINT screenPt);
    const StreamView*   ViewFor(TileKey key) const;
    const RemoteMirror* MirrorFor(TileKey key) const;
    // A box's key travels in an LPARAM: both halves need a 64-bit build.
    static_assert(sizeof(LPARAM) == 8, "TokenOf packs two 32-bit ids into an LPARAM");
    static LPARAM TokenOf(TileKey key) {
        return static_cast<LPARAM>((static_cast<uint64_t>(key.server) << 32) | key.id);
    }
    static TileKey KeyOf(LPARAM token) {
        return { static_cast<uint32_t>(static_cast<uint64_t>(token) >> 32),
                 static_cast<uint32_t>(static_cast<uint64_t>(token) & 0xFFFFFFFFu) };
    }

    // Layout.
    float SidebarW() const;
    D2D1_RECT_F SidebarRect() const;
    D2D1_RECT_F AddServerRect() const;
    D2D1_RECT_F ToggleRect() const;
    D2D1_RECT_F CanvasRect() const;
    // The canvas's size in device-independent pixels.
    float CanvasW() const;
    float CanvasH() const;
    // Where a box is drawn: its saved place, pulled in (and shrunk only if it
    // must) when the canvas is too small for it. The saved place itself is
    // never changed by a window resize, so growing the window puts it back.
    struct Box { float x, y, w, h; };
    Box Shown(const Tile& tile) const;
    // The nearest canvas or box edge within snapping range of `edge`, or
    // `edge` itself. `vertical` edges are x positions.
    float Snap(float edge, bool vertical, const TileKey& self, std::vector<float>& guides) const;
    D2D1_RECT_F TileRect(const Tile& tile) const;
    float SidebarContentHeight() const;
    void  ClampScroll();
    void  BuildRows();
    Hit   HitTest(POINT pt) const;
    void  UpdateDrag(POINT pt);
    void  EndDrag(bool commit);

    // Drawing.
    void DrawLabel(ID2D1DeviceContext* dc, const std::wstring& text, const D2D1_RECT_F& rect,
                   IDWriteTextFormat* font, const D2D1_COLOR_F& color,
                   DWRITE_TEXT_ALIGNMENT align);
    void DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& text,
                    bool hot);
    void DrawTile(ID2D1DeviceContext* dc, const Tile& tile, const D2D1_RECT_F& cell);

    // Name chips show while their box is hovered or dragged, and for a few
    // seconds after, then fade. A timer keeps the fade going on a still stream.
    void  SetHot(const Hit& hit);
    float ChipAlpha(const Tile& tile) const;
    void  ScheduleChipTimer();
    uint64_t drawNowMs_ = 0;   // Frozen per frame in PrepareDraw.
    ID2D1Bitmap1* BitmapFor(ID2D1DeviceContext* dc, ID3D11Texture2D* texture);

    std::vector<std::unique_ptr<Server>> servers_;
    uint32_t nextTag_ = 1;
    bool     sidebarHidden_ = false;
    float    scroll_ = 0.0f;   // Sidebar scroll offset in pixels.

    std::vector<Tile>       tiles_;
    std::vector<std::unique_ptr<PopoutWindow>> retiredPopouts_;   // See Retire().
    std::vector<TileLayout> pendingTiles_;   // Saved boxes whose mirror is not listed yet.
    TileKey focused_{};                      // Double-clicked box shown alone, or server 0.
    Drag    drag_;

    // Snapshot for OnDraw and hit testing, built in PrepareDraw.
    std::vector<ServerView> views_;
    std::vector<Row>        rows_;

    std::map<ID3D11Texture2D*, winrt::com_ptr<ID2D1Bitmap1>> bitmaps_;

    winrt::com_ptr<IDWriteTextFormat> titleFont_;
    winrt::com_ptr<IDWriteTextFormat> bodyFont_;
    winrt::com_ptr<IDWriteTextFormat> smallFont_;

    bool restoringPlacement_ = false;
    bool relaunched_ = false;
    bool deviceLostHandled_ = false;
    uint64_t startedMs_ = 0;
    void OnDeviceLost();
    float dpiScale_ = 1.0f;
    Hit hot_{};
    Hit pressed_{};
    bool mouseTracked_ = false;
};

}  // namespace rvm
