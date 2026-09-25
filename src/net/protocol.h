#pragma once
#include "common.h"

#include <array>
#include <deque>
#include <map>

// Wire protocol for streaming mirrors to a client over one UDP port.
//
// Every datagram is  [14-byte header][ciphertext][16-byte GCM tag].  The header
// is authenticated but not encrypted: magic, version, the sender's session id
// and a per-session counter, which together also form the AES-GCM nonce. The
// plaintext begins with a Msg byte.
namespace rvm::net {

constexpr uint8_t  kMagic   = 0xA5;
constexpr uint8_t  kVersion = 2;   // 2: per-direction keys, bound WELCOME, 600k PBKDF2.
constexpr uint16_t kDefaultPort = 5901;

constexpr size_t kKeyBytes    = 32;
constexpr size_t kTagBytes    = 16;
constexpr size_t kNonceBytes  = 12;
constexpr size_t kRandomBytes = 16;
constexpr size_t kHeaderBytes = 1 + 1 + 4 + 8;

// Stays under any sane path MTU so nothing fragments at the IP layer.
constexpr size_t kMaxDatagram = 1200;
constexpr size_t kMaxPlain    = kMaxDatagram - kHeaderBytes - kTagBytes;

using Key = std::array<uint8_t, kKeyBytes>;

// Hardware encoders refuse small frames: NVENC accepts 256x128 but not
// 128x128. A small crop is scaled up uniformly until both dimensions clear
// this floor on the way into the encoder; the client just shows the larger
// picture.
constexpr UINT kMinEncodeDim = 256;

// The other way, H.264 hardware encoders stop at 4096 a side, and the GPU at
// 16384. Larger crops are scaled down to fit.
constexpr UINT kMaxEncodeDim = 4096;

// The floors tried in turn when an encoder refuses a size even at 16-pixel
// alignment. The client tries the same ones to recognise what it receives.
constexpr UINT kEncodeFloors[] = { 256, 384, 512, 768, 1024 };

// The size the server encodes a crop at: scaled up uniformly to clear the
// floor, down uniformly to fit the ceiling, even in both dimensions for NV12,
// and padded to whole macroblocks for encoders that need it. A strip too thin
// to satisfy both limits in proportion is held at them, so its stream is out
// of proportion; the client, which knows the crop from the mirror list, uses
// this to recognise that and draw it in the true proportions.
//
// `minDim` raises the floor for an encoder that refuses a size above the
// usual one.
inline void EncodeSize(UINT cropW, UINT cropH, bool align16, UINT& w, UINT& h,
                       UINT minDim = kMinEncodeDim) {
    minDim = (std::min)((std::max)(minDim, kMinEncodeDim), kMaxEncodeDim);
    double sw = cropW, sh = cropH;
    const double up = (std::max)(1.0, (std::max)(minDim / sw, minDim / sh));
    sw *= up;
    sh *= up;
    const double down = (std::min)(1.0, (std::min)(kMaxEncodeDim / sw, kMaxEncodeDim / sh));
    sw *= down;
    sh *= down;
    w = static_cast<UINT>(ClampI(static_cast<int>(sw), static_cast<int>(minDim),
                                 static_cast<int>(kMaxEncodeDim))) & ~1u;
    h = static_cast<UINT>(ClampI(static_cast<int>(sh), static_cast<int>(minDim),
                                 static_cast<int>(kMaxEncodeDim))) & ~1u;
    if (align16) {
        w = (std::min)((w + 15) & ~15u, kMaxEncodeDim);
        h = (std::min)((h + 15) & ~15u, kMaxEncodeDim);
    }
}

enum class Msg : uint8_t {
    Hello = 1,       // client -> server  {version u16, clientRandom[16]}
    Welcome,         // server -> client  {serverRandom[16], clientRandom[16] echoed}
    ListReq,         // client -> server
    ListResp,        // server -> client  {count u8, {id u32, w u16, h u16, name str}...}
    Subscribe,       // client -> server  {mirrorId u32}
    Unsubscribe,     // client -> server  {mirrorId u32}
    Frame,           // server -> client  FrameHeader + chunk
    Nack,            // client -> server  {mirrorId u32, frameSeq u32, n u16, idx u16[n]}
    KeyframeReq,     // client -> server  {mirrorId u32}
    Ping,            // either            {t u64}
    Pong,            // either            {t u64}
    MirrorsChanged,  // server -> client  (re-list)
    Bye,             // either
    StreamStatus,    // server -> client  {mirrorId u32, state u8}: why a stream is not coming
};

// What a StreamStatus message reports. A client that does not know the message
// ignores it and simply keeps waiting for frames.
enum class StreamState : uint8_t {
    Ok = 0,             // Frames are coming, or will.
    EncoderFull = 1,    // The GPU has no encoder session free; retried when one frees.
    CannotEncode = 2,   // The encoder refuses this mirror at every size tried.
};

constexpr uint8_t kFlagKeyframe = 0x01;

struct FrameHeader {
    uint32_t mirrorId = 0;
    uint32_t frameSeq = 0;
    uint16_t pktIdx   = 0;
    uint16_t pktCount = 0;
    uint8_t  flags    = 0;
};
constexpr size_t kFrameHeaderBytes = 1 + 4 + 4 + 2 + 2 + 1;   // Msg byte included.
constexpr size_t kMaxChunk = kMaxPlain - kFrameHeaderBytes;

// Little-endian serialisation.
class Writer {
public:
    void U8(uint8_t v)   { buf_.push_back(v); }
    void U16(uint16_t v) { U8(v & 0xFF); U8(v >> 8); }
    void U32(uint32_t v) { U16(v & 0xFFFF); U16(static_cast<uint16_t>(v >> 16)); }
    void U64(uint64_t v) { U32(static_cast<uint32_t>(v)); U32(static_cast<uint32_t>(v >> 32)); }
    void Bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    // Length-prefixed UTF-8, capped at 255 bytes.
    void Str(const std::string& s) {
        const size_t n = (std::min)(s.size(), static_cast<size_t>(255));
        U8(static_cast<uint8_t>(n));
        Bytes(s.data(), n);
    }
    const std::vector<uint8_t>& Data() const { return buf_; }
    std::vector<uint8_t>& Data() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

    bool U8(uint8_t& v)   { if (Left() < 1) return false; v = *p_++; return true; }
    bool U16(uint16_t& v) { uint8_t a, b; if (!U8(a) || !U8(b)) return false; v = a | (b << 8); return true; }
    bool U32(uint32_t& v) { uint16_t a, b; if (!U16(a) || !U16(b)) return false; v = a | (static_cast<uint32_t>(b) << 16); return true; }
    bool U64(uint64_t& v) { uint32_t a, b; if (!U32(a) || !U32(b)) return false; v = a | (static_cast<uint64_t>(b) << 32); return true; }
    bool Bytes(void* out, size_t n) {
        if (Left() < n) return false;
        memcpy(out, p_, n);
        p_ += n;
        return true;
    }
    bool Str(std::string& s) {
        uint8_t n = 0;
        if (!U8(n) || Left() < n) return false;
        s.assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return true;
    }
    size_t Left() const { return static_cast<size_t>(end_ - p_); }
    const uint8_t* Ptr() const { return p_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

inline void WriteFrameHeader(Writer& w, const FrameHeader& h) {
    w.U8(static_cast<uint8_t>(Msg::Frame));
    w.U32(h.mirrorId);
    w.U32(h.frameSeq);
    w.U16(h.pktIdx);
    w.U16(h.pktCount);
    w.U8(h.flags);
}

// Reads the fields after the Msg byte.
inline bool ReadFrameHeader(Reader& r, FrameHeader& h) {
    return r.U32(h.mirrorId) && r.U32(h.frameSeq) && r.U16(h.pktIdx) && r.U16(h.pktCount) &&
           r.U8(h.flags);
}

std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);

}  // namespace rvm::net
