#pragma once
#include "net/protocol.h"

#include <mstcpip.h>

namespace rvm::net {

struct Endpoint {
    sockaddr_storage addr{};
    int len = 0;

    bool operator==(const Endpoint& o) const;
    std::wstring ToString() const;
};

// A single UDP socket, dual-stack where the OS allows it.
class UdpSocket {
public:
    UdpSocket() = default;
    UdpSocket(const UdpSocket&) = delete;             // Owns the socket.
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket();

    bool Open(uint16_t bindPort);   // 0 for an ephemeral port.
    uint16_t LocalPort() const;     // The port actually bound, or 0.
    void Close();

    bool SendTo(const Endpoint& to, const void* data, size_t len);

    // Bytes received, 0 on timeout, -1 on error.
    int Receive(void* buf, size_t cap, Endpoint& from, int timeoutMs);

    // Returns when a datagram is waiting (take it, and any more, with
    // Receive(..., 0)), on Wake() from any thread, or after `timeoutMs`, to
    // the millisecond. A wake given while nobody waits is kept.
    enum class WaitResult { Readable, Woken, Timeout, Error };
    WaitResult Wait(DWORD timeoutMs);
    void Wake();

    static bool Resolve(const std::wstring& host, uint16_t port, Endpoint& out);

private:
    // 1: done, 0: a datagram was discarded instead, -1: error.
    int PostReceive();
    int CompleteReceive();

    SOCKET sock_ = INVALID_SOCKET;

    // Wait's machinery: one overlapped receive into held_, so the socket
    // itself stays blocking for senders.
    HANDLE wake_ = nullptr;    // Auto-reset.
    HANDLE timer_ = nullptr;   // High resolution, where the OS has it.
    WSAOVERLAPPED recv_{};
    bool posted_ = false;
    bool held_ = false;
    DWORD recvFlags_ = 0;
    DWORD heldLen_ = 0;
    Endpoint heldFrom_;
    std::vector<uint8_t> heldBuf_;
};

}  // namespace rvm::net
