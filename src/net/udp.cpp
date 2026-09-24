#include "net/udp.h"

// Not every SDK header set exposes this; it is a stable IOCTL code.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace rvm::net {

namespace {

struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { WSACleanup(); }
};

void EnsureWinsock() {
    static WinsockInit init;
}

}  // namespace

bool Endpoint::operator==(const Endpoint& o) const {
    if (len != o.len || addr.ss_family != o.addr.ss_family) return false;
    if (addr.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
        const auto* b = reinterpret_cast<const sockaddr_in6*>(&o.addr);
        return a->sin6_port == b->sin6_port &&
               memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(in6_addr)) == 0;
    }
    const auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
    const auto* b = reinterpret_cast<const sockaddr_in*>(&o.addr);
    return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}

std::wstring Endpoint::ToString() const {
    wchar_t host[NI_MAXHOST]{}, service[NI_MAXSERV]{};
    if (GetNameInfoW(reinterpret_cast<const sockaddr*>(&addr), len, host, NI_MAXHOST,
                     service, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return L"?";
    }
    return std::wstring(host) + L":" + service;
}

UdpSocket::~UdpSocket() {
    Close();
}

bool UdpSocket::Open(uint16_t bindPort) {
    EnsureWinsock();
    Close();

    sock_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) return false;

    // Accept IPv4 as mapped addresses on the same socket.
    DWORD v6only = 0;
    setsockopt(sock_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));

    // Generous buffers: a burst of packets for several keyframes at once must
    // not be dropped by the stack before we read them.
    int bufSize = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    // A remote host closing its port produces ICMP that Winsock would otherwise
    // surface as an error on the next receive.
    BOOL off = FALSE;
    DWORD ignored = 0;
    WSAIoctl(sock_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);

    sockaddr_in6 local{};
    local.sin6_family = AF_INET6;
    local.sin6_addr   = in6addr_any;
    local.sin6_port   = htons(bindPort);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        Close();
        return false;
    }
    return true;
}

uint16_t UdpSocket::LocalPort() const {
    if (sock_ == INVALID_SOCKET) return 0;
    sockaddr_in6 local{};
    int len = sizeof(local);
    if (getsockname(sock_, reinterpret_cast<sockaddr*>(&local), &len) != 0) return 0;
    return ntohs(local.sin6_port);
}

void UdpSocket::Close() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

bool UdpSocket::SendTo(const Endpoint& to, const void* data, size_t len) {
    if (sock_ == INVALID_SOCKET) return false;
    return sendto(sock_, static_cast<const char*>(data), static_cast<int>(len), 0,
                  reinterpret_cast<const sockaddr*>(&to.addr), to.len) == static_cast<int>(len);
}

int UdpSocket::Receive(void* buf, size_t cap, Endpoint& from, int timeoutMs) {
    if (sock_ == INVALID_SOCKET) return -1;

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(sock_, &readable);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    const int ready = select(0, &readable, nullptr, nullptr, &tv);
    if (ready == 0) return 0;
    if (ready < 0) return -1;

    from.len = sizeof(from.addr);
    const int n = recvfrom(sock_, static_cast<char*>(buf), static_cast<int>(cap), 0,
                           reinterpret_cast<sockaddr*>(&from.addr), &from.len);
    if (n < 0) {
        // A single oversized or reset datagram is not fatal to the socket.
        const int err = WSAGetLastError();
        return (err == WSAEMSGSIZE || err == WSAECONNRESET) ? 0 : -1;
    }
    return n;
}

bool UdpSocket::Resolve(const std::wstring& host, uint16_t port, Endpoint& out) {
    EnsureWinsock();

    ADDRINFOW hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    ADDRINFOW* result = nullptr;
    const std::wstring service = std::to_wstring(port);
    if (GetAddrInfoW(host.c_str(), service.c_str(), &hints, &result) != 0 || !result) return false;

    // Our socket is IPv6 with v6only off, so an IPv4 result is sent to as a
    // mapped address.
    bool ok = false;
    for (ADDRINFOW* ai = result; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET6) {
            memcpy(&out.addr, ai->ai_addr, ai->ai_addrlen);
            out.len = static_cast<int>(ai->ai_addrlen);
            ok = true;
            break;
        }
        if (ai->ai_family == AF_INET && !ok) {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
            sockaddr_in6 v6{};
            v6.sin6_family = AF_INET6;
            v6.sin6_port   = v4->sin_port;
            v6.sin6_addr.u.Word[5] = 0xFFFF;
            memcpy(&v6.sin6_addr.u.Byte[12], &v4->sin_addr, 4);
            memcpy(&out.addr, &v6, sizeof(v6));
            out.len = sizeof(v6);
            ok = true;
        }
    }
    FreeAddrInfoW(result);
    return ok;
}

}  // namespace rvm::net
