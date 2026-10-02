#pragma once
#include "net/channel.h"
#include "net/codec.h"
#include "net/converter.h"
#include "net/packetizer.h"
#include "net/udp.h"
#if RVM_REMOTE_CONTROL
#include "net/control.h"
#endif
#include "stream_settings.h"

#include <set>
#include <thread>

namespace rvm {

struct MirrorInfo {
    uint32_t     id = 0;
    std::wstring name;
    UINT         width = 0;
    UINT         height = 0;
    bool         controllable = false;
};

// Serves mirrors over one UDP port: one encoder per watched mirror (hardware,
// else the CPU), shared by its subscribers. Everything a peer can make it do
// is capped or rate-limited.
class StreamServer {
public:
    StreamServer();
    ~StreamServer();
    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    bool Start(const StreamSettings& settings);
    void Stop();
    bool Running() const { return running_.load(); }
    uint16_t Port() const;   // The UDP port bound; settings.port 0 picks a free one.
    size_t ClientCount() const;
    size_t StreamCount() const;   // Encoders and buffers currently allocated.

    // The frame tee: a capture thread, under the device lock, with the
    // mirror's cache texture and effective crop.
    void SubmitFrame(uint32_t mirrorId, ID3D11Texture2D* cache, const RECT& crop);

    // UI thread. The mirrors clients may list and subscribe to.
    void SetMirrorList(std::vector<MirrorInfo> list);

    // Called on the network or encode thread when a stream needs a picture a
    // still source will not send (a keyframe, a retry, a skipped frame): the
    // handler should get the mirror to RepushFrame().
    using FrameRequester = std::function<void(uint32_t mirrorId)>;
    void SetFrameRequester(FrameRequester requester);

    // Any thread; an atomic load while nobody watches anything.
    bool Watched(uint32_t mirrorId) const;

#if RVM_REMOTE_CONTROL
    // Where remote input goes; before Start().
    void SetControlSink(net::ControlHost::Sink sink) { control_.SetSink(std::move(sink)); }
#endif

    // For tests: encode on the CPU from the next Start().
    void ForceSoftwareEncoding(bool on) { forceSoftware_ = on; }
    // For tests: how often the network thread has woken.
    uint64_t NetWakeups() const { return netWakeups_.load(); }

private:
    struct Client;
    struct Stream;

    // An answered HELLO, awaiting the first datagram under the session key:
    // only that proves the peer holds the key, since a HELLO can be replayed.
    struct Pending {
        std::shared_ptr<Client> client;
        uint32_t clientSession = 0;
        uint8_t  clientRandom[net::kRandomBytes]{};
        std::vector<uint8_t> welcome;   // Resent as-is if the HELLO is repeated.
        uint64_t createdMs = 0;
    };

    // HELLOs answered recently, by client session and random: a second copy is a replay.
    using HelloId = std::array<uint8_t, 4 + net::kRandomBytes>;
    struct HelloSeen { HelloId id; uint64_t ms; };
    static constexpr size_t   kMaxRecentHellos = 1024;
    static constexpr uint64_t kRecentHelloMs = 120000;
    void ExpireRecentHellos(uint64_t nowMs);
    void Subscribe(const std::shared_ptr<Client>& client, uint32_t mirrorId, uint64_t nowMs);

    void NetLoop();
    void EncodeLoop();
    void EncodeStream(Stream& s);
    DWORD NextCheckMs(Stream& s);
    void PruneStreams();
    void SendFrames(Stream& s, const std::vector<net::EncodedFrame>& frames);
    void RequestFrame(uint32_t mirrorId);
    // Tells the stream's viewers why it is, or is no longer, stalled.
    void SetStreamState(Stream& s, net::StreamState state);
    void SendStreamState(Client& client, const Stream& s);
    int  OtherOpenEncoders(const Stream& self);
    void ForceKeyframe(Stream& s, uint64_t nowMs);
    uint64_t ServiceDeferredKeyframes(uint64_t nowMs);   // When the next one falls due.

    void HandleDatagram(const net::Endpoint& from, const uint8_t* data, size_t len, uint64_t nowMs);
    void Handshake(const net::Endpoint& from, const uint8_t* data, size_t len, uint64_t nowMs);
    bool Promote(const std::shared_ptr<Client>& client);
    bool AdmissionAllowed(uint64_t nowMs);
    void HandleMessage(const std::shared_ptr<Client>& client, net::Reader& r, uint64_t nowMs);
    void HandleNack(Client& client, net::Reader& r, uint64_t nowMs);
    void SendTo(Client& client, const uint8_t* plain, size_t len);
    void SendTo(Client& client, const std::vector<uint8_t>& plain) { SendTo(client, plain.data(), plain.size()); }
    void SendListTo(Client& client);
    bool MirrorListed(uint32_t mirrorId);
#if RVM_REMOTE_CONTROL
    bool ControlAllowed(uintptr_t peer, uint32_t mirrorId);
    net::ControlHost control_;
#endif
    void DropSubscription(const std::shared_ptr<Client>& client, uint32_t mirrorId);
    void DropUnlistedSubscriptions();
    void RemoveClient(const std::shared_ptr<Client>& client);
    // Drops silent clients, stale handshakes and old HELLOs; returns when the
    // next client or handshake would expire.
    uint64_t Expire(uint64_t nowMs);

    std::shared_ptr<Client> FindClient(const net::Endpoint& endpoint);
    std::shared_ptr<Stream> FindStream(uint32_t mirrorId);
    std::shared_ptr<Stream> AcquireStream(uint32_t mirrorId);   // Creates, and counts a subscriber.

    StreamSettings settings_;
    std::atomic<UINT> fps_{ 60 };   // Read on capture threads; set at Start.
    bool software_ = false;   // Set at Start: no hardware encoder; streams start on the CPU.
    bool forceSoftware_ = false;
    net::Key masterKey_{};
    net::SecureChannel master_;   // Net thread: opens HELLOs, seals WELCOMEs.
    net::UdpSocket socket_;
    std::atomic<bool> running_{ false };
    std::thread netThread_;
    std::thread encodeThread_;
    HANDLE frameEvent_ = nullptr;
    bool sessionFreed_ = false;   // Encode thread: an encoder closed this pass.

    mutable std::mutex clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;

    std::vector<Pending> pending_;   // Net thread only.
    std::set<HelloId>     recentHellos_;       // Net thread only.
    std::deque<HelloSeen> recentHelloOrder_;
    double   admitTokens_ = 0.0;     // Net thread only.
    uint64_t admitRefillMs_ = 0;

    // Net thread deadlines, UINT64_MAX for none. Expiry may be early, never late.
    uint64_t expireDueMs_ = UINT64_MAX;
    uint64_t keyframeDueMs_ = UINT64_MAX;
#if RVM_REMOTE_CONTROL
    uint64_t leaseDueMs_ = UINT64_MAX;
#endif
    std::atomic<bool> listChanged_{ false };
    std::atomic<uint64_t> netWakeups_{ 0 };

    mutable std::mutex streamsMutex_;
    std::map<uint32_t, std::shared_ptr<Stream>> streams_;
    std::atomic<int> watching_{ 0 };   // Subscriptions, over every stream.

    std::mutex listMutex_;
    std::vector<MirrorInfo> mirrorList_;

    std::mutex requesterMutex_;
    FrameRequester requester_;
};

}  // namespace rvm
