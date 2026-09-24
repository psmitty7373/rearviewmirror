#include "client_window.h"

namespace rvm {

namespace {

constexpr wchar_t kClientClass[] = L"RvmClientWindow";
constexpr wchar_t kClientTitle[] = L"Rear View Mirror Client";

constexpr float kPad      = 12.0f;
constexpr float kSidebarW = 240.0f;
constexpr float kHandleW  = 18.0f;
constexpr float kHeaderH  = 48.0f;
constexpr float kServerH  = 46.0f;
constexpr float kItemH    = 44.0f;
constexpr float kRowGap   = 6.0f;
constexpr float kButtonH  = 26.0f;
constexpr float kTileGap  = 6.0f;
constexpr float kHandlePx = 9.0f;    // Edge band that resizes rather than moves.
constexpr float kSnapPx   = 8.0f;    // Edges this close to another edge line up with it.

constexpr uint64_t kChipHoldMs = 2500;   // Name chip stays this long after the mouse leaves,
constexpr uint64_t kChipFadeMs = 500;    // then fades out over this.
constexpr UINT_PTR kChipTimer  = 2;

const D2D1_COLOR_F kBg      = { 0.067f, 0.075f, 0.094f, 1.0f };
const D2D1_COLOR_F kPanel   = { 0.086f, 0.098f, 0.122f, 1.0f };
const D2D1_COLOR_F kItem    = { 0.102f, 0.114f, 0.141f, 1.0f };
const D2D1_COLOR_F kItemHot = { 0.129f, 0.145f, 0.180f, 1.0f };
const D2D1_COLOR_F kStroke  = { 1.0f, 1.0f, 1.0f, 0.07f };
const D2D1_COLOR_F kText    = { 0.910f, 0.918f, 0.941f, 1.0f };
const D2D1_COLOR_F kDim     = { 0.541f, 0.565f, 0.627f, 1.0f };
const D2D1_COLOR_F kAccent  = { kAccentR, kAccentG, kAccentB, 1.0f };
const D2D1_COLOR_F kAmber   = { 0.95f, 0.66f, 0.25f, 1.0f };
const D2D1_COLOR_F kDanger  = { 0.95f, 0.40f, 0.40f, 1.0f };
const D2D1_COLOR_F kTileBg  = { 0.0f, 0.0f, 0.0f, 1.0f };

enum TileMenuId : UINT {
    kMenuPopOut = 1, kMenuReturn, kMenuClickThrough, kMenuFront, kMenuBack, kMenuFit, kMenuRemove,
};

bool Contains(const D2D1_RECT_F& r, POINT p) {
    const float x = static_cast<float>(p.x), y = static_cast<float>(p.y);
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool Overlaps(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

}  // namespace

bool ClientWindow::Create() {
    POINT cursor{};
    GetCursorPos(&cursor);
    const RECT work = WorkAreaFor(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY));

    dpiScale_ = static_cast<float>(GetDpiForSystem()) / 96.0f;
    const int w = (std::min)(static_cast<int>(S(1100.0f)), RectW(work) - 40);
    const int h = (std::min)(static_cast<int>(S(700.0f)), RectH(work) - 40);
    RECT bounds{ (work.left + work.right) / 2 - w / 2, (work.top + work.bottom) / 2 - h / 2, 0, 0 };
    bounds.right = bounds.left + w;
    bounds.bottom = bounds.top + h;

    if (!CreateStyled(kClientClass, kClientTitle, bounds, WS_OVERLAPPEDWINDOW,
                      WS_EX_NOREDIRECTIONBITMAP)) {
        return false;
    }
    if (HICON bigIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON))) {
        SendMessageW(Hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    }
    if (HICON smallIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON))) {
        SendMessageW(Hwnd(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }

    dpiScale_ = static_cast<float>(GetDpiForWindow(Hwnd())) / 96.0f;
    ApplyTitleBarTheme(Hwnd());
    EnsureFonts();

    ClientConfig config = LoadClientConfig();
    sidebarHidden_ = config.sidebarHidden;
    pendingTiles_ = std::move(config.tiles);   // Adopted as each server's list arrives.
    for (const auto& s : config.servers) AddServer(s);

    if (RectW(config.window) > 0 && RectH(config.window) > 0) {
        // Where it was last time. SetWindowPlacement moves a rect that is no
        // longer on any monitor back onto one.
        WINDOWPLACEMENT wp{ sizeof(wp) };
        wp.rcNormalPosition = config.window;
        wp.showCmd = config.windowMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(Hwnd(), &wp);
    } else {
        ShowWindow(Hwnd(), SW_SHOW);
    }
    Render();

    // Nothing remembered: straight into adding the first server.
    if (servers_.empty()) PostMessageW(Hwnd(), WM_COMMAND, 1, 0);
    return true;
}

void ClientWindow::EnsureFonts() {
    auto& g = Gfx::Get();
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, winrt::com_ptr<IDWriteTextFormat>& out) {
        out = nullptr;
        if (FAILED(g.dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              S(size), L"en-us", out.put()))) {
            return;
        }
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    make(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFont_);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL,    bodyFont_);
    make(11.0f, DWRITE_FONT_WEIGHT_NORMAL,    smallFont_);
}

// ---------------------------------------------------------------------------
// Servers

void ClientWindow::AddServerFlow() {
    if (servers_.size() >= kMaxSavedServers) {
        MessageBoxW(Hwnd(), L"That is as many servers as the client keeps.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    ConnectSettings s;
    if (!ShowConnectDialog(Hwnd(), s)) return;

    for (const auto& existing : servers_) {
        if (existing->settings.host == s.host && existing->settings.port == s.port) {
            MessageBoxW(Hwnd(), (existing->label + L" is already in the list.").c_str(), kAppName,
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    AddServer(s);
    SaveConfig();
    Render();
}

void ClientWindow::AddServer(const ConnectSettings& settings) {
    auto server = std::make_unique<Server>();
    server->tag = nextTag_++;
    server->settings = settings;
    server->label = settings.host + L":" + std::to_wstring(settings.port);
    server->client = std::make_unique<StreamClient>();
    server->client->Connect(settings.host, settings.port, settings.key, Hwnd(), server->tag);
    servers_.push_back(std::move(server));
}

void ClientWindow::RemoveServer(uint32_t tag) {
    auto it = std::find_if(servers_.begin(), servers_.end(),
                           [&](const auto& s) { return s->tag == tag; });
    if (it == servers_.end()) return;
    const std::wstring label = (*it)->label;

    tiles_.erase(std::remove_if(tiles_.begin(), tiles_.end(),
                                [&](const Tile& t) { return t.key.server == tag; }),
                 tiles_.end());
    pendingTiles_.erase(std::remove_if(pendingTiles_.begin(), pendingTiles_.end(),
                                       [&](const TileLayout& t) { return t.server == label; }),
                        pendingTiles_.end());
    if (focused_.server == tag) focused_ = {};
    servers_.erase(it);   // Disconnects and joins its threads.
    SaveConfig();
}

ClientWindow::Server* ClientWindow::FindServer(uint32_t tag) {
    for (auto& s : servers_) {
        if (s->tag == tag) return s.get();
    }
    return nullptr;
}

const ClientWindow::ServerView* ClientWindow::FindView(uint32_t tag) const {
    for (const auto& v : views_) {
        if (v.tag == tag) return &v;
    }
    return nullptr;
}

std::wstring ClientWindow::LabelOf(uint32_t tag) const {
    for (const auto& s : servers_) {
        if (s->tag == tag) return s->label;
    }
    return {};
}

void ClientWindow::SaveConfig() const {
    ClientConfig config;
    for (const auto& s : servers_) config.servers.push_back(s->settings);
    config.sidebarHidden = sidebarHidden_;
    for (const auto& t : tiles_) config.tiles.push_back(ToLayout(t));
    for (const auto& p : pendingTiles_) config.tiles.push_back(p);

    // The restored rect, even while maximized or minimized, in the same
    // coordinates SetWindowPlacement takes back at the next launch.
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (Hwnd() && GetWindowPlacement(Hwnd(), &wp)) {
        config.window = wp.rcNormalPosition;
        config.windowMaximized = wp.showCmd == SW_SHOWMAXIMIZED ||
                                 (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    }
    SaveClientConfig(config);
}

void ClientWindow::SetSidebarHidden(bool hidden) {
    if (sidebarHidden_ == hidden) return;
    sidebarHidden_ = hidden;
    SetHot(Hit{});
    SaveConfig();
    Render();
}

// ---------------------------------------------------------------------------
// Tiles

ClientWindow::Tile* ClientWindow::FindTile(TileKey key) {
    for (auto& t : tiles_) {
        if (t.key == key) return &t;
    }
    return nullptr;
}

void ClientWindow::BringToFront(TileKey key) {
    auto it = std::find_if(tiles_.begin(), tiles_.end(), [&](const Tile& t) { return t.key == key; });
    if (it == tiles_.end() || std::next(it) == tiles_.end()) return;
    std::rotate(it, std::next(it), tiles_.end());
}

// The first free spot for a box a third of the canvas wide at 16:9, scanning
// rows with a gap between boxes; when the canvas is full, cascade over what
// is there.
void ClientWindow::PlaceTile(Tile& tile) {
    const float cw = CanvasW(), ch = CanvasH();
    tile.w = Clampf(std::round(cw / 3.0f), static_cast<float>(kMinBoxW), (std::max)(cw, 1.0f));
    tile.h = Clampf(std::round(tile.w * 9.0f / 16.0f), static_cast<float>(kMinBoxH), (std::max)(ch, 1.0f));
    const float gap = kTileGap, step = 24.0f;
    for (float y = 0; y + tile.h <= ch; y += step) {
        for (float x = 0; x + tile.w <= cw; x += step) {
            const bool free = std::none_of(tiles_.begin(), tiles_.end(), [&](const Tile& o) {
                if (o.key == tile.key) return false;
                const Box b = Shown(o);
                return Overlaps(x, y, tile.w + gap, tile.h + gap, b.x, b.y, b.w + gap, b.h + gap);
            });
            if (free) {
                tile.x = x;
                tile.y = y;
                return;
            }
        }
    }
    const float offset = static_cast<float>(tiles_.size() % 6) * 24.0f;
    tile.x = Clampf(offset, 0.0f, (std::max)(cw - tile.w, 0.0f));
    tile.y = Clampf(offset, 0.0f, (std::max)(ch - tile.h, 0.0f));
}

// A remembered box for this mirror is adopted, layout and pop-out and all;
// otherwise a new one is placed.
void ClientWindow::AddTile(TileKey key) {
    if (FindTile(key)) return;
    Tile tile;
    tile.key = key;

    const std::wstring label = LabelOf(key.server);
    auto saved = std::find_if(pendingTiles_.begin(), pendingTiles_.end(), [&](const TileLayout& t) {
        return t.server == label && t.mirror == key.id;
    });
    bool popped = false;
    if (saved != pendingTiles_.end()) {
        tile.x = static_cast<float>(saved->x); tile.y = static_cast<float>(saved->y);
        tile.w = static_cast<float>(saved->w); tile.h = static_cast<float>(saved->h);
        tile.popSettings.rect = saved->popRect;
        tile.popSettings.opacity = saved->popOpacity;
        tile.popSettings.clickThrough = saved->popClickThrough;
        tile.popSettings.aspectLocked = saved->popAspectLocked;
        popped = saved->popped;
        pendingTiles_.erase(saved);
    } else {
        PlaceTile(tile);
    }
    tile.chipSince = GetTickCount64();   // Named for a moment on arrival.
    tiles_.push_back(std::move(tile));
    if (popped) PopOut(tiles_.back());
    SaveConfig();
}

void ClientWindow::RemoveTile(TileKey key, bool remember) {
    auto it = std::find_if(tiles_.begin(), tiles_.end(), [&](const Tile& t) { return t.key == key; });
    if (it == tiles_.end()) return;
    if (remember) pendingTiles_.push_back(ToLayout(*it));
    tiles_.erase(it);   // Destroys its pop-out window, if any.
    if (focused_ == key) focused_ = {};
    if (drag_.active && drag_.key == key) EndDrag(false);
    SaveConfig();
}

TileLayout ClientWindow::ToLayout(const Tile& tile) const {
    TileLayout l;
    l.server = LabelOf(tile.key.server);
    l.mirror = tile.key.id;
    l.x = static_cast<int>(std::lround(tile.x)); l.y = static_cast<int>(std::lround(tile.y));
    l.w = static_cast<int>(std::lround(tile.w)); l.h = static_cast<int>(std::lround(tile.h));
    const PopoutWindow::Settings s = tile.popout ? tile.popout->CurrentSettings() : tile.popSettings;
    l.popped = tile.popout != nullptr;
    l.popRect = s.rect;
    l.popOpacity = s.opacity;
    l.popClickThrough = s.clickThrough;
    l.popAspectLocked = s.aspectLocked;
    return l;
}

// The server's list changed. Boxes whose mirror vanished wait as saved
// layouts; saved layouts whose mirror is back become boxes again.
void ClientWindow::MirrorsChanged(Server& server) {
    const auto mirrors = server.client->Mirrors();
    const auto present = [&](uint32_t id) {
        return std::any_of(mirrors.begin(), mirrors.end(),
                           [&](const RemoteMirror& m) { return m.id == id; });
    };

    std::vector<TileKey> gone;
    for (const auto& t : tiles_) {
        if (t.key.server == server.tag && !present(t.key.id)) gone.push_back(t.key);
    }
    for (const TileKey& key : gone) RemoveTile(key, /*remember=*/true);

    std::vector<uint32_t> returning;
    for (const auto& p : pendingTiles_) {
        if (p.server == server.label && present(p.mirror)) returning.push_back(p.mirror);
    }
    for (uint32_t id : returning) {
        server.client->SetSubscribed(id, true);
        AddTile({ server.tag, id });
    }
}

const StreamView* ClientWindow::ViewFor(TileKey key) const {
    const ServerView* sv = FindView(key.server);
    if (!sv) return nullptr;
    for (const auto& v : sv->views) {
        if (v.id == key.id) return &v;
    }
    return nullptr;
}

const RemoteMirror* ClientWindow::MirrorFor(TileKey key) const {
    const ServerView* sv = FindView(key.server);
    if (!sv) return nullptr;
    for (const auto& m : sv->mirrors) {
        if (m.id == key.id) return &m;
    }
    return nullptr;
}

void ClientWindow::PopOut(Tile& tile) {
    if (tile.popout) return;
    UINT nativeW = 0, nativeH = 0;
    if (const StreamView* v = ViewFor(tile.key); v && v->frames > 0) {
        nativeW = v->width;
        nativeH = v->height;
    } else if (const RemoteMirror* m = MirrorFor(tile.key)) {
        nativeW = m->width;
        nativeH = m->height;
    }
    if (nativeW == 0 || nativeH == 0) {
        nativeW = 640;
        nativeH = 360;
    }

    auto popout = std::make_unique<PopoutWindow>();
    if (!popout->Create(Hwnd(), TokenOf(tile.key), tile.popSettings, nativeW, nativeH, Hwnd())) return;
    if (const StreamView* v = ViewFor(tile.key)) {
        popout->SetFrame(v->texture, v->width, v->height, v->frames);
        popout->Render();
    }
    tile.popout = std::move(popout);
    if (focused_ == tile.key) focused_ = {};
    SaveConfig();
}

void ClientWindow::Dock(Tile& tile) {
    if (!tile.popout) return;
    tile.popSettings = tile.popout->CurrentSettings();
    tile.popout.reset();
    SaveConfig();
}

void ClientWindow::FeedPopouts(uint32_t serverTag) {
    for (auto& t : tiles_) {
        if (!t.popout || t.key.server != serverTag) continue;
        if (const StreamView* v = ViewFor(t.key)) {
            t.popout->SetFrame(v->texture, v->width, v->height, v->frames);
            t.popout->Render();
        }
    }
}

// Size the box so the stream shows at 100%, as far as the canvas allows.
void ClientWindow::FitToStream(Tile& tile) {
    UINT w = 0, h = 0;
    if (const StreamView* v = ViewFor(tile.key); v && v->frames > 0) { w = v->width; h = v->height; }
    else if (const RemoteMirror* m = MirrorFor(tile.key))            { w = m->width; h = m->height; }
    if (w == 0 || h == 0) return;

    // One stream pixel per screen pixel, scaled down evenly if the canvas is
    // smaller than that.
    const float cw = CanvasW(), ch = CanvasH();
    const Box seen = Shown(tile);
    tile.x = seen.x;   // Grow from where the box is seen, not a place off-canvas.
    tile.y = seen.y;
    float bw = w / dpiScale_, bh = h / dpiScale_;
    const float fit = (std::min)(1.0f, (std::min)(cw / bw, ch / bh));
    bw *= fit;
    bh *= fit;
    tile.w = (std::max)(bw, static_cast<float>(kMinBoxW));
    tile.h = (std::max)(bh, static_cast<float>(kMinBoxH));
    tile.x = Clampf(tile.x, 0.0f, (std::max)(cw - tile.w, 0.0f));
    tile.y = Clampf(tile.y, 0.0f, (std::max)(ch - tile.h, 0.0f));
}

void ClientWindow::ShowTileMenu(TileKey key, POINT screenPt) {
    Tile* tile = FindTile(key);
    if (!tile) return;
    const bool popped = tile->popout != nullptr;

    HMENU menu = CreatePopupMenu();
    if (popped) {
        AppendMenuW(menu, MF_STRING, kMenuReturn, L"Return to grid");
        AppendMenuW(menu, MF_STRING | (tile->popout->ClickThrough() ? MF_CHECKED : 0),
                    kMenuClickThrough, L"Click-through");
    } else {
        AppendMenuW(menu, MF_STRING, kMenuPopOut, L"Pop out");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuFit, L"Size to stream (100%)");
    AppendMenuW(menu, MF_STRING, kMenuFront, L"Bring to front");
    AppendMenuW(menu, MF_STRING, kMenuBack, L"Send to back");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRemove, L"Remove from the canvas");

    SetForegroundWindow(Hwnd());
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, Hwnd(), nullptr));
    PostMessageW(Hwnd(), WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (!Hwnd()) return;
    tile = FindTile(key);   // The list may have changed under the menu.
    if (!tile) return;

    switch (cmd) {
    case kMenuPopOut: PopOut(*tile); break;
    case kMenuReturn: Dock(*tile); break;
    case kMenuClickThrough:
        if (tile->popout) tile->popout->SetClickThrough(!tile->popout->ClickThrough());
        break;
    case kMenuFit:    FitToStream(*tile); SaveConfig(); break;
    case kMenuFront:  BringToFront(key); SaveConfig(); break;
    case kMenuBack: {
        auto it = std::find_if(tiles_.begin(), tiles_.end(), [&](const Tile& t) { return t.key == key; });
        if (it != tiles_.end()) std::rotate(tiles_.begin(), it, std::next(it));
        SaveConfig();
        break;
    }
    case kMenuRemove:
        if (Server* s = FindServer(key.server)) s->client->SetSubscribed(key.id, false);
        RemoveTile(key, /*remember=*/false);
        break;
    default:
        break;
    }
    SetHot(Hit{});
    Render();
}

// ---------------------------------------------------------------------------
// Layout

float ClientWindow::SidebarW() const {
    return S(sidebarHidden_ ? kHandleW : kSidebarW);
}

D2D1_RECT_F ClientWindow::SidebarRect() const {
    return D2D1::RectF(0, 0, SidebarW(), static_cast<float>(Height()));
}

D2D1_RECT_F ClientWindow::AddServerRect() const {
    const float cy = S(kHeaderH) * 0.5f;
    return D2D1::RectF(S(kPad), cy - S(kButtonH) * 0.5f,
                       S(kSidebarW) - S(kPad) - S(kButtonH) - S(6.0f), cy + S(kButtonH) * 0.5f);
}

D2D1_RECT_F ClientWindow::ToggleRect() const {
    if (sidebarHidden_) return SidebarRect();
    const float cy = S(kHeaderH) * 0.5f;
    return D2D1::RectF(S(kSidebarW) - S(kPad) - S(kButtonH), cy - S(kButtonH) * 0.5f,
                       S(kSidebarW) - S(kPad), cy + S(kButtonH) * 0.5f);
}

D2D1_RECT_F ClientWindow::CanvasRect() const {
    return D2D1::RectF(SidebarW() + S(kTileGap), S(kTileGap),
                       static_cast<float>(Width()) - S(kTileGap),
                       static_cast<float>(Height()) - S(kTileGap));
}

float ClientWindow::CanvasW() const {
    const D2D1_RECT_F c = CanvasRect();
    return (std::max)(c.right - c.left, 1.0f) / dpiScale_;
}

float ClientWindow::CanvasH() const {
    const D2D1_RECT_F c = CanvasRect();
    return (std::max)(c.bottom - c.top, 1.0f) / dpiScale_;
}

ClientWindow::Box ClientWindow::Shown(const Tile& tile) const {
    const float cw = CanvasW(), ch = CanvasH();
    Box b;
    b.w = Clampf(tile.w, (std::min)(static_cast<float>(kMinBoxW), cw), cw);
    b.h = Clampf(tile.h, (std::min)(static_cast<float>(kMinBoxH), ch), ch);
    b.x = Clampf(tile.x, 0.0f, cw - b.w);
    b.y = Clampf(tile.y, 0.0f, ch - b.h);
    return b;
}

D2D1_RECT_F ClientWindow::TileRect(const Tile& tile) const {
    const D2D1_RECT_F c = CanvasRect();
    if (focused_ == tile.key) return c;
    const Box b = Shown(tile);
    return D2D1::RectF(c.left + S(b.x), c.top + S(b.y), c.left + S(b.x + b.w), c.top + S(b.y + b.h));
}

// Candidates are the canvas edges and every other box's edges, plus those
// edges offset by the usual gap, so boxes can butt up neatly or align flush.
float ClientWindow::Snap(float edge, bool vertical, const TileKey& self,
                         std::vector<float>& guides) const {
    if (GetKeyState(VK_MENU) & 0x8000) return edge;   // Alt: place freely.

    const float range = kSnapPx;
    float best = edge, bestDist = range + 1.0f, guide = 0.0f;
    const auto consider = [&](float target, float line) {
        const float d = std::fabs(target - edge);
        if (d <= range && d < bestDist) {
            best = target;
            bestDist = d;
            guide = line;
        }
    };
    consider(0.0f, 0.0f);
    consider(vertical ? CanvasW() : CanvasH(), vertical ? CanvasW() : CanvasH());
    for (const auto& o : tiles_) {
        if (o.key == self || o.popout) continue;
        const Box b = Shown(o);   // Line up with where boxes are seen.
        const float lo = vertical ? b.x : b.y;
        const float hi = lo + (vertical ? b.w : b.h);
        consider(lo, lo);
        consider(hi, hi);
        consider(lo - kTileGap, lo);
        consider(hi + kTileGap, hi);
    }
    if (bestDist <= range) guides.push_back(guide);
    return best;
}

float ClientWindow::SidebarContentHeight() const {
    float h = S(kPad);
    for (const auto& v : views_) {
        h += S(kServerH) + S(kRowGap) + v.mirrors.size() * (S(kItemH) + S(kRowGap)) + S(kPad);
    }
    return h;
}

void ClientWindow::ClampScroll() {
    const float visible = static_cast<float>(Height()) - S(kHeaderH);
    const float maxScroll = (std::max)(0.0f, SidebarContentHeight() - visible);
    scroll_ = Clampf(scroll_, 0.0f, maxScroll);
}

// One rectangle per sidebar row, already scrolled: what OnDraw paints and
// HitTest checks.
void ClientWindow::BuildRows() {
    rows_.clear();
    ClampScroll();
    const float left = S(kPad), right = S(kSidebarW) - S(kPad);
    float y = S(kHeaderH) + S(kPad) - scroll_;
    for (const auto& v : views_) {
        rows_.push_back({ Part::ServerRow, v.tag, 0, D2D1::RectF(left, y, right, y + S(kServerH)) });
        rows_.push_back({ Part::RemoveServer, v.tag, 0,
                          D2D1::RectF(right - S(26.0f), y + S(2.0f), right - S(2.0f), y + S(26.0f)) });
        y += S(kServerH) + S(kRowGap);
        for (const auto& m : v.mirrors) {
            rows_.push_back({ Part::MirrorItem, v.tag, m.id,
                              D2D1::RectF(left + S(10.0f), y, right, y + S(kItemH)) });
            y += S(kItemH) + S(kRowGap);
        }
        y += S(kPad);
    }
}

ClientWindow::Hit ClientWindow::HitTest(POINT pt) const {
    Hit hit;
    if (Contains(ToggleRect(), pt)) {
        hit.part = Part::SidebarToggle;
        return hit;
    }
    if (!sidebarHidden_ && Contains(SidebarRect(), pt)) {
        if (Contains(AddServerRect(), pt)) {
            hit.part = Part::AddServer;
            return hit;
        }
        if (pt.y < S(kHeaderH)) return hit;
        for (const auto& r : rows_) {
            if ((r.part == Part::RemoveServer || r.part == Part::MirrorItem) && Contains(r.rect, pt)) {
                hit.part = r.part;
                hit.server = r.server;
                hit.id = r.id;
                return hit;
            }
        }
        return hit;
    }
    if (!Contains(CanvasRect(), pt)) return hit;

    hit.part = Part::Canvas;
    if (focused_.server != 0) {
        hit.part = Part::Tile;
        hit.server = focused_.server;
        hit.id = focused_.id;
        return hit;
    }
    // Front-most first.
    for (auto it = tiles_.rbegin(); it != tiles_.rend(); ++it) {
        const D2D1_RECT_F r = TileRect(*it);
        if (!Contains(r, pt)) continue;
        hit.part = Part::Tile;
        hit.server = it->key.server;
        hit.id = it->key.id;
        const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
        const float band = (std::min)(S(kHandlePx), (std::min)(r.right - r.left, r.bottom - r.top) * 0.25f);
        if (x < r.left + band)   hit.edges |= kEdgeLeft;
        if (x >= r.right - band) hit.edges |= kEdgeRight;
        if (y < r.top + band)    hit.edges |= kEdgeTop;
        if (y >= r.bottom - band) hit.edges |= kEdgeBottom;
        return hit;
    }
    return hit;
}

void ClientWindow::UpdateDrag(POINT pt) {
    Tile* t = FindTile(drag_.key);
    if (!t) {
        EndDrag(false);
        return;
    }
    // Windows sends a move right after a press even when nothing moved; that
    // must not turn a pulled-in box's drawn place into its saved one.
    if (!drag_.moved && pt.x == drag_.start.x && pt.y == drag_.start.y) return;
    const float cw = CanvasW(), ch = CanvasH();
    const float dx = (pt.x - drag_.start.x) / dpiScale_;
    const float dy = (pt.y - drag_.start.y) / dpiScale_;
    const float minW = static_cast<float>(kMinBoxW), minH = static_cast<float>(kMinBoxH);
    drag_.guideX.clear();
    drag_.guideY.clear();

    float l = drag_.x0, tp = drag_.y0, r = drag_.x0 + drag_.w0, b = drag_.y0 + drag_.h0;
    if (drag_.edges == 0) {
        // Moving: whichever edge is nearer a snap target wins, on each axis.
        l = Clampf(drag_.x0 + dx, 0.0f, (std::max)(cw - drag_.w0, 0.0f));
        tp = Clampf(drag_.y0 + dy, 0.0f, (std::max)(ch - drag_.h0, 0.0f));
        std::vector<float> gl, gr, gt, gb;
        const float sl = Snap(l, true, drag_.key, gl);
        const float sr = Snap(l + drag_.w0, true, drag_.key, gr) - drag_.w0;
        if (!gl.empty() && (gr.empty() || std::fabs(sl - l) <= std::fabs(sr - l))) {
            l = sl;
            drag_.guideX = gl;
        } else if (!gr.empty()) {
            l = sr;
            drag_.guideX = gr;
        }
        const float st = Snap(tp, false, drag_.key, gt);
        const float sb = Snap(tp + drag_.h0, false, drag_.key, gb) - drag_.h0;
        if (!gt.empty() && (gb.empty() || std::fabs(st - tp) <= std::fabs(sb - tp))) {
            tp = st;
            drag_.guideY = gt;
        } else if (!gb.empty()) {
            tp = sb;
            drag_.guideY = gb;
        }
        l = Clampf(l, 0.0f, (std::max)(cw - drag_.w0, 0.0f));
        tp = Clampf(tp, 0.0f, (std::max)(ch - drag_.h0, 0.0f));
        r = l + drag_.w0;
        b = tp + drag_.h0;
    } else {
        // Resizing: only the edges being dragged move, each snapping on its own.
        if (drag_.edges & kEdgeLeft)   l  = Clampf(Snap(l + dx, true, drag_.key, drag_.guideX), 0.0f, r - minW);
        if (drag_.edges & kEdgeRight)  r  = Clampf(Snap(r + dx, true, drag_.key, drag_.guideX), l + minW, cw);
        if (drag_.edges & kEdgeTop)    tp = Clampf(Snap(tp + dy, false, drag_.key, drag_.guideY), 0.0f, b - minH);
        if (drag_.edges & kEdgeBottom) b  = Clampf(Snap(b + dy, false, drag_.key, drag_.guideY), tp + minH, ch);
    }
    if (l != t->x || tp != t->y || r - l != t->w || b - tp != t->h) {
        t->x = l;
        t->y = tp;
        t->w = r - l;
        t->h = b - tp;
        drag_.moved = true;
    }
    Render();   // Guides may have changed even if the box did not.
}

void ClientWindow::EndDrag(bool commit) {
    if (!drag_.active) return;
    drag_.active = false;
    if (GetCapture() == Hwnd()) ReleaseCapture();
    if (Tile* t = FindTile(drag_.key)) t->chipSince = GetTickCount64();
    if (commit && drag_.moved) SaveConfig();
    drag_.moved = false;
}

// A box the mouse leaves starts its chip's countdown from now.
void ClientWindow::SetHot(const Hit& hit) {
    if (hot_.part == Part::Tile && !(hit.part == Part::Tile && hit.Key() == hot_.Key())) {
        if (Tile* t = FindTile(hot_.Key())) t->chipSince = GetTickCount64();
    }
    hot_ = hit;
}

float ClientWindow::ChipAlpha(const Tile& tile) const {
    const bool hovered = hot_.part == Part::Tile && hot_.Key() == tile.key;
    const bool dragged = drag_.active && drag_.key == tile.key;
    if (hovered || dragged) return 1.0f;
    const uint64_t age = drawNowMs_ - tile.chipSince;
    if (age < kChipHoldMs) return 1.0f;
    if (age >= kChipHoldMs + kChipFadeMs) return 0.0f;
    return 1.0f - static_cast<float>(age - kChipHoldMs) / static_cast<float>(kChipFadeMs);
}

// Wakes the window when the next chip starts to fade, then every frame of
// the fade; nothing at all once every chip is hidden or hovered.
void ClientWindow::ScheduleChipTimer() {
    uint64_t wait = UINT64_MAX;
    for (const auto& t : tiles_) {
        if (ChipAlpha(t) <= 0.0f) continue;
        const bool held = (hot_.part == Part::Tile && hot_.Key() == t.key) ||
                          (drag_.active && drag_.key == t.key);
        if (held) continue;
        const uint64_t age = drawNowMs_ - t.chipSince;
        wait = (std::min)(wait, age < kChipHoldMs ? kChipHoldMs - age : 16ull);
    }
    if (wait == UINT64_MAX) KillTimer(Hwnd(), kChipTimer);
    else                    SetTimer(Hwnd(), kChipTimer, static_cast<UINT>((std::max)(wait, 10ull)), nullptr);
}

// ---------------------------------------------------------------------------
// Messages

LRESULT ClientWindow::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == 1) AddServerFlow();
        return 0;

    case WM_RVM_CLIENT_EVENT: {
        Server* server = FindServer(ClientEventTag(wp));
        if (!server) return 0;
        const ClientEvent event = ClientEventOf(wp);
        if (event == ClientEvent::FrameReady) server->client->AckFrameReady();
        if (event == ClientEvent::ListUpdated) MirrorsChanged(*server);
        Render();
        if (event == ClientEvent::FrameReady) FeedPopouts(server->tag);
        return 0;
    }

    case WM_RVM_POPOUT_EVENT: {
        Tile* tile = FindTile(KeyOf(lp));
        if (!tile || !tile->popout) return 0;
        if (static_cast<PopoutEvent>(wp) == PopoutEvent::ReturnToGrid) Dock(*tile);
        else                                                          SaveConfig();
        Render();
        return 0;
    }

    case WM_SIZE: {
        const UINT w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0) {
            ResizeSurface(w, h);
            Render();
        }
        return 0;
    }

    case WM_EXITSIZEMOVE:
        SaveConfig();   // The window's own place and size.
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = static_cast<LONG>(S(640.0f));
        mmi->ptMinTrackSize.y = static_cast<LONG>(S(400.0f));
        return 0;
    }

    case WM_SETTINGCHANGE:
        // Sent with "ImmersiveColorSet" when the Windows theme changes.
        if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
            ApplyTitleBarTheme(Hwnd());
        }
        break;

    case WM_DPICHANGED: {
        dpiScale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
        EnsureFonts();
        const RECT* suggested = reinterpret_cast<RECT*>(lp);
        SetWindowPos(Hwnd(), nullptr, suggested->left, suggested->top,
                     RectW(*suggested), RectH(*suggested), SWP_NOZORDER | SWP_NOACTIVATE);
        Render();
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            int edges = 0;
            if (drag_.active)                edges = drag_.edges;
            else if (hot_.part == Part::Tile) edges = hot_.edges;
            LPCWSTR cursor = IDC_ARROW;
            const bool h = (edges & (kEdgeLeft | kEdgeRight)) != 0;
            const bool v = (edges & (kEdgeTop | kEdgeBottom)) != 0;
            if (h && v) {
                const bool nwse = (edges == (kEdgeLeft | kEdgeTop)) || (edges == (kEdgeRight | kEdgeBottom));
                cursor = nwse ? IDC_SIZENWSE : IDC_SIZENESW;
            } else if (h) {
                cursor = IDC_SIZEWE;
            } else if (v) {
                cursor = IDC_SIZENS;
            } else if (drag_.active || (hot_.part == Part::Tile && focused_.server == 0)) {
                cursor = IDC_SIZEALL;
            }
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!mouseTracked_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, Hwnd(), 0 };
            TrackMouseEvent(&tme);
            mouseTracked_ = true;
        }
        if (drag_.active) {
            UpdateDrag(pt);
            return 0;
        }
        const Hit hit = HitTest(pt);
        if (!(hit == hot_)) {
            SetHot(hit);
            Render();
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kChipTimer) {
            KillTimer(Hwnd(), kChipTimer);
            Render();   // Re-arms itself while a chip is still fading.
        }
        return 0;

    case WM_MOUSELEAVE:
        mouseTracked_ = false;
        SetHot(Hit{});
        Render();
        return 0;

    case WM_CAPTURECHANGED:
        if (drag_.active && reinterpret_cast<HWND>(lp) != Hwnd()) {
            EndDrag(true);
            Render();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(Hwnd(), &pt);
        if (!sidebarHidden_ && Contains(SidebarRect(), pt)) {
            scroll_ -= GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA) * S(60.0f);
            ClampScroll();
            SetHot(Hit{});
            Render();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        pressed_ = HitTest(pt);
        if (pressed_.part == Part::Tile && focused_.server == 0) {
            // Reorder first: BringToFront moves boxes around in tiles_, so a
            // pointer taken before it would name whichever box now sits there.
            BringToFront(pressed_.Key());
            Tile* t = FindTile(pressed_.Key());
            if (!t) return 0;
            drag_ = Drag{};
            drag_.active = true;
            drag_.key = pressed_.Key();
            drag_.edges = pressed_.edges;
            drag_.start = pt;
            // Start from where the box is seen: a box pulled in by a small
            // window moves from there, and only then takes that place.
            const Box seen = Shown(*t);
            drag_.x0 = seen.x; drag_.y0 = seen.y; drag_.w0 = seen.w; drag_.h0 = seen.h;
            SetCapture(Hwnd());
            Render();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (drag_.active) {
            const bool moved = drag_.moved;
            const TileKey key = drag_.key;
            EndDrag(true);
            // A plain click on a popped-out box brings its window back.
            if (!moved) {
                if (Tile* t = FindTile(key); t && t->popout) Dock(*t);
            }
            SetHot(HitTest(pt));
            Render();
            return 0;
        }
        const Hit hit = HitTest(pt);
        const Hit pressed = pressed_;
        pressed_ = Hit{};
        if (!(hit == pressed)) return 0;

        switch (hit.part) {
        case Part::AddServer:
            AddServerFlow();
            break;
        case Part::SidebarToggle:
            SetSidebarHidden(!sidebarHidden_);
            break;
        case Part::RemoveServer: {
            Server* server = FindServer(hit.server);
            if (!server) break;
            const std::wstring question = L"Disconnect from " + server->label + L" and forget it?";
            if (MessageBoxW(Hwnd(), question.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                RemoveServer(hit.server);
            }
            SetHot(Hit{});
            Render();
            break;
        }
        case Part::MirrorItem: {
            Server* server = FindServer(hit.server);
            if (!server) break;
            const bool on = !server->client->IsSubscribed(hit.id);
            server->client->SetSubscribed(hit.id, on);
            if (on) AddTile(hit.Key());
            else    RemoveTile(hit.Key(), /*remember=*/false);
            Render();
            break;
        }
        default:
            break;
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        const Hit hit = HitTest(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (hit.part == Part::Tile) {
            EndDrag(false);
            const Tile* t = FindTile(hit.Key());
            if (t && !t->popout) {
                focused_ = (focused_ == hit.Key()) ? TileKey{} : hit.Key();
                Render();
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const Hit hit = HitTest(pt);
        if (hit.part == Part::Tile) {
            EndDrag(true);
            ClientToScreen(Hwnd(), &pt);
            ShowTileMenu(hit.Key(), pt);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && focused_.server != 0) {
            focused_ = {};
            Render();
        } else if (wp == VK_TAB) {
            SetSidebarHidden(!sidebarHidden_);
        }
        return 0;

    case WM_CLOSE:
        SaveConfig();
        tiles_.clear();     // Pop-out windows first,
        servers_.clear();   // then the connections.
        DestroyWindow(Hwnd());
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        break;
    }
    return D2DOverlay::OnMessage(msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Drawing

void ClientWindow::PrepareDraw() {
    drawNowMs_ = GetTickCount64();
    ScheduleChipTimer();
    views_.clear();
    for (const auto& s : servers_) {
        ServerView v;
        v.tag       = s->tag;
        v.label     = s->label;
        v.status    = s->client->Status();
        v.connected = s->client->Connected();
        v.rttMs     = s->client->RttMs();
        v.mirrors   = s->client->Mirrors();
        v.views     = s->client->Views();
        views_.push_back(std::move(v));
    }
    BuildRows();

    // Bitmaps wrap textures; forget any whose texture is gone.
    for (auto it = bitmaps_.begin(); it != bitmaps_.end();) {
        bool live = false;
        for (const auto& v : views_) {
            live = live || std::any_of(v.views.begin(), v.views.end(), [&](const StreamView& sv) {
                return sv.texture.get() == it->first;
            });
        }
        it = live ? std::next(it) : bitmaps_.erase(it);
    }
}

ID2D1Bitmap1* ClientWindow::BitmapFor(ID2D1DeviceContext* dc, ID3D11Texture2D* texture) {
    auto it = bitmaps_.find(texture);
    if (it != bitmaps_.end()) return it->second.get();

    winrt::com_ptr<IDXGISurface> surface;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(surface.put())))) return nullptr;
    auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put()))) return nullptr;
    bitmaps_[texture] = bitmap;
    return bitmap.get();
}

void ClientWindow::DrawLabel(ID2D1DeviceContext* dc, const std::wstring& text,
                             const D2D1_RECT_F& rect, IDWriteTextFormat* font,
                             const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align) {
    if (text.empty() || !font) return;
    font->SetTextAlignment(align);
    brush_->SetColor(color);
    dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), font, rect, brush_.get(),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void ClientWindow::DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect,
                              const std::wstring& text, bool hot) {
    brush_->SetColor(hot ? D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.20f)
                         : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, S(6.0f), S(6.0f)), brush_.get());
    brush_->SetColor(hot ? D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.75f) : kStroke);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, S(6.0f), S(6.0f)), brush_.get(), S(1.0f));
    DrawLabel(dc, text, rect, bodyFont_.get(), hot ? kAccent : kText, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void ClientWindow::DrawTile(ID2D1DeviceContext* dc, const Tile& tile, const D2D1_RECT_F& cell) {
    const bool hot = (hot_.part == Part::Tile && hot_.Key() == tile.key) ||
                     (drag_.active && drag_.key == tile.key);
    const StreamView* view = ViewFor(tile.key);
    const RemoteMirror* mirror = MirrorFor(tile.key);
    const ServerView* sv = FindView(tile.key.server);

    std::wstring name = mirror ? mirror->name : L"";
    if (servers_.size() > 1 && sv) name = sv->label + L"  ·  " + name;

    if (tile.popout) {
        brush_->SetColor(hot ? kItemHot : kItem);
        dc->FillRoundedRectangle(D2D1::RoundedRect(cell, S(6.0f), S(6.0f)), brush_.get());
        DrawLabel(dc, L"Popped out  ·  click to return", cell, bodyFont_.get(), kDim,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        brush_->SetColor(kTileBg);
        dc->FillRoundedRectangle(D2D1::RoundedRect(cell, S(6.0f), S(6.0f)), brush_.get());

        if (view && view->texture && view->width > 0 && view->height > 0 && view->frames > 0) {
            // Letterbox to the stream's aspect.
            const float cw = cell.right - cell.left, ch = cell.bottom - cell.top;
            const float scale = (std::min)(cw / view->width, ch / view->height);
            const float dw = view->width * scale, dh = view->height * scale;
            const float dx = cell.left + (cw - dw) * 0.5f, dy = cell.top + (ch - dh) * 0.5f;
            if (ID2D1Bitmap1* bitmap = BitmapFor(dc, view->texture.get())) {
                dc->DrawBitmap(bitmap, D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.0f,
                               scale < 1.0f ? D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR
                                            : D2D1_INTERPOLATION_MODE_LINEAR);
            }
        } else {
            const bool connected = sv && sv->connected;
            DrawLabel(dc, connected ? L"Waiting for the first frame…" : L"Reconnecting…", cell,
                      bodyFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    const float chipAlpha = ChipAlpha(tile);
    if (!name.empty() && chipAlpha > 0.0f) {
        const bool fading = chipAlpha < 1.0f;
        if (fading) {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), chipAlpha),
                          nullptr);
        }
        DrawChip(dc, Ellipsize(name, 48), cell.left + S(10.0f), cell.top + S(10.0f), 0, 0, 0.75f);
        if (fading) dc->PopLayer();
    }

    if (hot && focused_.server == 0) {
        brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.9f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(cell, S(6.0f), S(6.0f)), brush_.get(), S(1.5f));
        // Corner ticks: the box can be resized as well as moved.
        const float tick = (std::min)(S(16.0f), (std::min)(cell.right - cell.left, cell.bottom - cell.top) * 0.25f);
        const float th = S(3.0f);
        const float l = cell.left, t = cell.top, r = cell.right, b = cell.bottom;
        const D2D1_RECT_F ticks[] = {
            { l, t, l + tick, t + th }, { l, t, l + th, t + tick },
            { r - tick, t, r, t + th }, { r - th, t, r, t + tick },
            { l, b - th, l + tick, b }, { l, b - tick, l + th, b },
            { r - tick, b - th, r, b }, { r - th, b - tick, r, b },
        };
        for (const auto& tr : ticks) dc->FillRectangle(tr, brush_.get());
    }
}

void ClientWindow::OnDraw(ID2D1DeviceContext* dc) {
    if (!titleFont_) EnsureFonts();
    if (!titleFont_ || !bodyFont_ || !smallFont_) return;

    const float w = static_cast<float>(Width());
    const float h = static_cast<float>(Height());

    brush_->SetColor(kBg);
    dc->FillRectangle(D2D1::RectF(0, 0, w, h), brush_.get());

    // Sidebar, or the handle it collapses to.
    brush_->SetColor(kPanel);
    dc->FillRectangle(SidebarRect(), brush_.get());
    brush_->SetColor(kStroke);
    dc->FillRectangle(D2D1::RectF(SidebarW() - S(1.0f), 0, SidebarW(), h), brush_.get());

    if (sidebarHidden_) {
        const bool hot = hot_.part == Part::SidebarToggle;
        if (hot) {
            brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.15f));
            dc->FillRectangle(SidebarRect(), brush_.get());
        }
        DrawLabel(dc, L"›", SidebarRect(), titleFont_.get(), hot ? kAccent : kDim,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        DrawButton(dc, AddServerRect(), L"Add server…", hot_.part == Part::AddServer);
        DrawButton(dc, ToggleRect(), L"‹", hot_.part == Part::SidebarToggle);

        dc->PushAxisAlignedClip(D2D1::RectF(0, S(kHeaderH), S(kSidebarW), h),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        if (views_.empty()) {
            DrawLabel(dc, L"No servers yet",
                      D2D1::RectF(S(kPad), S(kHeaderH) + S(kPad), S(kSidebarW) - S(kPad),
                                  S(kHeaderH) + S(kPad) + S(22.0f)),
                      smallFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        for (const auto& row : rows_) {
            if (row.rect.bottom < S(kHeaderH) || row.rect.top > h) continue;
            const ServerView* sv = FindView(row.server);
            if (!sv) continue;

            switch (row.part) {
            case Part::ServerRow: {
                const D2D1_RECT_F r = row.rect;
                const float cx = r.left + S(6.0f), cy = r.top + S(13.0f);
                brush_->SetColor(sv->connected ? kAccent : kAmber);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), S(4.0f), S(4.0f)), brush_.get());

                const D2D1_RECT_F title = D2D1::RectF(r.left + S(18.0f), r.top, r.right - S(30.0f),
                                                      r.top + S(26.0f));
                DrawLabel(dc, Ellipsize(sv->label, 26), title, titleFont_.get(), kText,
                          DWRITE_TEXT_ALIGNMENT_LEADING);

                std::wstring status = sv->status;
                if (sv->connected) {
                    status = L"Connected";
                    if (sv->rttMs >= 0) status += L"   ·   " + std::to_wstring(sv->rttMs) + L" ms";
                    if (sv->mirrors.empty()) status += L"   ·   no mirrors";
                }
                DrawLabel(dc, Ellipsize(status, 40),
                          D2D1::RectF(r.left + S(18.0f), r.top + S(24.0f), r.right, r.bottom),
                          smallFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_LEADING);
                break;
            }
            case Part::RemoveServer: {
                const bool hot = hot_.part == Part::RemoveServer && hot_.server == row.server;
                if (hot) {
                    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(row.rect, S(5.0f), S(5.0f)), brush_.get());
                }
                DrawLabel(dc, L"✕", row.rect, smallFont_.get(), hot ? kDanger : kDim,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
                break;
            }
            case Part::MirrorItem: {
                const RemoteMirror* m = nullptr;
                for (const auto& cand : sv->mirrors) {
                    if (cand.id == row.id) { m = &cand; break; }
                }
                if (!m) break;
                const D2D1_RECT_F r = row.rect;
                const TileKey key{ row.server, row.id };
                const bool on = std::any_of(tiles_.begin(), tiles_.end(),
                                            [&](const Tile& t) { return t.key == key; });
                const bool hot = hot_.part == Part::MirrorItem && hot_.server == row.server &&
                                 hot_.id == row.id;

                brush_->SetColor(hot ? kItemHot : kItem);
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, S(8.0f), S(8.0f)), brush_.get());
                brush_->SetColor(on ? D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.9f) : kStroke);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(r, S(8.0f), S(8.0f)), brush_.get(),
                                         on ? S(1.5f) : S(1.0f));

                const float cx = r.left + S(14.0f), cy = (r.top + r.bottom) * 0.5f;
                brush_->SetColor(on ? kAccent : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), S(4.5f), S(4.5f)), brush_.get());

                const D2D1_RECT_F text = D2D1::RectF(r.left + S(28.0f), r.top + S(5.0f),
                                                     r.right - S(8.0f), r.top + S(24.0f));
                DrawLabel(dc, Ellipsize(m->name, 26), text, bodyFont_.get(), on ? kText : kDim,
                          DWRITE_TEXT_ALIGNMENT_LEADING);
                const D2D1_RECT_F sub = D2D1::RectF(text.left, r.top + S(22.0f), text.right,
                                                    r.bottom - S(4.0f));
                DrawLabel(dc, std::to_wstring(m->width) + L" × " + std::to_wstring(m->height), sub,
                          smallFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_LEADING);
                break;
            }
            default:
                break;
            }
        }
        dc->PopAxisAlignedClip();
    }

    // Canvas.
    const D2D1_RECT_F canvas = CanvasRect();
    if (tiles_.empty()) {
        std::wstring hint;
        if (servers_.empty())    hint = L"Add a server to begin.";
        else if (sidebarHidden_) hint = L"Open the sidebar (Tab) to choose mirrors.";
        DrawLabel(dc, hint, canvas, bodyFont_.get(), kDim, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    dc->PushAxisAlignedClip(canvas, D2D1_ANTIALIAS_MODE_ALIASED);
    if (focused_.server != 0) {
        if (const Tile* t = FindTile(focused_)) DrawTile(dc, *t, canvas);
    } else {
        for (const auto& t : tiles_) DrawTile(dc, t, TileRect(t));
    }

    // The edges the dragged box has lined up with, while it is dragged.
    if (drag_.active) {
        brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB, 0.7f));
        for (float gx : drag_.guideX) {
            const float x = std::round(canvas.left + S(gx));
            dc->FillRectangle(D2D1::RectF(x - 0.5f, canvas.top, x + 0.5f, canvas.bottom), brush_.get());
        }
        for (float gy : drag_.guideY) {
            const float y = std::round(canvas.top + S(gy));
            dc->FillRectangle(D2D1::RectF(canvas.left, y - 0.5f, canvas.right, y + 0.5f), brush_.get());
        }
    }
    dc->PopAxisAlignedClip();
}

}  // namespace rvm
