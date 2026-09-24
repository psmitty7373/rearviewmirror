#pragma once
#include "net/protocol.h"

#include <bcrypt.h>

namespace rvm::net {

bool RandomBytes(void* out, size_t len);

// The shared secret both ends type. PBKDF2-SHA256 with a fixed application
// salt: what matters is that a probe of the port learns nothing, not that the
// passphrase resists an offline attack from someone who already has the key.
Key DeriveMasterKey(const std::wstring& passphrase);

// Per-session key from the two handshake randoms, so a counter is never reused
// under one key across sessions.
Key DeriveSessionKey(const Key& master, const uint8_t clientRandom[kRandomBytes],
                     const uint8_t serverRandom[kRandomBytes]);

// AES-256-GCM through the system provider.
class Cipher {
public:
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
