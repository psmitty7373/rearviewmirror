#include "net/control.h"

namespace rvm::net {
namespace {
constexpr uint8_t kBegin = 0, kInput = 1, kEnd = 2;
constexpr size_t kBatch = 32, kQueue = 256;

bool Valid(const RemoteInput& e) {
    switch (e.kind) {
    case InputKind::Move: return e.code == 0 && e.value == 0;
    case InputKind::Button: return e.code < 5 && (e.value == 0 || e.value == 1);
    case InputKind::Wheel: return e.code < 2;
    case InputKind::Key: return e.code > 0 && e.code < 256 && e.value >= 0 && e.value <= 0x1ff;
    default: return false;
    }
}
void WriteInput(Writer& w, const RemoteInput& e) {
    w.U8(static_cast<uint8_t>(e.kind)); w.U16(e.code); w.U16(static_cast<uint16_t>(e.value));
    w.U16(e.x); w.U16(e.y); w.U8(e.down ? 1 : 0);
}
bool ReadInput(Reader& r, RemoteInput& e) {
    uint8_t kind = 0, down = 0;
    uint16_t value = 0;
    if (!r.U8(kind) || !r.U16(e.code) || !r.U16(value) || !r.U16(e.x) || !r.U16(e.y) ||
        !r.U8(down) || down > 1) return false;
    e.kind = static_cast<InputKind>(kind); e.value = static_cast<int16_t>(value); e.down = down != 0;
    return Valid(e);
}
bool Inject(const RemoteInput& e) {
    INPUT i{};
    if (e.kind == InputKind::Key) {
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = e.code;
        if (e.value && e.code != VK_PAUSE) {
            i.ki.wVk = 0;
            i.ki.wScan = e.value & 0xff;
            i.ki.dwFlags = KEYEVENTF_SCANCODE | ((e.value & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0);
        }
        if (!e.down) i.ki.dwFlags |= KEYEVENTF_KEYUP;
    } else {
        i.type = INPUT_MOUSE;
        i.mi.dx = e.x; i.mi.dy = e.y;
        i.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        if (e.kind == InputKind::Button) {
            const DWORD down[] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                   MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN };
            const DWORD up[] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP,
                                 MOUSEEVENTF_XUP, MOUSEEVENTF_XUP };
            i.mi.dwFlags |= e.value ? down[e.code] : up[e.code];
            if (e.code >= 3) i.mi.mouseData = e.code == 3 ? XBUTTON1 : XBUTTON2;
        } else if (e.kind == InputKind::Wheel) {
            i.mi.dwFlags |= e.code ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            i.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(e.value));
        }
    }
    return SendInput(1, &i, sizeof(i)) == 1;
}
}

struct ControlHost::Impl {
    Sink sink;
    struct Peer { uint64_t token = 0; double budget = 2000; uint64_t refill = 0; };
    std::map<uintptr_t, Peer> peers;
    uintptr_t owner = 0;
    uint32_t mirror = 0, ack = 0;
    uint64_t token = 0, seen = 0;
    std::map<uint16_t, RemoteInput> keys;
    std::map<uint16_t, RemoteInput> buttons;
    uint16_t x = 0, y = 0;
    explicit Impl(Sink s) : sink(s ? std::move(s) : Sink(Inject)) {}
    void Release() {
        for (auto [code, e] : keys) { e.down = false; sink(e); }
        for (auto [code, e] : buttons) { e.value = 0; e.x = x; e.y = y; sink(e); }
        keys.clear(); buttons.clear(); owner = 0; mirror = 0; ack = 0;
    }
};
ControlHost::ControlHost(Sink sink) : impl_(std::make_unique<Impl>(std::move(sink))) {}
ControlHost::~ControlHost() { Reset(); }
void ControlHost::Reset() { impl_->Release(); impl_->peers.clear(); }
void ControlHost::Drop(uintptr_t peer) {
    if (impl_->owner == peer) impl_->Release();
    impl_->peers.erase(peer);
}
void ControlHost::Poll(uint64_t now, const std::function<bool(uintptr_t, uint32_t)>& eligible) {
    auto& s = *impl_;
    if (s.owner && (now - s.seen >= kControlLeaseMs || !eligible(s.owner, s.mirror))) s.Release();
}
std::vector<uint8_t> ControlHost::Handle(uintptr_t peer, Reader& r, uint64_t now, bool eligible) {
    auto& s = *impl_;
    uint32_t mirror = 0, first = 0;
    uint64_t token = 0;
    uint8_t op = 0, count = 0;
    if (!peer || !r.U32(mirror) || !r.U64(token) || !token || !r.U8(op) || op > kEnd ||
        !r.U32(first) || !r.U8(count) || count > kBatch || (op != kInput && count)) return {};
    std::vector<RemoteInput> events(count);
    for (auto& e : events) if (!ReadInput(r, e)) return {};
    if (r.Left() || (count && (!first || first > UINT32_MAX - count))) return {};
    if (!s.peers.count(peer) && s.peers.size() >= 8) return {};
    auto& p = s.peers[peer];
    ControlState status = ControlState::Denied;
    if (s.owner && now - s.seen >= kControlLeaseMs) s.Release();
    if (token > p.token) {
        p.token = token;
        if (s.owner == peer) s.Release();
        if (op == kBegin && eligible) {
            if (s.owner) status = ControlState::Busy;
            else {
                s.owner = peer; s.mirror = mirror; s.token = token; s.ack = 0; s.seen = now;
            }
        }
    }
    if (s.owner == peer && s.token == token && s.mirror == mirror) {
        if (!eligible || op == kEnd) {
            s.Release();
            status = op == kEnd ? ControlState::Idle : ControlState::Denied;
        } else {
            s.seen = now;
            status = ControlState::Active;
            p.budget = (std::min)(2000.0, p.budget + (now - p.refill) * 2.0);
            p.refill = now;
            for (size_t i = 0; i < events.size(); ++i) {
                const uint32_t seq = first + static_cast<uint32_t>(i);
                if (seq <= s.ack) continue;
                if (seq != s.ack + 1 || p.budget < 1) break;
                p.budget -= 1;
                auto e = events[i];
                if (e.kind == InputKind::Key) {
                    const auto held = s.keys.find(e.code);
                    if (held == s.keys.end() && !e.down) { s.ack = seq; continue; }
                    // A release uses the scan code actually pressed, even if
                    // a peer sends inconsistent metadata on its key-up.
                    if (held != s.keys.end()) e.value = held->second.value;
                } else if (e.kind == InputKind::Button && !e.value && !s.buttons.count(e.code)) {
                    s.ack = seq; continue;
                }
                if (!s.sink(e)) { s.Release(); status = ControlState::Lost; break; }
                if (e.kind == InputKind::Key) {
                    if (e.down) s.keys[e.code] = e; else s.keys.erase(e.code);
                } else {
                    s.x = e.x; s.y = e.y;
                    if (e.kind == InputKind::Button) {
                        if (e.value) s.buttons[e.code] = e; else s.buttons.erase(e.code);
                    }
                }
                s.ack = seq;
            }
        }
    } else if (op == kEnd) status = ControlState::Idle;
    Writer w;
    w.U8(static_cast<uint8_t>(Msg::ControlReply)); w.U64(token);
    w.U32(s.owner == peer && s.token == token ? s.ack : 0);
    w.U8(static_cast<uint8_t>(status));
    return w.Data();
}

struct ControlClient::Impl {
    mutable std::mutex mutex;
    ControlState state = ControlState::Idle;
    uint32_t mirror = 0, next = 1, sentThrough = 0;
    uint64_t token = 0, lastSend = 0, lastReply = 0, progress = 0, endUntil = 0;
    bool ending = false;
    std::deque<std::pair<uint32_t, RemoteInput>> queue;
};
ControlClient::ControlClient() : impl_(std::make_unique<Impl>()) {}
ControlClient::~ControlClient() = default;
void ControlClient::Begin(uint32_t mirror, uint64_t now) {
    auto& s = *impl_; std::lock_guard lock(s.mutex);
    ++s.token; s.mirror = mirror; s.state = ControlState::Pending; s.queue.clear();
    s.next = 1; s.sentThrough = 0; s.lastSend = 0; s.lastReply = s.progress = now; s.ending = false;
}
void ControlClient::End(uint64_t now) {
    auto& s = *impl_; std::lock_guard lock(s.mutex);
    if (s.state == ControlState::Pending || s.state == ControlState::Active) {
        s.ending = true; s.endUntil = now + kControlLeaseMs; s.lastSend = 0;
    }
    s.state = ControlState::Idle; s.queue.clear();
}
void ControlClient::Reset() {
    auto& s = *impl_; std::lock_guard lock(s.mutex);
    s.state = ControlState::Idle; s.ending = false; s.queue.clear(); s.mirror = 0;
}
void ControlClient::Push(const RemoteInput& e) {
    if (!Valid(e)) return;
    auto& s = *impl_; std::lock_guard lock(s.mutex);
    if (s.state != ControlState::Active) return;
    // Only coalesce a move that has never been sent: changing an acknowledged
    // sequence would otherwise discard the final position of a drag.
    if (e.kind == InputKind::Move && !s.queue.empty() && s.queue.back().first > s.sentThrough &&
        s.queue.back().second.kind == InputKind::Move) { s.queue.back().second = e; return; }
    if (s.queue.size() >= kQueue || s.next == UINT32_MAX) {
        s.state = ControlState::Lost; s.ending = true; s.endUntil = ControlNowMs() + kControlLeaseMs;
        s.queue.clear(); s.lastSend = 0; return;
    }
    if (s.queue.empty()) s.progress = ControlNowMs();
    s.queue.emplace_back(s.next++, e);
}
std::vector<uint8_t> ControlClient::Poll(uint64_t now) {
    auto& s = *impl_; std::lock_guard lock(s.mutex);
    if (s.ending && now >= s.endUntil) s.ending = false;
    if (!s.ending && s.state != ControlState::Pending && s.state != ControlState::Active) return {};
    if (!s.ending && (now - s.lastReply >= kControlLeaseMs ||
        (!s.queue.empty() && now - s.progress >= kControlLeaseMs))) {
        s.state = ControlState::Lost; s.ending = true; s.endUntil = now + kControlLeaseMs;
        s.queue.clear();
    }
    const uint64_t interval = s.queue.empty() ? 100 : 8;
    if (s.lastSend && now - s.lastSend < interval) return {};
    s.lastSend = now;
    Writer w;
    w.U8(static_cast<uint8_t>(Msg::Control)); w.U32(s.mirror); w.U64(s.token);
    w.U8(s.ending ? kEnd : s.state == ControlState::Pending ? kBegin : kInput);
    const size_t count = s.ending ? 0 : (std::min)(s.queue.size(), kBatch);
    w.U32(count ? s.queue.front().first : 0); w.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
        WriteInput(w, s.queue[i].second);
        s.sentThrough = (std::max)(s.sentThrough, s.queue[i].first);
    }
    return w.Data();
}
void ControlClient::Reply(Reader& r, uint64_t now) {
    uint64_t token = 0; uint32_t ack = 0; uint8_t state = 0;
    if (!r.U64(token) || !r.U32(ack) || !r.U8(state) || r.Left() ||
        state > static_cast<uint8_t>(ControlState::Lost)) return;
    auto& s = *impl_; std::lock_guard lock(s.mutex);
    if (token != s.token || ack > s.sentThrough) return;
    if (s.ending) {
        if (state != static_cast<uint8_t>(ControlState::Active)) s.ending = false;
        return;
    }
    if (s.state != ControlState::Pending && s.state != ControlState::Active) return;
    s.lastReply = now;
    s.state = static_cast<ControlState>(state);
    while (!s.queue.empty() && s.queue.front().first <= ack) {
        s.queue.pop_front(); s.progress = now;
    }
    if (s.state != ControlState::Active) s.queue.clear();
}
ControlState ControlClient::State() const {
    std::lock_guard lock(impl_->mutex); return impl_->state;
}
uint32_t ControlClient::Mirror() const {
    std::lock_guard lock(impl_->mutex); return impl_->mirror;
}

struct ControlKeyboard::Impl {
    inline static Impl* current = nullptr;   // UI thread only; one focused desktop.
    HHOOK hook = nullptr;
    HWND window = nullptr;
    std::function<void(const RemoteInput&)> input;
    std::function<void()> release;
    std::array<bool, 256> held{};
    bool releasing = false;
    static LRESULT CALLBACK Hook(int code, WPARAM wp, LPARAM lp) {
        Impl* s = current;
        if (code < 0 || !s || GetForegroundWindow() != s->window) return CallNextHookEx(nullptr, code, wp, lp);
        const auto& k = *reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (k.flags & LLKHF_INJECTED || k.vkCode >= 256) return CallNextHookEx(nullptr, code, wp, lp);
        if (s->releasing) return 1;
        const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        const bool ctrl = s->held[VK_LCONTROL] || s->held[VK_RCONTROL] || s->held[VK_CONTROL] ||
                          (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        const bool alt = s->held[VK_LMENU] || s->held[VK_RMENU] || s->held[VK_MENU] ||
                         (GetAsyncKeyState(VK_MENU) & 0x8000);
        if (down && k.vkCode == VK_F12 && ctrl && alt) {
            s->releasing = true;
            s->release(); return 1;
        }
        if (down || s->held[k.vkCode]) {
            RemoteInput e;
            e.kind = InputKind::Key; e.code = static_cast<uint16_t>(k.vkCode);
            e.value = static_cast<int16_t>((k.scanCode & 0xff) | ((k.flags & LLKHF_EXTENDED) ? 0x100 : 0));
            e.down = down; s->held[k.vkCode] = down; s->input(e);
        }
        return 1;
    }
};
ControlKeyboard::ControlKeyboard() : impl_(std::make_unique<Impl>()) {}
ControlKeyboard::~ControlKeyboard() { Stop(); }
bool ControlKeyboard::Start(HWND window, std::function<void(const RemoteInput&)> input,
                            std::function<void()> release) {
    Stop();
    if (Impl::current) return false;
    auto& s = *impl_;
    s.window = window; s.input = std::move(input); s.release = std::move(release); s.held.fill(false);
    s.releasing = false;
    s.hook = SetWindowsHookExW(WH_KEYBOARD_LL, Impl::Hook, GetModuleHandleW(nullptr), 0);
    if (!s.hook) return false;
    Impl::current = &s;
    return true;
}
void ControlKeyboard::Stop() {
    if (Impl::current == impl_.get()) Impl::current = nullptr;
    if (impl_->hook) UnhookWindowsHookEx(impl_->hook);
    impl_->hook = nullptr;
}
}  // namespace rvm::net
