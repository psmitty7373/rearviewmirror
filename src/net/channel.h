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

    // Under the long-lived master key, start at a random counter: session ids
    // are only 32 bits, and a nonce must never repeat under one key.
    void BeginSend(uint32_t sessionId, uint64_t startCounter = 0);
    void BeginRecv(uint32_t sessionId);

    // Wraps `plain` into a complete datagram. False if it would not fit.
    bool Seal(const uint8_t* plain, size_t len, std::vector<uint8_t>& datagram);

    // Verifies and unwraps; the sender's session id is returned for a channel
    // with no receive session bound yet.
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
