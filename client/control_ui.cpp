#include "client_window.h"

namespace rvm {
namespace { constexpr UINT_PTR kControlTimer = 3; }

void ClientWindow::BeginControl(TileKey key) {
    EndControl();
    const Tile* tile = FindTile(key);
    const RemoteMirror* mirror = MirrorFor(key);
    const StreamView* view = ViewFor(key);
    Server* server = FindServer(key.server);
    if (!tile || tile->popout || !mirror || !mirror->controllable ||
        !view || !view->frames || !server || !server->client->Connected()) return;
    EndDrag(true);
    BringToFront(key);
    SetFocus(Hwnd());
    controlKey_ = key;
    controlState_ = net::ControlState::Pending;
    server->client->Control().Begin(key.id, net::ControlNowMs());
    SetTimer(Hwnd(), kControlTimer, 50, nullptr);
    SetWindowTextW(Hwnd(), L"Rear View Mirror Client — Requesting desktop control…");
    Render();
}

void ClientWindow::EndControl(const wchar_t* message) {
    const TileKey key = controlKey_;
    controlKey_ = {};
    controlState_ = net::ControlState::Idle;
    controlKeyboard_.Stop();
    controlKeyboardOn_ = false;
    controlButtons_ = 0;
    if (key.server) {
        if (Server* server = FindServer(key.server)) server->client->Control().End(net::ControlNowMs());
        if (GetCapture() == Hwnd()) ReleaseCapture();
        KillTimer(Hwnd(), kControlTimer);
        SetWindowTextW(Hwnd(), message ? message : L"Rear View Mirror Client");
        Render();
    }
}

void ClientWindow::PollControl() {
    if (!controlKey_.server) return;
    Server* server = FindServer(controlKey_.server);
    const Tile* tile = FindTile(controlKey_);
    bool eligible = false;
    if (server) {
        for (const auto& mirror : server->client->Mirrors()) {
            if (mirror.id == controlKey_.id && mirror.controllable) eligible = true;
        }
    }
    if (!server || !tile || tile->popout || !eligible || !server->client->Connected() ||
        GetForegroundWindow() != Hwnd() || IsIconic(Hwnd())) { EndControl(); return; }
    const auto state = server->client->Control().State();
    if (state != net::ControlState::Pending && state != net::ControlState::Active) {
        EndControl(state == net::ControlState::Busy
            ? L"Rear View Mirror Client — Desktop is controlled by another viewer"
            : L"Rear View Mirror Client — Desktop control ended; select Control desktop to retry");
        return;
    }
    if (state == net::ControlState::Active && !controlKeyboardOn_) {
        controlKeyboardOn_ = controlKeyboard_.Start(Hwnd(),
            [this](const net::RemoteInput& e) {
                if (Server* s = FindServer(controlKey_.server)) s->client->Control().Push(e);
            }, [window = Hwnd()] { PostMessageW(window, net::WM_RELEASE_CONTROL, 0, 0); });
        if (!controlKeyboardOn_) {
            EndControl(L"Rear View Mirror Client — Could not capture keyboard input");
            return;
        }
        SetWindowTextW(Hwnd(), L"Rear View Mirror Client — Controlling desktop · Ctrl+Alt+F12 to release");
    }
    if (controlState_ != state) { controlState_ = state; Render(); }
}

bool ClientWindow::DrawControlTile(ID2D1DeviceContext* dc, const Tile& tile, const D2D1_RECT_F& cell) {
    if (controlKey_ != tile.key) return false;
    const bool narrow = cell.right - cell.left < S(240.0f);
    const wchar_t* label = controlState_ == net::ControlState::Active
        ? (narrow ? L"Control" : L"Control · Ctrl+Alt+F12")
        : (narrow ? L"Waiting…" : L"Requesting control…");
    dc->PushAxisAlignedClip(cell, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    DrawChip(dc, label, cell.left + S(10.0f), cell.top + S(10.0f), 0, 0, 0.75f);
    dc->PopAxisAlignedClip();
    brush_->SetColor(D2D1::ColorF(kAccentR, kAccentG, kAccentB));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(cell, S(6.0f), S(6.0f)), brush_.get(), S(3.0f));
    return true;
}

bool ClientWindow::ControlMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == net::WM_RELEASE_CONTROL) { EndControl(); return true; }
    if (!controlKey_.server) return false;
    if (msg == WM_KILLFOCUS || msg == WM_CANCELMODE || msg == WM_ENTERSIZEMOVE ||
        (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) ||
        (msg == WM_CAPTURECHANGED && controlButtons_ && reinterpret_cast<HWND>(lp) != Hwnd())) {
        EndControl(); return false;
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
        msg == WM_CHAR || msg == WM_SYSCHAR) {
        // Physical keys go through the hook. Never interpret a remote Tab or
        // Escape as a local layout command, including messages already queued.
        if (controlState_ == net::ControlState::Pending && wp == VK_ESCAPE) EndControl();
        return true;
    }

    net::RemoteInput e;
    bool button = false;
    switch (msg) {
    case WM_MOUSEMOVE: break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: e.code = 0; e.value = 1; button = true; break;
    case WM_LBUTTONUP: e.code = 0; button = true; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: e.code = 1; e.value = 1; button = true; break;
    case WM_RBUTTONUP: e.code = 1; button = true; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: e.code = 2; e.value = 1; button = true; break;
    case WM_MBUTTONUP: e.code = 2; button = true; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        e.code = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4; e.value = 1; button = true; break;
    case WM_XBUTTONUP:
        e.code = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4; button = true; break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        e.kind = net::InputKind::Wheel; e.code = msg == WM_MOUSEHWHEEL ? 1 : 0;
        e.value = GET_WHEEL_DELTA_WPARAM(wp); break;
    default: return false;
    }
    const Tile* tile = FindTile(controlKey_);
    Server* server = FindServer(controlKey_.server);
    if (!tile || !server) { EndControl(); return false; }
    const auto picture = PictureRect(*tile);
    const float w = picture.right - picture.left, h = picture.bottom - picture.top;
    if (w <= 0 || h <= 0) { EndControl(); return false; }
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (e.kind == net::InputKind::Wheel) ScreenToClient(Hwnd(), &pt);
    const bool inside = pt.x >= picture.left && pt.x < picture.right &&
                        pt.y >= picture.top && pt.y < picture.bottom;
    if (!inside && !controlButtons_) {
        if (button && e.value) EndControl();
        return false;
    }
    // Black letterbox bars never move the remote pointer. During a drag,
    // captured motion beyond the picture clamps to its nearest desktop edge.
    if (controlState_ != net::ControlState::Active) return true;
    e.x = static_cast<uint16_t>(std::lround(Clampf((pt.x - picture.left) / (std::max)(w - 1, 1.0f), 0, 1) * 65535));
    e.y = static_cast<uint16_t>(std::lround(Clampf((pt.y - picture.top) / (std::max)(h - 1, 1.0f), 0, 1) * 65535));
    if (button) {
        e.kind = net::InputKind::Button;
        const unsigned bit = 1u << e.code;
        if (e.value) { controlButtons_ |= bit; SetCapture(Hwnd()); }
        else {
            if (!(controlButtons_ & bit)) return true;
            controlButtons_ &= ~bit;
        }
    }
    server->client->Control().Push(e);
    if (button && !controlButtons_ && GetCapture() == Hwnd()) ReleaseCapture();
    return true;
}
}  // namespace rvm
