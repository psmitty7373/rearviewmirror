#pragma once
#include "net/channel.h"
#include "net/codec.h"
#include "net/converter.h"
#include "net/packetizer.h"
#include "net/udp.h"
#include "stream_settings.h"

#include <set>
#include <thread>

namespace rvm {

struct MirrorInfo {
    uint32_t     id = 0;
    std::wstring name;
    UINT         width = 0;
    UINT         height = 0;
};

// Serves mirrors to clients over one UDP port. Each subscribed mirror gets a
// hardware encoder fed by a GPU crop of the frames the mirror already has; the
// only CPU work per frame is packetising and sealing the bitstream.
//
// Everything a peer can make the server do is bounded: sessions, pending
// handshakes, subscriptions, streams, keyframes, frame requests and
// retransmits all have caps or rates, so neither a stranger nor a client
// holding the key can grow memory or flood the app.
class StreamServer {
public:
    ~StreamServer();

    bool Start(const StreamSettings& settings);
    void Stop();
    bool Running() const { return running_.load(); }
    size_t ClientCount() const;
    size_t StreamCount() const;   // Encoders and buffers currently allocated.

    // The frame tee. Called on a capture thread under the renderer and device
    // locks with the mirror's cache texture and effective crop; until a client
    // subscribes to that mirror it costs one atomic load.
    void SubmitFrame(uint32_t mirrorId, ID3D11Texture2D* cache, const RECT& crop);

    // UI thread. What clients see when they ask for the list, and the only
    // mirrors they may subscribe to.
    void SetMirrorList(std::vector<MirrorInfo> list);

    // Invoked (on the network thread) when a stream needs a frame it may not
    // otherwise get: a client just subscribed, or asked for a keyframe. The
    // handler should get the mirror to RepushFrame(). Rate-limited per mirror.
    using FrameRequester = std::function<void(uint32_t mirrorId)>;
    void SetFrameRequester(FrameRequester requester);

private:
    struct Client;
    struct Stream;

    // A HELLO that was answered, waiting for the first datagram under the
    // session key. Only that datagram proves the peer holds the key: a HELLO
    // can be replayed by anyone who captured one.
    struct Pending {
        std::shared_ptr<Client> client;
        uint32_t clientSession = 0;
        std::vector<uint8_t> welcome;   // Resent as-is if the HELLO is repeated.
        uint64_t createdMs = 0;
    };

    void NetLoop();
    void EncodeLoop();
    void EncodeStream(Stream& s);
    void PruneStreams();
    void SendFrames(Stream& s, const std::vector<net::EncodedFrame>& frames);
    void RequestFrame(uint32_t mirrorId);
    void ForceKeyframe(Stream& s, uint64_t nowMs);
    void ServiceDeferredKeyframes(uint64_t nowMs);

    void HandleDatagram(const net::Endpoint& from, const uint8_t* data, size_t len, uint64_t nowMs);
    void Handshake(const net::Endpoint& from, const uint8_t* data, size_t len, uint64_t nowMs);
    bool Promote(const std::shared_ptr<Client>& client);
    bool AdmissionAllowed(uint64_t nowMs);
    void HandleMessage(const std::shared_ptr<Client>& client, net::Reader& r, uint64_t nowMs);
    void HandleNack(Client& client, net::Reader& r, uint64_t nowMs);
    void SendTo(Client& client, const std::vector<uint8_t>& plain);
    void SendListTo(Client& client);
    bool MirrorListed(uint32_t mirrorId);
    void DropSubscription(const std::shared_ptr<Client>& client, uint32_t mirrorId);
    void DropUnlistedSubscriptions();
    void RemoveClient(const std::shared_ptr<Client>& client);
    void ExpireClients(uint64_t nowMs);

    std::shared_ptr<Client> FindClient(const net::Endpoint& endpoint);
    std::shared_ptr<Stream> FindStream(uint32_t mirrorId);
    std::shared_ptr<Stream> AcquireStream(uint32_t mirrorId);   // Creates, and counts a subscriber.

    StreamSettings settings_;
    std::atomic<UINT> fps_{ 60 };   // Read on capture threads; set at Start.
    net::Key masterKey_{};
    net::SecureChannel master_;   // Net thread: opens HELLOs, seals WELCOMEs.
    net::UdpSocket socket_;
    std::atomic<bool> running_{ false };
    std::thread netThread_;
    std::thread encodeThread_;
    HANDLE frameEvent_ = nullptr;

    mutable std::mutex clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;

    std::vector<Pending> pending_;   // Net thread only.
    double   admitTokens_ = 0.0;     // Net thread only.
    uint64_t admitRefillMs_ = 0;

    mutable std::mutex streamsMutex_;
    std::map<uint32_t, std::shared_ptr<Stream>> streams_;

    std::mutex listMutex_;
    std::vector<MirrorInfo> mirrorList_;

    std::mutex requesterMutex_;
    FrameRequester requester_;
};

}  // namespace rvm
