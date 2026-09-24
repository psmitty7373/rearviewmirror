#pragma once
#include "net/crypto.h"

namespace rvm::net {

// Seals and opens datagrams for one peer. Each direction has its own session
// id and counter; the receive side keeps a sliding window so a replayed
// datagram is rejected.
class SecureChannel {
public:
    SecureChannel() = default;
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;

    // One key both ways: only for the master-key handshake messages.
    bool SetKey(const Key& key);
    // A session: each direction has its own key.
    bool SetKeys(const Key& sendKey, const Key& recvKey);

    // The handshake runs under the long-lived master key, so it starts its
    // counter at a random point: session ids are only 32 bits, and a nonce
    // must never repeat under one key.
    void BeginSend(uint32_t sessionId, uint64_t startCounter = 0);
    uint32_t SendSession() const { return sendSession_; }

    void BeginRecv(uint32_t sessionId);
    uint32_t RecvSession() const { return recvSession_; }
    bool RecvBound() const { return recvBound_; }

    // Wraps `plain` into a complete datagram. False if it would not fit.
    bool Seal(const uint8_t* plain, size_t len, std::vector<uint8_t>& datagram);

    // Verifies and unwraps. The sender's session id is returned so a server
    // can tell whom a datagram is from before it has bound a receive session.
    bool Open(const uint8_t* datagram, size_t len, std::vector<uint8_t>& plain,
              uint32_t& senderSession);

    // Header check only: is this datagram plausibly ours at all?
    static bool PeekSession(const uint8_t* datagram, size_t len, uint32_t& session);

private:
    // One key object per direction: sealing and opening run on different
    // threads, and CNG does not promise a key handle is safe to share.
    Cipher sendCipher_;
    Cipher recvCipher_;
    uint32_t sendSession_ = 0;
    uint64_t sendCounter_ = 0;

    uint32_t recvSession_ = 0;
    bool     recvBound_ = false;
    uint64_t recvHighest_ = 0;
    uint64_t recvWindow_ = 0;   // Bit i set: counter (highest - i) already seen.
};

}  // namespace rvm::net
