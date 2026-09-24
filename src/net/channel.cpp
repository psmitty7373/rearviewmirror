#include "net/channel.h"

namespace rvm::net {

namespace {

void WriteHeader(uint8_t* h, uint32_t session, uint64_t counter) {
    h[0] = kMagic;
    h[1] = kVersion;
    for (int i = 0; i < 4; ++i) h[2 + i] = static_cast<uint8_t>(session >> (8 * i));
    for (int i = 0; i < 8; ++i) h[6 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

bool ReadHeader(const uint8_t* h, size_t len, uint32_t& session, uint64_t& counter) {
    if (len < kHeaderBytes || h[0] != kMagic || h[1] != kVersion) return false;
    session = 0;
    counter = 0;
    for (int i = 0; i < 4; ++i) session |= static_cast<uint32_t>(h[2 + i]) << (8 * i);
    for (int i = 0; i < 8; ++i) counter |= static_cast<uint64_t>(h[6 + i]) << (8 * i);
    return true;
}

// Nonce = session id || counter: unique per direction for the life of a key.
void MakeNonce(uint8_t nonce[kNonceBytes], uint32_t session, uint64_t counter) {
    for (int i = 0; i < 4; ++i) nonce[i] = static_cast<uint8_t>(session >> (8 * i));
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

}  // namespace

bool SecureChannel::SetKey(const Key& key) {
    return sendCipher_.Init(key) && recvCipher_.Init(key);
}

bool SecureChannel::SetKeys(const Key& sendKey, const Key& recvKey) {
    return sendCipher_.Init(sendKey) && recvCipher_.Init(recvKey);
}

void SecureChannel::BeginSend(uint32_t sessionId, uint64_t startCounter) {
    sendSession_ = sessionId;
    sendCounter_ = startCounter;
}

void SecureChannel::BeginRecv(uint32_t sessionId) {
    recvSession_ = sessionId;
    recvBound_ = true;
    recvHighest_ = 0;
    recvWindow_ = 0;
}

bool SecureChannel::PeekSession(const uint8_t* datagram, size_t len, uint32_t& session) {
    uint64_t counter = 0;
    return ReadHeader(datagram, len, session, counter);
}

bool SecureChannel::Seal(const uint8_t* plain, size_t len, std::vector<uint8_t>& datagram) {
    if (len > kMaxPlain) return false;
    const uint64_t counter = ++sendCounter_;

    datagram.resize(kHeaderBytes + len + kTagBytes);
    WriteHeader(datagram.data(), sendSession_, counter);

    uint8_t nonce[kNonceBytes];
    MakeNonce(nonce, sendSession_, counter);
    return sendCipher_.Seal(nonce, datagram.data(), kHeaderBytes, plain, len,
                        datagram.data() + kHeaderBytes);
}

bool SecureChannel::Open(const uint8_t* datagram, size_t len, std::vector<uint8_t>& plain,
                         uint32_t& senderSession) {
    uint64_t counter = 0;
    if (!ReadHeader(datagram, len, senderSession, counter)) return false;
    // Every message carries at least its type byte; a body-less datagram is
    // forged by definition and must not even reach the cipher.
    if (len > kMaxDatagram || len < kHeaderBytes + kTagBytes + 1) return false;
    if (recvBound_ && senderSession != recvSession_) return false;

    // Replay window: reject anything already seen or too old to track.
    if (recvBound_) {
        if (counter == 0) return false;
        if (counter <= recvHighest_) {
            const uint64_t back = recvHighest_ - counter;
            if (back >= 64 || (recvWindow_ & (1ull << back))) return false;
        }
    }

    uint8_t nonce[kNonceBytes];
    MakeNonce(nonce, senderSession, counter);
    plain.resize(len - kHeaderBytes - kTagBytes);
    if (!recvCipher_.Open(nonce, datagram, kHeaderBytes, datagram + kHeaderBytes,
                      len - kHeaderBytes, plain.data())) {
        return false;
    }

    // Only a datagram that authenticated may advance the window.
    if (recvBound_) {
        if (counter > recvHighest_) {
            const uint64_t shift = counter - recvHighest_;
            recvWindow_ = (shift >= 64) ? 0 : (recvWindow_ << shift);
            recvWindow_ |= 1;
            recvHighest_ = counter;
        } else {
            recvWindow_ |= 1ull << (recvHighest_ - counter);
        }
    }
    return true;
}

}  // namespace rvm::net
