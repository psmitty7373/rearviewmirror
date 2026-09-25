#pragma once
#include "net/channel.h"
#include "net/codec.h"
#include "net/converter.h"
#include "net/packetizer.h"
#include "net/udp.h"

#include <condition_variable>
#include <set>
#include <thread>

namespace rvm {

// Posted to the window given to Connect(). wParam carries the event in its
// low byte and the tag given to Connect() above it, so one window can tell
// several clients apart.
constexpr UINT WM_RVM_CLIENT_EVENT = WM_APP + 20;
enum class ClientEvent : WPARAM { StatusChanged = 1, ListUpdated, FrameReady };

inline WPARAM MakeClientEvent(ClientEvent event, uint32_t tag) {
    return static_cast<WPARAM>(event) | (static_cast<WPARAM>(tag) << 8);
}
inline ClientEvent ClientEventOf(WPARAM wp) { return static_cast<ClientEvent>(wp & 0xFF); }
inline uint32_t    ClientEventTag(WPARAM wp) { return static_cast<uint32_t>(wp >> 8); }

struct RemoteMirror {
    uint32_t     id = 0;
    std::wstring name;
    UINT         width = 0;
    UINT         height = 0;
};

// What the window draws for one subscribed stream: a BGRA texture that the
// decode thread keeps current.
struct StreamView {
    uint32_t id = 0;
    winrt::com_ptr<ID3D11Texture2D> texture;
    UINT     width = 0;
    UINT     height = 0;
    uint64_t frames = 0;
    // What the server last said about the stream (net::StreamState): why no
    // frames are coming, if they are not.
    uint8_t  state = 0;
};

// What to show in place of a stream the server says it cannot send, or null
// when nothing is wrong beyond frames not having arrived yet.
inline const wchar_t* StreamStateText(uint8_t state) {
    switch (static_cast<net::StreamState>(state)) {
    case net::StreamState::EncoderFull:  return L"The server's encoder is busy with other streams";
    case net::StreamState::CannotEncode: return L"The server's encoder cannot encode this mirror";
    default:                             return nullptr;
    }
}

// Connects to one server, keeps a list of its mirrors, and decodes the
// subscribed ones. A network thread reassembles and requests resends; a decode
// thread turns access units into textures. A lost or refused connection is
// retried until Disconnect(), and subscriptions are restored when it returns.
class StreamClient {
public:
    ~StreamClient();

    void Connect(const std::wstring& host, uint16_t port, const std::wstring& key, HWND notify,
                 uint32_t tag = 0);
    void Disconnect();

    bool Connected() const { return connected_.load(); }
    std::wstring Status() const;
    // Network round trip in microseconds, smoothed over recent pings; -1
    // until the first answer.
    int64_t RttUs() const { return rttUs_.load(); }

    std::vector<RemoteMirror> Mirrors() const;
    bool IsSubscribed(uint32_t id) const;
    void SetSubscribed(uint32_t id, bool on);
    std::vector<StreamView> Views() const;

    // The window calls this when it handles FrameReady, so the next decoded
    // frame posts a fresh one; bursts collapse into one repaint.
    void AckFrameReady() { framePosted_.store(false); }

private:
    struct Stream;

    void NetLoop();
    void DecodeLoop();
    void DecodeOne(Stream& s, net::FrameAssembler::Frame& frame);

    void BeginSession();
    void DropSession(std::wstring status);
    void Send(const std::vector<uint8_t>& plain);
    void SendHello();
    bool HandleDatagram(const uint8_t* data, size_t len, uint64_t nowMs);
    void RequestList(uint64_t nowMs);
    void HandleMessage(net::Reader& r, uint64_t nowMs);
    void PollStreams(uint64_t nowMs);
    void SetStatus(std::wstring status);
    void Notify(ClientEvent event, LPARAM lp = 0);
    std::shared_ptr<Stream> FindStream(uint32_t id) const;

    std::wstring host_;
    uint16_t     port_ = 0;
    std::wstring key_;
    HWND         notify_ = nullptr;
    uint32_t     tag_ = 0;

    net::UdpSocket socket_;
    net::Endpoint  server_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> connected_{ false };
    std::thread netThread_;
    std::thread decodeThread_;

    std::mutex sendMutex_;
    net::SecureChannel channel_;
    net::SecureChannel master_;   // Net thread: seals HELLOs, opens WELCOMEs.
    net::Key masterKey_{};
    std::atomic<uint64_t> session_{ 0 };   // Bumped by every new handshake.

    // Net thread only.
    bool     listPending_ = false;
    uint64_t lastListReqMs_ = 0;
    std::map<uint32_t, uint64_t> unwantedMs_;
    uint32_t clientSession_ = 0;
    uint8_t  clientRandom_[net::kRandomBytes]{};

    mutable std::mutex stateMutex_;
    std::wstring status_;
    std::vector<RemoteMirror> mirrors_;
    std::set<uint32_t> wanted_;   // Mirrors the user chose; survives reconnects.
    std::map<uint32_t, std::shared_ptr<Stream>> streams_;
    std::atomic<int64_t> rttUs_{ -1 };

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::pair<std::shared_ptr<Stream>, net::FrameAssembler::Frame>> queue_;

    std::atomic<bool> framePosted_{ false };
};

}  // namespace rvm
