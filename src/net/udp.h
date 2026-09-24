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
    bool Valid() const { return sock_ != INVALID_SOCKET; }

    bool SendTo(const Endpoint& to, const void* data, size_t len);

    // Bytes received, 0 on timeout, -1 on error.
    int Receive(void* buf, size_t cap, Endpoint& from, int timeoutMs);

    static bool Resolve(const std::wstring& host, uint16_t port, Endpoint& out);

private:
    SOCKET sock_ = INVALID_SOCKET;
};

}  // namespace rvm::net
