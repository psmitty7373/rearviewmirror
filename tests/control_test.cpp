#include "net/control.h"
#include <cstdio>

using namespace rvm::net;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
std::vector<uint8_t> Deliver(ControlHost& host, uintptr_t peer, const std::vector<uint8_t>& packet,
                             uint64_t now, bool eligible = true) {
    if (packet.empty()) return {};
    Reader r(packet.data() + 1, packet.size() - 1);
    return host.Handle(peer, r, now, eligible);
}
void Reply(ControlClient& client, const std::vector<uint8_t>& packet, uint64_t now) {
    if (packet.empty()) return;
    Reader r(packet.data() + 1, packet.size() - 1);
    client.Reply(r, now);
}
void Connect(ControlClient& c, ControlHost& h, uintptr_t peer, uint64_t now) {
    c.Begin(7, now);
    Reply(c, Deliver(h, peer, c.Poll(now), now), now);
}
RemoteInput Key(bool down) {
    RemoteInput e; e.kind = InputKind::Key; e.code = 'A'; e.value = 0x1e; e.down = down; return e;
}
RemoteInput Button(bool down) {
    RemoteInput e; e.kind = InputKind::Button; e.code = 0; e.value = down ? 1 : 0;
    e.x = 1234; e.y = 5678; return e;
}
void Reliability() {
    std::vector<RemoteInput> received;
    ControlHost host([&](const RemoteInput& e) { received.push_back(e); return true; });
    ControlClient client;
    const uint64_t now = ControlNowMs();
    client.Begin(7, now);
    const auto begin = client.Poll(now);
    Deliver(host, 1, begin, now);   // Lose the grant, then repeat the request.
    Reply(client, Deliver(host, 1, client.Poll(now + 100), now + 100), now + 100);
    Check(client.State() == ControlState::Active, "lost grant is recovered");
    client.Push(Key(true)); client.Push(Key(false));
    const auto first = client.Poll(now + 108);   // Lose this packet.
    Check(!first.empty(), "key transitions queued");
    const auto retry = client.Poll(now + 116);
    Deliver(host, 1, retry, now + 116);   // Lose acknowledgement too.
    Reply(client, Deliver(host, 1, client.Poll(now + 124), now + 124), now + 124);
    Check(received.size() == 2 && received[0].down && !received[1].down,
          "lost and duplicated packets preserve tap order without repeating input");
    client.Push(Button(true));
    Reply(client, Deliver(host, 1, client.Poll(now + 132), now + 132), now + 132);
    client.End(now + 133);
    const auto end = client.Poll(now + 133);
    Reply(client, Deliver(host, 1, end, now + 133), now + 133);
    Check(received.size() == 4 && received.back().kind == InputKind::Button && received.back().value == 0,
          "release ends a held drag");
    Deliver(host, 1, begin, now + 140);
    Deliver(host, 1, retry, now + 140);
    Check(received.size() == 4, "delayed begin and input cannot resurrect released focus");
    Connect(client, host, 1, now + 150);
    Deliver(host, 1, end, now + 151);
    client.Push(Key(true));
    Reply(client, Deliver(host, 1, client.Poll(now + 160), now + 160), now + 160);
    Check(received.size() == 5 && received.back().down, "old release cannot end a newer focus lease");
    host.Poll(now + 1661, [](uintptr_t, uint32_t) { return true; });
    Check(received.size() == 6 && !received.back().down, "lease timeout releases held keyboard input");
}
void Admission() {
    std::vector<RemoteInput> received;
    ControlHost host([&](const RemoteInput& e) { received.push_back(e); return true; });
    ControlClient a, b;
    const uint64_t now = ControlNowMs();
    a.Begin(7, now);
    Reply(a, Deliver(host, 1, a.Poll(now), now, false), now);
    Check(a.State() == ControlState::Denied, "window, cropped, off and unsubscribed mirrors denied by eligibility gate");
    Connect(a, host, 1, now + 1);
    Connect(b, host, 2, now + 1);
    Check(a.State() == ControlState::Active && b.State() == ControlState::Busy, "second viewer cannot steal desktop focus");
    a.Push(Key(true)); a.Push(Button(true));
    Reply(a, Deliver(host, 1, a.Poll(now + 10), now + 10), now + 10);
    host.Poll(now + 11, [](uintptr_t, uint32_t) { return false; });
    Check(received.size() == 4 && !received[2].down && received[3].value == 0,
          "removal or crop change releases keys and mouse buttons");
    Connect(b, host, 2, now + 12);
    Check(b.State() == ControlState::Active, "another viewer can acquire released control");
    b.Push(Key(true));
    Reply(b, Deliver(host, 2, b.Poll(now + 22), now + 22), now + 22);
    host.Drop(2);
    Check(received.size() == 6 && !received.back().down, "disconnect releases input immediately");
}
void BoundsAndReordering() {
    std::vector<RemoteInput> received;
    ControlHost host([&](const RemoteInput& e) { received.push_back(e); return true; });
    ControlClient client;
    const uint64_t now = ControlNowMs();
    Connect(client, host, 1, now);
    client.Push(Key(true));
    const auto first = client.Poll(now + 8);
    auto invalid = first;
    invalid.back() = 2;   // Invalid boolean, no partial injection.
    Check(Deliver(host, 1, invalid, now + 9).empty() && received.empty(), "malformed input rejected before injection");
    auto truncated = first; truncated.pop_back();
    Check(Deliver(host, 1, truncated, now + 10).empty() && received.empty(), "truncated input rejected");
    auto future = first;
    // Msg(1), mirror(4), token(8), op(1), then first sequence.
    future[14] = 2;
    Deliver(host, 1, future, now + 11);
    Check(received.empty(), "out-of-order transition waits for its predecessor");
    Reply(client, Deliver(host, 1, first, now + 12), now + 12);
    RemoteInput move; move.x = 65535; move.y = 65535;
    for (int i = 0; i < 1000; ++i) { move.x = static_cast<uint16_t>(i); client.Push(move); }
    Reply(client, Deliver(host, 1, client.Poll(now + 20), now + 20), now + 20);
    Check(received.size() == 2 && received.back().x == 999, "unsent pointer moves coalesce to the latest position");
    for (int i = 0; i < 300; ++i) client.Push(Key((i & 1) == 0));
    Check(client.State() == ControlState::Lost, "input backlog is bounded and relinquishes control");
    Deliver(host, 1, client.Poll(now + 28), now + 28);
    Check(!received.back().down, "queue overflow releases previously injected keys");
    Connect(client, host, 1, now + 30);
    client.Poll(now + 1531);
    Check(client.State() == ControlState::Lost, "missing acknowledgements end client focus");
    client.Reset();
    Check(client.State() == ControlState::Idle && client.Poll(now + 1540).empty(), "reconnect does not restore input focus");
}
void InjectionFailure() {
    unsigned calls = 0;
    ControlHost host([&](const RemoteInput&) { return ++calls == 1; });
    ControlClient client;
    const uint64_t now = ControlNowMs();
    Connect(client, host, 1, now);
    client.Push(Key(true)); client.Push(Button(true));
    Reply(client, Deliver(host, 1, client.Poll(now + 8), now + 8), now + 8);
    Check(client.State() == ControlState::Lost && calls == 3, "injection failure ends control and attempts held-key release");
}
void LostRepliesAndClockSkew() {
    ControlHost host([](const RemoteInput&) { return true; });
    ControlClient a, b, c;
    const uint64_t now = ControlNowMs();
    Connect(a, host, 1, now);
    b.Begin(7, now);
    Deliver(host, 2, b.Poll(now), now);   // Lose the Busy reply, then repeat the request.
    Reply(b, Deliver(host, 2, b.Poll(now + 100), now + 100), now + 100);
    Check(b.State() == ControlState::Busy, "repeated request after a lost busy reply stays busy");
    c.Begin(7, now + 5);   // The UI thread stamps a time newer than the net thread read.
    c.Poll(now + 4);
    Check(c.State() == ControlState::Pending, "poll time read before a newer stamp does not expire control");
}
// What the hook reports for each key, as the client forwards it, and what
// SendInput must be given for it. Nothing is injected.
void KeyTranslation() {
    const auto key = [](uint16_t vk, int16_t scan) {
        RemoteInput e; e.kind = InputKind::Key; e.code = vk; e.value = scan; e.down = true;
        return InputFor(e);
    };
    const auto extended = [](const INPUT& i) { return (i.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0; };
    const INPUT left = key(VK_LSHIFT, 0x2A), right = key(VK_RSHIFT, 0x136);   // The hook flags right Shift.
    Check(left.ki.wScan == 0x2A && right.ki.wScan == 0x36 && !extended(left) && !extended(right),
          "both Shifts are injected as plain scan codes, never as the fake E0 shift");
    const INPUT rightCtrl = key(VK_RCONTROL, 0x11D), arrow = key(VK_LEFT, 0x14B), letter = key('A', 0x1E);
    Check(extended(rightCtrl) && extended(arrow) && !extended(letter) && letter.ki.wScan == 0x1E,
          "other keys keep their extended flag as reported");
}
}
int main() {
    static_assert(RVM_REMOTE_CONTROL, "Control tests require the feature to be enabled");
    Reliability(); Admission(); BoundsAndReordering(); InjectionFailure(); LostRepliesAndClockSkew();
    KeyTranslation();
    printf("%s\n", failures ? "FAILED" : "All control checks passed");
    return failures ? 1 : 0;
}
