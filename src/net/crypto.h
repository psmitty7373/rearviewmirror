#pragma once
#include "net/protocol.h"

#include <bcrypt.h>

namespace rvm::net {

// Cryptographic randomness. The system generator does not fail in practice;
// if it ever does, the process stops rather than carry on with weak values.
void RandomBytes(void* out, size_t len);

// The shared secret both ends type, stretched with PBKDF2-SHA256. A captured
// handshake lets an observer test guesses offline, so the work factor is high
// and the app generates long random keys; a short typed passphrase would be
// the weak point. Any failure yields an unguessable random key, never a
// known one, so a broken crypto provider fails closed.
constexpr unsigned kPbkdf2Iterations = 600000;
Key DeriveMasterKey(const std::wstring& passphrase);

// Per-session, per-direction key from the two handshake randoms, so a
// counter is never reused under one key and a datagram can never be
// reflected back to its sender. `direction` is kClientToServer or
// kServerToClient.
enum class Direction { kClientToServer, kServerToClient };
Key DeriveSessionKey(const Key& master, const uint8_t clientRandom[kRandomBytes],
                     const uint8_t serverRandom[kRandomBytes], Direction direction);

// AES-256-GCM through the system provider.
class Cipher {
public:
    Cipher() = default;
    Cipher(const Cipher&) = delete;             // Owns CNG handles.
    Cipher& operator=(const Cipher&) = delete;
    ~Cipher();
    bool Init(const Key& key);

    // `out` needs len + kTagBytes.
    bool Seal(const uint8_t nonce[kNonceBytes], const uint8_t* aad, size_t aadLen,
              const uint8_t* plain, size_t len, uint8_t* out) const;

    // `len` includes the tag; `out` needs len - kTagBytes.
    bool Open(const uint8_t nonce[kNonceBytes], const uint8_t* aad, size_t aadLen,
              const uint8_t* sealed, size_t len, uint8_t* out) const;

private:
    void Release();
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

// Stores a passphrase for the current Windows user only (DPAPI).
bool ProtectSecret(const std::wstring& secret, std::vector<uint8_t>& blob);
bool UnprotectSecret(const std::vector<uint8_t>& blob, std::wstring& secret);

std::wstring ToHex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> FromHex(const std::wstring& hex);

}  // namespace rvm::net
