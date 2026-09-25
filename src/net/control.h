#pragma once
#if RVM_REMOTE_CONTROL
#include "net/protocol.h"

namespace rvm::net {
constexpr UINT WM_RELEASE_CONTROL = WM_APP + 23;

// Input pacing must not inherit GetTickCount64's roughly 16 ms granularity.
inline uint64_t ControlNowMs() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c);
    const auto ticks = static_cast<uint64_t>(c.QuadPart);
    return ticks / frequency * 1000 + ticks % frequency * 1000 / frequency;
}

enum class InputKind : uint8_t { Move, Button, Wheel, Key };
struct RemoteInput {
    InputKind kind = InputKind::Move;
    uint16_t code = 0;   // Button 0..4, wheel axis 0..1, or virtual key.
    int16_t value = 0;   // Button down, wheel delta, or scan code + extended bit (0x100).
    uint16_t x = 0, y = 0;   // Absolute virtual-desktop coordinates, 0..65535.
    bool down = false;       // Key direction.
};
enum class ControlState : uint8_t { Idle, Pending, Active, Busy, Denied, Lost };
constexpr uint64_t kControlLeaseMs = 1500;

// Network-thread only. One controlling viewer per server, regardless of the
// number of desktop mirrors. Tests supply a sink instead of injecting input.
class ControlHost {
public:
    using Sink = std::function<bool(const RemoteInput&)>;
    explicit ControlHost(Sink sink = {});
    ~ControlHost();
    std::vector<uint8_t> Handle(uintptr_t peer, Reader& r, uint64_t now, bool eligible);
    void Poll(uint64_t now, const std::function<bool(uintptr_t, uint32_t)>& eligible);
    void Drop(uintptr_t peer);
    void Reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// UI enqueues input; network thread sends/retries bounded batches and applies
// acknowledgements. No input survives a connection or control-focus change.
class ControlClient {
public:
    ControlClient();
    ~ControlClient();
    void Begin(uint32_t mirror, uint64_t now);
    void End(uint64_t now);
    void Reset();
    void Push(const RemoteInput& input);
    std::vector<uint8_t> Poll(uint64_t now);
    void Reply(Reader& r, uint64_t now);
    ControlState State() const;
    uint32_t Mirror() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The hook captures Windows shortcuts too, but only while this window is
// foreground. Ctrl+Alt+F12 always releases focus locally.
class ControlKeyboard {
public:
    ControlKeyboard();
    ~ControlKeyboard();
    bool Start(HWND window, std::function<void(const RemoteInput&)> input,
               std::function<void()> release);
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rvm::net
#endif  // RVM_REMOTE_CONTROL
