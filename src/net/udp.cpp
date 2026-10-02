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

// A single oversized or reset datagram is not fatal to the socket.
bool Discardable(int err) {
    return err == WSAEMSGSIZE || err == WSAECONNRESET || err == WSAENETRESET;
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
    if (wake_) CloseHandle(wake_);
    if (timer_) CloseHandle(timer_);
    if (recv_.hEvent) WSACloseEvent(recv_.hEvent);
}

bool UdpSocket::Open(uint16_t bindPort) {
    EnsureWinsock();
    Close();

    if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!timer_) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    }
    if (!recv_.hEvent) recv_.hEvent = WSACreateEvent();
    if (!wake_ || recv_.hEvent == WSA_INVALID_EVENT) return false;
    heldBuf_.resize(2048);   // Anything longer is no datagram of ours.

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
    if (sock_ == INVALID_SOCKET) return;
    if (posted_) {
        // The receive must be over before its buffer and OVERLAPPED are reused.
        CancelIoEx(reinterpret_cast<HANDLE>(sock_), &recv_);
        DWORD n = 0, flags = 0;
        WSAGetOverlappedResult(sock_, &recv_, &n, TRUE, &flags);
        posted_ = false;
    }
    held_ = false;
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
}

bool UdpSocket::SendTo(const Endpoint& to, const void* data, size_t len) {
    if (sock_ == INVALID_SOCKET) return false;
    return sendto(sock_, static_cast<const char*>(data), static_cast<int>(len), 0,
                  reinterpret_cast<const sockaddr*>(&to.addr), to.len) == static_cast<int>(len);
}

int UdpSocket::Receive(void* buf, size_t cap, Endpoint& from, int timeoutMs) {
    if (sock_ == INVALID_SOCKET) return -1;

    // A receive that Wait posted gets the next datagram.
    if (posted_) {
        const DWORD wait = static_cast<DWORD>((std::max)(timeoutMs, 0));
        if (WaitForSingleObject(recv_.hEvent, wait) != WAIT_OBJECT_0) return 0;
        if (CompleteReceive() < 0) return -1;
    }
    if (held_) {
        held_ = false;
        if (heldLen_ > cap) return 0;
        memcpy(buf, heldBuf_.data(), heldLen_);
        from = heldFrom_;
        return static_cast<int>(heldLen_);
    }
    if (posted_) return 0;

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
        return Discardable(WSAGetLastError()) ? 0 : -1;
    }
    return n;
}

int UdpSocket::PostReceive() {
    WSAResetEvent(recv_.hEvent);
    WSABUF b{ static_cast<ULONG>(heldBuf_.size()), reinterpret_cast<char*>(heldBuf_.data()) };
    heldFrom_.len = sizeof(heldFrom_.addr);
    recvFlags_ = 0;
    // Finishing at once signals the event too, so it is collected the same way.
    if (WSARecvFrom(sock_, &b, 1, nullptr, &recvFlags_, reinterpret_cast<sockaddr*>(&heldFrom_.addr),
                    &heldFrom_.len, &recv_, nullptr) != 0) {
        const int err = WSAGetLastError();
        if (Discardable(err)) return 0;
        if (err != WSA_IO_PENDING) return -1;
    }
    posted_ = true;
    return 1;
}

int UdpSocket::CompleteReceive() {
    posted_ = false;
    DWORD flags = 0;
    if (!WSAGetOverlappedResult(sock_, &recv_, &heldLen_, FALSE, &flags)) {
        return Discardable(WSAGetLastError()) ? 0 : -1;
    }
    held_ = true;
    return 1;
}

UdpSocket::WaitResult UdpSocket::Wait(DWORD timeoutMs) {
    if (sock_ == INVALID_SOCKET) return WaitResult::Error;
    if (held_) return WaitResult::Readable;
    // A discarded datagram ends the wait at once, so a flood of them cannot
    // hold the caller's deadlines off.
    if (!posted_) {
        const int posted = PostReceive();
        if (posted <= 0) return posted == 0 ? WaitResult::Timeout : WaitResult::Error;
    }

    // The plain timeout rounds up to the system tick; a high-resolution timer
    // keeps short waits short without raising the tick rate.
    HANDLE handles[3] = { recv_.hEvent, wake_, timer_ };
    DWORD count = 2, wait = timeoutMs;
    if (timer_ && timeoutMs != INFINITE && timeoutMs > 0) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(timeoutMs) * 10'000;
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            count = 3;
            wait = INFINITE;
        }
    }
    switch (WaitForMultipleObjects(count, handles, FALSE, wait)) {
    case WAIT_OBJECT_0: {
        const int got = CompleteReceive();
        return got > 0 ? WaitResult::Readable : got == 0 ? WaitResult::Timeout : WaitResult::Error;
    }
    case WAIT_OBJECT_0 + 1: return WaitResult::Woken;
    case WAIT_OBJECT_0 + 2:
    case WAIT_TIMEOUT:      return WaitResult::Timeout;
    default:                return WaitResult::Error;
    }
}

void UdpSocket::Wake() {
    if (wake_) SetEvent(wake_);
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
