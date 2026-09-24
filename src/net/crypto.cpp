#include "net/crypto.h"

#include <dpapi.h>

namespace rvm::net {

namespace {

constexpr char  kSalt[] = "RearViewMirror.stream.v2";

// A key nobody can know: used whenever derivation fails, so failure can only
// ever mean "nothing decrypts", never "everything is under a zero key".
Key UnknowableKey() {
    Key k{};
    RandomBytes(k.data(), k.size());
    return k;
}

struct AlgHandle {
    BCRYPT_ALG_HANDLE h = nullptr;
    AlgHandle(LPCWSTR alg, ULONG flags) { BCryptOpenAlgorithmProvider(&h, alg, nullptr, flags); }
    ~AlgHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

}  // namespace

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void RandomBytes(void* out, size_t len) {
    auto* p = static_cast<PUCHAR>(out);
    while (len > 0) {
        const ULONG chunk = static_cast<ULONG>((std::min)(len, static_cast<size_t>(0x10000000)));
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            // Nonces, session ids and keys would be predictable: stop.
            __fastfail(FAST_FAIL_FATAL_APP_EXIT);
        }
        p += chunk;
        len -= chunk;
    }
}

Key DeriveMasterKey(const std::wstring& passphrase) {
    AlgHandle alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!alg.h) return UnknowableKey();

    Key key{};
    std::string pass = ToUtf8(passphrase);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        alg.h, reinterpret_cast<PUCHAR>(pass.data()), static_cast<ULONG>(pass.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(kSalt)), static_cast<ULONG>(sizeof(kSalt) - 1),
        kPbkdf2Iterations, key.data(), static_cast<ULONG>(key.size()), 0);
    SecureZeroMemory(pass.data(), pass.size());
    return BCRYPT_SUCCESS(status) ? key : UnknowableKey();
}

Key DeriveSessionKey(const Key& master, const uint8_t clientRandom[kRandomBytes],
                     const uint8_t serverRandom[kRandomBytes], Direction direction) {
    AlgHandle alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!alg.h) return UnknowableKey();

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg.h, &hash, nullptr, 0,
                                         const_cast<PUCHAR>(master.data()),
                                         static_cast<ULONG>(master.size()), 0))) {
        return UnknowableKey();
    }
    static const char c2s[] = "session c->s";
    static const char s2c[] = "session s->c";
    const char* label = direction == Direction::kClientToServer ? c2s : s2c;
    Key out{};
    const bool ok =
        BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(label)),
                                      sizeof(c2s) - 1, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(clientRandom), kRandomBytes, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(serverRandom), kRandomBytes, 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0));
    BCryptDestroyHash(hash);
    return ok ? out : UnknowableKey();
}

Cipher::~Cipher() {
    Release();
}

void Cipher::Release() {
    if (key_) { BCryptDestroyKey(key_); key_ = nullptr; }
    if (alg_) { BCryptCloseAlgorithmProvider(alg_, 0); alg_ = nullptr; }
}

bool Cipher::Init(const Key& key) {
    Release();
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
        return false;
    }
    if (!BCRYPT_SUCCESS(BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE,
                                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) ||
        !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0,
                                                   const_cast<PUCHAR>(key.data()),
                                                   static_cast<ULONG>(key.size()), 0))) {
        Release();
        return false;
    }
    return true;
}

bool Cipher::Seal(const uint8_t nonce[kNonceBytes], const uint8_t* aad, size_t aadLen,
                  const uint8_t* plain, size_t len, uint8_t* out) const {
    if (!key_ || !plain || !out || len == 0) return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = const_cast<PUCHAR>(nonce);
    info.cbNonce    = kNonceBytes;
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag      = out + len;
    info.cbTag      = kTagBytes;

    ULONG written = 0;
    return BCRYPT_SUCCESS(BCryptEncrypt(key_, const_cast<PUCHAR>(plain), static_cast<ULONG>(len),
                                        &info, nullptr, 0, out, static_cast<ULONG>(len),
                                        &written, 0)) && written == len;
}

bool Cipher::Open(const uint8_t nonce[kNonceBytes], const uint8_t* aad, size_t aadLen,
                  const uint8_t* sealed, size_t len, uint8_t* out) const {
    // With an empty body BCryptDecrypt gets a null output buffer, treats the
    // call as a size query and succeeds without checking the tag: a forged
    // datagram would authenticate. Nothing we send is ever empty.
    if (!key_ || !out || len <= kTagBytes) return false;
    const size_t body = len - kTagBytes;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = const_cast<PUCHAR>(nonce);
    info.cbNonce    = kNonceBytes;
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag      = const_cast<PUCHAR>(sealed + body);
    info.cbTag      = kTagBytes;

    ULONG written = 0;
    return BCRYPT_SUCCESS(BCryptDecrypt(key_, const_cast<PUCHAR>(sealed), static_cast<ULONG>(body),
                                        &info, nullptr, 0, out, static_cast<ULONG>(body),
                                        &written, 0)) && written == body;
}

std::wstring ToHex(const std::vector<uint8_t>& bytes) {
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::vector<uint8_t> FromHex(const std::wstring& hex) {
    std::vector<uint8_t> out;
    if (hex.size() % 2) return out;
    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

bool ProtectSecret(const std::wstring& secret, std::vector<uint8_t>& blob) {
    DATA_BLOB in{ static_cast<DWORD>(secret.size() * sizeof(wchar_t)),
                  reinterpret_cast<BYTE*>(const_cast<wchar_t*>(secret.data())) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"RearViewMirror stream key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    blob.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool UnprotectSecret(const std::vector<uint8_t>& blob, std::wstring& secret) {
    if (blob.empty()) return false;
    DATA_BLOB in{ static_cast<DWORD>(blob.size()), const_cast<BYTE*>(blob.data()) };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    secret.assign(reinterpret_cast<const wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

}  // namespace rvm::net
