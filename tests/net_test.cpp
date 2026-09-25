// Headless checks for the streaming stack: encryption, replay protection,
// packetisation with loss and recovery, and a full GPU encode -> decode round
// trip through Media Foundation.
#include "capture.h"
#include "net/channel.h"
#include "net/codec.h"
#include "net/converter.h"
#include "net/packetizer.h"
#include "persist.h"
#include "stream_client.h"
#include "stream_server.h"

#include <cstdio>
#include <cstring>

using namespace rvm;
using namespace rvm::net;

static int failures = 0;
static void Check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// Decoded frames of the client's first stream, or 0 if it has none yet: a
// failed earlier check must not turn into an out-of-bounds read.
static uint64_t FramesOf(const StreamClient& client) {
    const auto views = client.Views();
    return views.empty() ? 0 : views[0].frames;
}

static void TestCrypto() {
    printf("crypto\n");
    const Key a = DeriveMasterKey(L"correct horse battery staple");
    const Key b = DeriveMasterKey(L"correct horse battery staple");
    const Key c = DeriveMasterKey(L"correct horse battery stapl");
    Check(a == b, "same passphrase derives the same key");
    Check(a != c, "different passphrase derives a different key");

    uint8_t cr[kRandomBytes], sr[kRandomBytes];
    RandomBytes(cr, sizeof(cr));
    RandomBytes(sr, sizeof(sr));
    const Key s1 = DeriveSessionKey(a, cr, sr, Direction::kClientToServer);
    const Key s2 = DeriveSessionKey(a, sr, cr, Direction::kClientToServer);
    const Key s3 = DeriveSessionKey(a, cr, sr, Direction::kServerToClient);
    Check(s1 != a && s1 != s2, "session key depends on both randoms and differs from master");
    Check(s1 != s3, "each direction has its own session key");

    Cipher cipher;
    Check(cipher.Init(s1), "AES-GCM initialises");
    const uint8_t nonce[kNonceBytes] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    const uint8_t aad[4] = { 9, 9, 9, 9 };
    std::vector<uint8_t> plain(700);
    for (size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<uint8_t>(i * 7);
    std::vector<uint8_t> sealed(plain.size() + kTagBytes), opened(plain.size());
    Check(cipher.Seal(nonce, aad, sizeof(aad), plain.data(), plain.size(), sealed.data()), "seal");
    Check(cipher.Open(nonce, aad, sizeof(aad), sealed.data(), sealed.size(), opened.data()) &&
          opened == plain, "open recovers the plaintext");
    sealed[100] ^= 1;
    Check(!cipher.Open(nonce, aad, sizeof(aad), sealed.data(), sealed.size(), opened.data()),
          "a flipped ciphertext bit is rejected");
    sealed[100] ^= 1;
    const uint8_t badAad[4] = { 9, 9, 9, 8 };
    Check(!cipher.Open(nonce, badAad, sizeof(badAad), sealed.data(), sealed.size(), opened.data()),
          "a modified header (AAD) is rejected");
}

static void TestChannel() {
    printf("channel\n");
    const Key key = DeriveMasterKey(L"k");
    SecureChannel tx, rx;
    tx.SetKey(key);
    rx.SetKey(key);
    tx.BeginSend(0x1234);
    rx.BeginRecv(0x1234);

    std::vector<std::vector<uint8_t>> grams(5);
    for (int i = 0; i < 5; ++i) {
        const uint8_t body[2] = { static_cast<uint8_t>(i), 0xEE };
        Check(tx.Seal(body, 2, grams[i]), "seal datagram");
    }

    std::vector<uint8_t> plain;
    uint32_t sender = 0;
    Check(rx.Open(grams[0].data(), grams[0].size(), plain, sender) && plain[0] == 0 &&
          sender == 0x1234, "first datagram opens, sender identified");
    Check(rx.Open(grams[2].data(), grams[2].size(), plain, sender) && plain[0] == 2,
          "skipping ahead is fine");
    Check(rx.Open(grams[1].data(), grams[1].size(), plain, sender) && plain[0] == 1,
          "an out-of-order datagram inside the window is accepted");
    Check(!rx.Open(grams[1].data(), grams[1].size(), plain, sender),
          "a replayed datagram is rejected");
    Check(!rx.Open(grams[0].data(), grams[0].size(), plain, sender),
          "replaying the first one is rejected too");

    SecureChannel other;
    other.SetKey(key);
    other.BeginSend(0x9999);
    std::vector<uint8_t> foreign;
    other.Seal(reinterpret_cast<const uint8_t*>("x"), 1, foreign);
    Check(!rx.Open(foreign.data(), foreign.size(), plain, sender),
          "a datagram from a different session is rejected once bound");

    SecureChannel wrongKey;
    wrongKey.SetKey(DeriveMasterKey(L"not k"));
    wrongKey.BeginRecv(0x1234);
    Check(!wrongKey.Open(grams[3].data(), grams[3].size(), plain, sender),
          "the wrong key cannot open anything");

    // A forged datagram with a header and a tag but no body: made without the
    // key, with the largest counter, it must neither authenticate nor move
    // the replay window so that real datagrams are then refused.
    std::vector<uint8_t> forged(kHeaderBytes + kTagBytes, 0xAB);
    forged[0] = kMagic;
    forged[1] = kVersion;
    for (int i = 0; i < 4; ++i) forged[2 + i] = static_cast<uint8_t>(0x1234u >> (8 * i));
    for (int i = 0; i < 8; ++i) forged[6 + i] = 0xFF;
    Check(!rx.Open(forged.data(), forged.size(), plain, sender),
          "a forged body-less datagram is rejected");
    Check(rx.Open(grams[4].data(), grams[4].size(), plain, sender) && plain[0] == 4,
          "and genuine datagrams still open after it");
    std::vector<uint8_t> emptyGram;
    Check(!tx.Seal(nullptr, 0, emptyGram), "an empty plaintext is never sealed");

    std::vector<uint8_t> big(kMaxPlain + 1), gram;
    Check(!tx.Seal(big.data(), big.size(), gram), "oversized plaintext refused");
    big.resize(kMaxPlain);
    Check(tx.Seal(big.data(), big.size(), gram) && gram.size() == kMaxDatagram,
          "a full plaintext fits exactly in the datagram budget");
}

static void TestPacketizer() {
    printf("packetizer\n");
    FrameSender sender(7);
    FrameAssembler assembler;
    std::vector<FrameAssembler::Frame> got;
    std::vector<FrameAssembler::Missing> nacks;

    auto makeFrame = [](size_t n, uint8_t seed) {
        std::vector<uint8_t> f(n);
        for (size_t i = 0; i < n; ++i) f[i] = static_cast<uint8_t>(seed + i * 3);
        return f;
    };
    auto feed = [&](const std::vector<uint8_t>& pkt, uint64_t t) {
        Reader r(pkt.data(), pkt.size());
        uint8_t msg = 0;
        FrameHeader h;
        r.U8(msg);
        ReadFrameHeader(r, h);
        assembler.Accept(h, r.Ptr(), r.Left(), t, got);
    };

    // Frame 0: a 60 KB keyframe, delivered intact.
    const auto f0 = makeFrame(60000, 1);
    const auto& p0 = sender.Packetize(0, true, f0.data(), f0.size());
    Check(p0.size() == (f0.size() + kMaxChunk - 1) / kMaxChunk, "keyframe split into expected packet count");
    for (const auto& p : p0) feed(p, 0);
    Check(got.size() == 1 && got[0].data == f0 && got[0].keyframe, "keyframe reassembled exactly");
    got.clear();

    // Frame 1: one packet lost. No NACK until frame 2 starts arriving.
    const auto f1 = makeFrame(9000, 2);
    const auto p1 = sender.Packetize(1, false, f1.data(), f1.size());   // Copy: history may rotate.
    for (size_t i = 0; i < p1.size(); ++i) {
        if (i != 3) feed(p1[i], 10);
    }
    assembler.Poll(20, nacks);
    Check(got.empty() && nacks.empty(), "no NACK while the gap could still be in flight");

    const auto f2 = makeFrame(3000, 3);
    const auto p2 = sender.Packetize(2, false, f2.data(), f2.size());
    feed(p2[0], 30);
    assembler.Poll(36, nacks);
    Check(nacks.size() == 1 && nacks[0].frameSeq == 1 && nacks[0].indices == std::vector<uint16_t>{ 3 },
          "a NACK for exactly the missing packet once a later frame is seen");
    Check(got.empty(), "frame 2 is held back behind the incomplete frame 1");

    // Retransmit from the sender's history.
    const auto* resend = sender.Lookup(1, 3);
    Check(resend != nullptr, "sender still has the packet");
    feed(*resend, 40);
    for (size_t i = 1; i < p2.size(); ++i) feed(p2[i], 41);
    Check(got.size() == 2 && got[0].data == f1 && got[1].data == f2,
          "both frames delivered, in order, after the resend");
    got.clear();
    nacks.clear();

    // Frame 3 is never completed: it should be dropped and a keyframe demanded;
    // frames 4 and 5 (non-key) are discarded; frame 6 (key) restarts.
    const auto f3 = makeFrame(5000, 4);
    const auto p3 = sender.Packetize(3, false, f3.data(), f3.size());
    feed(p3[0], 100);
    const auto f4 = makeFrame(2000, 5);
    const auto p4 = sender.Packetize(4, false, f4.data(), f4.size());
    for (const auto& p : p4) feed(p, 110);
    assembler.Poll(200, nacks);
    Check(assembler.NeedKeyframe() && got.empty(), "hopeless frame dropped; keyframe now required");

    const auto f5 = makeFrame(2000, 6);
    const auto p5 = sender.Packetize(5, false, f5.data(), f5.size());
    for (const auto& p : p5) feed(p, 210);
    Check(got.empty(), "a non-keyframe cannot restart decoding");

    const auto f6 = makeFrame(40000, 7);
    const auto p6 = sender.Packetize(6, true, f6.data(), f6.size());
    for (const auto& p : p6) feed(p, 220);
    Check(got.size() == 1 && got[0].frameSeq == 6 && got[0].keyframe && got[0].data == f6,
          "the next keyframe restarts the chain, older frames discarded");
    Check(!assembler.NeedKeyframe(), "keyframe requirement cleared");

    const auto* gone = sender.Lookup(0, 0);
    Check(gone != nullptr, "history retains recent frames");
}

static bool ReadPixel(ID3D11Texture2D* tex, UINT x, UINT y, uint8_t bgra[4]) {
    auto& g = Gfx::Get();
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(g.d3d->CreateTexture2D(&d, nullptr, staging.put()))) return false;
    g.ctx->CopyResource(staging.get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    memcpy(bgra, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4, 4);
    g.ctx->Unmap(staging.get(), 0);
    return true;
}

static void TestCodec() {
    printf("codec\n");
    const std::wstring encoderName = HardwareEncoderName();
    if (encoderName.empty()) {
        printf("  SKIP  no hardware H.264 encoder on this machine\n");
        return;
    }
    printf("  encoder: %ls\n", encoderName.c_str());

    auto& g = Gfx::Get();
    const UINT W = 640, H = 360;

    VideoConverter toNv12, toBgra;
    Check(toNv12.Init() && toBgra.Init(), "video processors initialise");

    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = W; bd.Height = H; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> source, display;
    D3D11_TEXTURE2D_DESC nd = bd;
    nd.Format = DXGI_FORMAT_NV12;
    winrt::com_ptr<ID3D11Texture2D> nv12;
    Check(SUCCEEDED(g.d3d->CreateTexture2D(&bd, nullptr, source.put())) &&
          SUCCEEDED(g.d3d->CreateTexture2D(&bd, nullptr, display.put())) &&
          SUCCEEDED(g.d3d->CreateTexture2D(&nd, nullptr, nv12.put())), "textures created");

    H264Encoder encoder;
    Check(encoder.Init(W, H, 60, 6'000'000), "encoder initialises");
    H264Decoder decoder;
    Check(decoder.Init(), "decoder initialises");

    // Flat colour blocks, so a decoded pixel can be compared against what was
    // drawn without worrying about codec ringing at edges.
    std::vector<uint8_t> pixels(W * H * 4);
    int decodedFrames = 0, keyframes = 0;
    size_t totalBytes = 0;
    bool colourOk = true;

    std::lock_guard<std::mutex> lock(g.deviceMutex);
    for (int f = 0; f < 12; ++f) {
        // The last few frames share a shade: however many frames the encoder
        // and decoder hold back, the last decoded one shows it.
        const uint8_t shade = static_cast<uint8_t>(40 + (std::min)(f, 8) * 15);
        for (UINT y = 0; y < H; ++y) {
            for (UINT x = 0; x < W; ++x) {
                uint8_t* p = &pixels[(y * W + x) * 4];
                const bool left = x < W / 2;
                p[0] = left ? shade : 30;          // B
                p[1] = left ? 60 : shade;          // G
                p[2] = left ? 200 : 90;            // R
                p[3] = 255;
            }
        }
        g.ctx->UpdateSubresource(source.get(), 0, nullptr, pixels.data(), W * 4, 0);
        if (!toNv12.Convert(source.get(), 0, nullptr, nv12.get())) { colourOk = false; break; }

        std::vector<EncodedFrame> encoded;
        if (!encoder.EncodeSync(nv12.get(), encoded)) { colourOk = false; break; }

        for (const auto& e : encoded) {
            totalBytes += e.data.size();
            if (e.keyframe) ++keyframes;
            std::vector<DecodedFrame> decoded;
            if (!decoder.Decode(e.data.data(), e.data.size(), decoded)) { colourOk = false; break; }
            for (const auto& d : decoded) {
                ++decodedFrames;
                if (!toBgra.Convert(d.texture.get(), d.subresource, nullptr, display.get())) {
                    colourOk = false;
                }
            }
        }
    }

    Check(keyframes >= 1, "at least one keyframe was produced");
    Check(decodedFrames >= 8, "most frames decoded (low-latency pipeline delay is one or two)");
    printf("  %d encoded frames -> %d decoded, %zu bytes total, %d keyframes\n",
           12, decodedFrames, totalBytes, keyframes);

    // The last decoded frame should show the final left/right colours.
    uint8_t l[4]{}, r[4]{};
    if (decodedFrames > 0 && ReadPixel(display.get(), W / 4, H / 2, l) &&
        ReadPixel(display.get(), 3 * W / 4, H / 2, r)) {
        const auto within = [](int a, int b) { return std::abs(a - b) <= 24; };   // `near` is a Windows macro.
        const bool leftRed  = within(l[2], 200) && within(l[1], 60);
        const bool rightGrn = within(r[1], 40 + 8 * 15) && within(r[2], 90);
        printf("  left pixel  B%3d G%3d R%3d   right pixel  B%3d G%3d R%3d\n",
               l[0], l[1], l[2], r[0], r[1], r[2]);
        Check(leftRed && rightGrn, "decoded colours match the source within codec tolerance");
    } else {
        Check(false, "could not read back a decoded pixel");
    }
    Check(colourOk, "no stage of the GPU pipeline reported failure");
}

// Which frame sizes will the hardware encoder actually accept? Mirrors are
// arbitrary crops, so odd sizes are the normal case, not the exception.
static void TestEncoderSizes() {
    printf("encoder sizes\n");
    if (HardwareEncoderName().empty()) {
        printf("  SKIP  no hardware encoder\n");
        return;
    }
    auto& g = Gfx::Get();
    // Everything at or above the server's kMinEncodeDim floor must work; the
    // server upscales anything smaller before it reaches the encoder.
    const UINT sizes[][2] = {
        { 640, 360 }, { 1198, 712 }, { 1200, 720 }, { 1196, 712 }, { 1194, 712 },
        { 1000, 700 }, { 998, 600 }, { 336, 848 }, { 302, 848 }, { 256, 256 }, { 258, 300 },
        // Whole desktops: one 1080p or 4K screen, and wide multi-monitor layouts
        // as EncodeSize brings them within the 4096 ceiling.
        { 1920, 1080 }, { 3840, 2160 }, { 4096, 1152 }, { 4096, 768 },
    };
    bool allInit = true;
    for (const auto& sz : sizes) {
        H264Encoder enc;
        const bool init = enc.Init(sz[0], sz[1], 60, 4'000'000);
        bool encoded = false;
        if (init) {
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = sz[0]; nd.Height = sz[1]; nd.MipLevels = 1; nd.ArraySize = 1;
            nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            winrt::com_ptr<ID3D11Texture2D> nv12;
            if (SUCCEEDED(g.d3d->CreateTexture2D(&nd, nullptr, nv12.put()))) {
                std::vector<EncodedFrame> out;
                enc.EncodeSync(nv12.get(), out, 500);
                encoded = !out.empty();
            }
        }
        printf("  %5u x %-5u  init=%s  encoded=%s\n", sz[0], sz[1], init ? "yes" : "NO ",
               encoded ? "yes" : "NO ");
        if (!init || !encoded) allInit = false;
    }
    Check(allInit, "every size at or above the 256 px floor can be encoded");
}

// Every choice in the Streaming dialog's encoder preset list starts an
// encoder that produces frames. `--presets` measures what each one costs.
static void TestEncoderPresets() {
    printf("encoder presets\n");
    if (HardwareEncoderName().empty()) {
        printf("  SKIP  no hardware encoder\n");
        return;
    }
    auto& g = Gfx::Get();
    D3D11_TEXTURE2D_DESC nd{};
    nd.Width = 1280; nd.Height = 720; nd.MipLevels = 1; nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> nv12;
    if (!SUCCEEDED(g.d3d->CreateTexture2D(&nd, nullptr, nv12.put()))) {
        Check(false, "NV12 texture for the preset check");
        return;
    }
    const struct { UINT value; const char* name; } presets[] = {
        { 0, "Fastest" }, { H264Encoder::kEncoderDefault, "Balanced" }, { 100, "Quality" },
    };
    for (const auto& p : presets) {
        H264Encoder enc;
        bool encoded = false;
        if (enc.Init(1280, 720, 60, 4'000'000, p.value)) {
            std::vector<EncodedFrame> out;
            enc.EncodeSync(nv12.get(), out, 500);
            encoded = !out.empty();
        }
        char what[96];
        snprintf(what, sizeof what, "the %s preset encodes", p.name);
        Check(encoded, what);
    }
}

// The whole desktop as one texture the size of the virtual screen. Capture
// sends every monitor's current picture as soon as it starts, so frames come
// without anything on screen changing. Only frames are counted; no pixel is
// ever read back.
template <class Pred>
static bool WaitFor(Pred pred, int timeoutMs);

static void TestDesktopCapture() {
    printf("desktop capture\n");
    if (!CaptureSupported()) {
        printf("  SKIP  screen capture not supported here\n");
        return;
    }
    const RECT bounds = DesktopCapture::Bounds();
    std::atomic<int> frames{ 0 };
    std::atomic<bool> sizeOk{ true };
    DesktopCapture capture;
    const bool started = capture.Start([&](ID3D11Texture2D* texture, UINT w, UINT h) {
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        if (w != static_cast<UINT>(RectW(bounds)) || h != static_cast<UINT>(RectH(bounds)) ||
            d.Width != w || d.Height != h) {
            sizeOk = false;
        }
        ++frames;
    });
    Check(started, "desktop capture starts");
    if (!started) return;
    Check(WaitFor([&] { return frames.load() >= 1; }, 3000), "a desktop frame arrives");
    Check(sizeOk.load(), "desktop frames span the whole virtual screen");
    const SIZE content = capture.ContentSize();
    Check(content.cx == RectW(bounds) && content.cy == RectH(bounds), "content size is the virtual screen");
    printf("  %ldx%ld virtual screen, %d frame(s) so far\n", RectW(bounds), RectH(bounds),
           frames.load());
    capture.Stop();
    const int stopped = frames.load();
    Sleep(250);
    Check(frames.load() == stopped, "no frames after Stop");
}

// What the encoder spends. A mirror sends a frame whenever anything changes,
// so a pointer moving over a still desktop is the common case and must cost
// little; a whole screen scrolling must stay within the bitrate; and full
// pictures come only on request, never on a timer. The picture is text-like
// detail at 1080p, the kind a real desktop is made of.
static void TestRateControl() {
    printf("rate control\n");
    if (HardwareEncoderName().empty()) {
        printf("  SKIP  no hardware encoder\n");
        return;
    }
    auto& g = Gfx::Get();
    constexpr UINT W = 1920, H = 1080, kFps = 60, kBitrate = 8'000'000;

    // Dark "glyphs" in rows on a light page: detail that does not compress
    // away, like text. Deterministic, so every run sees the same picture.
    std::vector<uint32_t> page(static_cast<size_t>(W) * H);
    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            const bool glyphRow = (y % 18) < 12 && (x % 9) < 7;
            uint32_t h = (x / 2) * 73856093u ^ (y / 2) * 19349663u;
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            const bool ink = glyphRow && (h & 0x100u);
            page[static_cast<size_t>(y) * W + x] = ink ? 0xFF202428u : 0xFFF2F2F0u;
        }
    }
    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = W; bd.Height = H; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const D3D11_SUBRESOURCE_DATA init{ page.data(), W * 4, 0 };
    winrt::com_ptr<ID3D11Texture2D> background, work, pointer, nv12;
    // A second, unrelated page: alternating the two defeats prediction, the
    // worst case an encoder can be handed.
    std::vector<uint32_t> other(page.size());
    for (size_t i = 0; i < other.size(); ++i) {
        uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
        h ^= h >> 15;
        other[i] = (h & 0x40u) ? 0xFF303438u : 0xFFE8E8E4u;
    }
    const D3D11_SUBRESOURCE_DATA otherInit{ other.data(), W * 4, 0 };
    winrt::com_ptr<ID3D11Texture2D> background2;
    D3D11_TEXTURE2D_DESC pd = bd;
    pd.Width = 24; pd.Height = 24;
    std::vector<uint32_t> white(24 * 24, 0xFFFFFFFFu);
    const D3D11_SUBRESOURCE_DATA pinit{ white.data(), 24 * 4, 0 };
    D3D11_TEXTURE2D_DESC nd = bd;
    nd.Format = DXGI_FORMAT_NV12;
    const bool made = SUCCEEDED(g.d3d->CreateTexture2D(&bd, &init, background.put())) &&
                      SUCCEEDED(g.d3d->CreateTexture2D(&bd, &otherInit, background2.put())) &&
                      SUCCEEDED(g.d3d->CreateTexture2D(&bd, nullptr, work.put())) &&
                      SUCCEEDED(g.d3d->CreateTexture2D(&pd, &pinit, pointer.put())) &&
                      SUCCEEDED(g.d3d->CreateTexture2D(&nd, nullptr, nv12.put()));
    VideoConverter conv;
    H264Encoder enc;
    Check(made && conv.Init() && enc.Init(W, H, kFps, kBitrate), "encoder at 1920x1080, 60 fps, 8 Mbps");
    if (!made || !enc.Ready()) return;

    int keyframes = 0;
    size_t keyBytes = 0;
    // Draws frame `i`: the page scrolled by `scroll` pixels, a pointer at `px`.
    auto encode = [&](UINT scroll, UINT px, size_t& bytes, ID3D11Texture2D* source = nullptr) {
        ID3D11Texture2D* src = source ? source : background.get();
        {
            std::lock_guard<std::mutex> device(g.deviceMutex);
            const UINT s = scroll % W;
            const D3D11_BOX right{ s, 0, 0, W, H, 1 };
            g.ctx->CopySubresourceRegion(work.get(), 0, 0, 0, 0, src, 0, &right);
            if (s > 0) {
                const D3D11_BOX left{ 0, 0, 0, s, H, 1 };
                g.ctx->CopySubresourceRegion(work.get(), 0, W - s, 0, 0, src, 0, &left);
            }
            g.ctx->CopySubresourceRegion(work.get(), 0, px % (W - 24), 500, 0, pointer.get(), 0, nullptr);
            conv.Convert(work.get(), 0, nullptr, nv12.get());
        }
        std::vector<EncodedFrame> out;
        enc.EncodeSync(nv12.get(), out, 500);
        for (const auto& f : out) {
            bytes += f.data.size();
            if (f.keyframe) {
                ++keyframes;
                keyBytes += f.data.size();
            }
        }
    };

    // The pointer wandering over a still page, for longer than the old
    // four-second keyframe interval.
    constexpr int kPointerFrames = kFps * 5;
    size_t first = 0, pointerBytes = 0;
    encode(0, 0, first);   // The opening keyframe.
    keyBytes = 0;
    for (int i = 1; i <= kPointerFrames; ++i) encode(0, static_cast<UINT>(i * 7), pointerBytes);
    const int timedKeyframes = keyframes - 1;
    const double perPointerFrame =
        static_cast<double>(pointerBytes - keyBytes) / (kPointerFrames - timedKeyframes);
    const double budgetPerFrame = kBitrate / 8.0 / kFps;   // What constant bitrate spends.
    printf("  opening keyframe %zu bytes; %d more on a timer in 5 s; pointer-only frames "
           "average %.0f bytes (constant-bitrate budget %.0f)\n",
           first, timedKeyframes, perPointerFrame, budgetPerFrame);
    Check(keyframes == 1, "no keyframes on a timer: only the opening one in 5 s of frames");
    Check(perPointerFrame < budgetPerFrame / 4, "a pointer moving over a still page costs little");

    // One second of the whole page scrolling: everything changes, every frame.
    size_t scrollBytes = 0;
    for (int i = 1; i <= static_cast<int>(kFps); ++i) encode(static_cast<UINT>(i * 6), 0, scrollBytes);
    const double scrollMbps = scrollBytes * 8.0 / 1e6;
    printf("  one second of scrolling: %.2f Mbps (limit %.0f)\n", scrollMbps, kBitrate / 1e6);
    Check(scrollMbps <= kBitrate / 1e6 * 1.25, "a whole screen scrolling stays within the bitrate");

    // The worst case, reported rather than checked: every frame unrelated to
    // the one before. No rate-control mode holds this to the limit: the
    // encoder will not drop quality further, so each frame stays large.
    // Only sending fewer frames could, which the frame-rate cap does.
    size_t worstBytes = 0;
    for (int i = 1; i <= static_cast<int>(kFps); ++i) {
        encode(static_cast<UINT>(i * 37), 0, worstBytes, (i & 1) ? background2.get() : background.get());
    }
    const double worstMbps = worstBytes * 8.0 / 1e6;
    printf("  one second of unrelated pictures: %.2f Mbps (limit %.0f)\n", worstMbps, kBitrate / 1e6);
}

// The mirror renderer hands the server its cache texture, which is bound as a
// shader resource only and much larger than the crop. The converter must
// accept exactly that, not just the render-target-bound sources above.
static void TestConverterSources() {
    printf("converter sources\n");
    auto& g = Gfx::Get();
    VideoConverter conv;
    Check(conv.Init(), "video processor initialises");

    const UINT srcW = 1522, srcH = 1360, cropW = 354, cropH = 418;
    D3D11_TEXTURE2D_DESC nd{};
    nd.Width = cropW; nd.Height = cropH; nd.MipLevels = 1; nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> nv12;
    Check(SUCCEEDED(g.d3d->CreateTexture2D(&nd, nullptr, nv12.put())), "NV12 target created");

    const struct { UINT bind; const char* name; } variants[] = {
        { D3D11_BIND_SHADER_RESOURCE, "shader-resource only (renderer cache)" },
        { D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "render target" },
    };
    for (const auto& v : variants) {
        D3D11_TEXTURE2D_DESC bd{};
        bd.Width = srcW; bd.Height = srcH; bd.MipLevels = 1; bd.ArraySize = 1;
        bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
        bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = v.bind;
        winrt::com_ptr<ID3D11Texture2D> src;
        const bool created = SUCCEEDED(g.d3d->CreateTexture2D(&bd, nullptr, src.put()));
        Check(created, v.name);
        if (!created) continue;
        const RECT crop{ 900, 700, 900 + static_cast<LONG>(cropW), 700 + static_cast<LONG>(cropH) };
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) ok = conv.Convert(src.get(), 0, &crop, nv12.get());
        char what[160];
        snprintf(what, sizeof what, "%ux%u %s source converts cropped into NV12", srcW, srcH, v.name);
        Check(ok, what);
    }
}

template <class Pred>
static bool WaitFor(Pred pred, int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (pred()) return true;
        Sleep(10);
    }
    return pred();
}

// Server and client in one process over real UDP on the loopback interface:
// handshake, list, subscribe, encode, packetise, reassemble, decode, display.
static void TestLoopback() {
    printf("loopback\n");
    if (HardwareEncoderName().empty()) {
        printf("  SKIP  no hardware H.264 encoder on this machine\n");
        return;
    }
    auto& g = Gfx::Get();

    StreamSettings settings;
    settings.enabled = true;
    settings.port = 0;   // Any free port, read back once started.
    settings.key = L"loopback-test-key";
    settings.bitrateKbps = 4000;
    settings.fps = 60;

    StreamServer server;
    Check(server.Start(settings), "server starts on the loopback port");
    const uint16_t kPort = server.Port();
    server.SetMirrorList({ { 1, L"Test mirror", 640, 360 } });

    StreamClient wrongKey;
    wrongKey.Connect(L"127.0.0.1", kPort, L"not-the-key", nullptr);
    Check(!WaitFor([&] { return wrongKey.Connected(); }, 1500),
          "a client with the wrong key is never admitted");
    wrongKey.Disconnect();

    StreamClient client;
    client.Connect(L"127.0.0.1", kPort, L"loopback-test-key", nullptr);
    Check(WaitFor([&] { return client.Connected(); }, 5000),
          "client completes the encrypted handshake");
    Check(WaitFor([&] { return client.Mirrors().size() == 1; }, 3000) &&
          client.Mirrors()[0].name == L"Test mirror" && client.Mirrors()[0].id == 1,
          "client receives the mirror list");
    Check(server.ClientCount() == 1, "server counts exactly one client");

    client.SetSubscribed(1, true);
    Check(WaitFor([&] { return client.IsSubscribed(1); }, 500), "subscription recorded");
    Sleep(100);   // Let the subscribe reach the server before the first frame.

    // An 800x500 source; the crop selects a 640x360 window of it.
    const UINT SW = 800, SH = 500;
    const RECT crop{ 80, 70, 720, 430 };
    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = SW; bd.Height = SH; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> source;
    Check(SUCCEEDED(g.d3d->CreateTexture2D(&bd, nullptr, source.put())), "source texture");

    std::vector<uint8_t> pixels(SW * SH * 4);
    for (int f = 0; f < 60; ++f) {
        const uint8_t shade = static_cast<uint8_t>(60 + (f % 20) * 8);
        for (UINT y = 0; y < SH; ++y) {
            for (UINT x = 0; x < SW; ++x) {
                uint8_t* p = &pixels[(y * SW + x) * 4];
                p[0] = 40; p[1] = shade; p[2] = 190; p[3] = 255;   // B G R: a warm orange-ish.
            }
        }
        {
            std::lock_guard<std::mutex> lock(g.deviceMutex);
            g.ctx->UpdateSubresource(source.get(), 0, nullptr, pixels.data(), SW * 4, 0);
            server.SubmitFrame(1, source.get(), crop);
        }
        Sleep(16);
    }

    const bool gotFrames = WaitFor([&] {
        auto v = client.Views();
        return !v.empty() && v[0].frames >= 5;
    }, 3000);
    Check(gotFrames, "frames travel server -> client and decode (>= 5 decoded)");

    auto views = client.Views();
    if (!views.empty() && views[0].texture) {
        printf("  decoded %llu frames at %ux%u\n",
               static_cast<unsigned long long>(views[0].frames), views[0].width, views[0].height);
        Check(views[0].width == 640 && views[0].height == 360, "decoded size equals the crop, not the source");
        uint8_t px[4]{};
        std::lock_guard<std::mutex> lock(g.deviceMutex);
        if (ReadPixel(views[0].texture.get(), 320, 180, px)) {
            printf("  decoded pixel B%3d G%3d R%3d\n", px[0], px[1], px[2]);
            Check(std::abs(px[2] - 190) <= 24 && std::abs(px[0] - 40) <= 24,
                  "decoded colour matches what the server was fed");
        } else {
            Check(false, "read back a decoded pixel");
        }
    }

    Check(WaitFor([&] { return client.RttMs() >= 0; }, 3000), "ping/pong yields a round-trip time");
    printf("  loopback RTT %d ms\n", client.RttMs());

    // A still source: capture delivers nothing, so the server must be able to
    // ask for the mirror's last frame when a client subscribes.
    int requests = 0;
    server.SetFrameRequester([&](uint32_t id) {
        ++requests;
        std::lock_guard<std::mutex> lock(g.deviceMutex);
        server.SubmitFrame(id, source.get(), crop);
    });
    client.SetSubscribed(1, false);
    Sleep(150);
    const uint64_t before = FramesOf(client);
    client.SetSubscribed(1, true);
    Check(WaitFor([&] { return requests > 0; }, 2000), "subscribing asks the mirror for a frame");
    Check(WaitFor([&] {
              auto v = client.Views();
              return !v.empty() && v[0].frames > 0;
          }, 3000), "a subscriber of a still source gets its first frame anyway");
    (void)before;

    // A single isolated frame must also arrive, not wait for a successor.
    const uint64_t single = FramesOf(client);
    {
        std::lock_guard<std::mutex> lock(g.deviceMutex);
        server.SubmitFrame(1, source.get(), crop);
    }
    Check(WaitFor([&] { return FramesOf(client) > single; }, 2000),
          "one lone frame is delivered without waiting for the next");

    // The server goes away and comes back: the client must reconnect on its
    // own and restore the subscription without being told.
    const uint64_t beforeRestart = FramesOf(client);
    server.Stop();
    Check(WaitFor([&] { return !client.Connected(); }, 15000), "client notices the server is gone");
    Check(client.IsSubscribed(1), "the wish to watch mirror 1 survives the outage");
    settings.port = kPort;
    Check(server.Start(settings), "server restarts on the same port");
    server.SetMirrorList({ { 1, L"Test mirror", 640, 360 } });
    server.SetFrameRequester([&](uint32_t id) {
        std::lock_guard<std::mutex> lock(g.deviceMutex);
        server.SubmitFrame(id, source.get(), crop);
    });
    Check(WaitFor([&] { return client.Connected(); }, 15000), "client reconnects by itself");
    Check(WaitFor([&] {
              auto v = client.Views();
              return !v.empty() && v[0].frames > beforeRestart;
          }, 5000), "subscription restored: frames flow again after the restart");

    client.Disconnect();
    Check(WaitFor([&] { return server.ClientCount() == 0; }, 2000), "BYE removes the client from the server");
    server.Stop();
    Check(!server.Running(), "server stops");
}

// A client that speaks the wire protocol by hand, so it can do what the real
// client never would: replay a HELLO, flood requests, ask for nonsense.
struct RawPeer {
    UdpSocket socket;
    Endpoint server;
    Key master{};
    SecureChannel channel;
    uint32_t session = 0;
    uint8_t random[kRandomBytes]{};
    std::vector<uint8_t> hello;   // The sealed HELLO, kept for replaying.

    bool Open(uint16_t port, const std::wstring& key) {
        master = DeriveMasterKey(key);
        RandomBytes(random, kRandomBytes);
        do { RandomBytes(&session, sizeof(session)); } while (session == 0);
        return socket.Open(0) && UdpSocket::Resolve(L"127.0.0.1", port, server);
    }

    bool Handshake() {
        SecureChannel h;
        h.SetKey(master);
        uint64_t start = 0;
        RandomBytes(&start, sizeof(start));
        h.BeginSend(session, start >> 1);
        Writer w;
        w.U8(static_cast<uint8_t>(Msg::Hello));
        w.U16(kVersion);
        w.Bytes(random, kRandomBytes);
        if (!h.Seal(w.Data().data(), w.Data().size(), hello)) return false;
        socket.SendTo(server, hello.data(), hello.size());

        std::vector<uint8_t> buf(kMaxDatagram + 64), plain;
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (GetTickCount64() < deadline) {
            Endpoint from;
            const int n = socket.Receive(buf.data(), buf.size(), from, 50);
            if (n <= 0) continue;
            SecureChannel welcome;
            welcome.SetKey(master);
            uint32_t serverSession = 0;
            if (!welcome.Open(buf.data(), static_cast<size_t>(n), plain, serverSession) ||
                plain.size() < 1 + 2 * kRandomBytes || plain[0] != static_cast<uint8_t>(Msg::Welcome) ||
                memcmp(plain.data() + 1 + kRandomBytes, random, kRandomBytes) != 0) {
                continue;
            }
            channel.SetKeys(DeriveSessionKey(master, random, plain.data() + 1, Direction::kClientToServer),
                            DeriveSessionKey(master, random, plain.data() + 1, Direction::kServerToClient));
            channel.BeginSend(session);
            channel.BeginRecv(serverSession);
            return true;
        }
        return false;
    }

    void Send(const Writer& w) {
        std::vector<uint8_t> datagram;
        if (channel.Seal(w.Data().data(), w.Data().size(), datagram)) {
            socket.SendTo(server, datagram.data(), datagram.size());
        }
    }

    void SendRaw(const std::vector<uint8_t>& datagram) {
        socket.SendTo(server, datagram.data(), datagram.size());
    }

    // Every message that opens under the session key within `ms`.
    std::vector<std::vector<uint8_t>> Collect(int ms) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<uint8_t> buf(kMaxDatagram + 64);
        const ULONGLONG deadline = GetTickCount64() + ms;
        while (GetTickCount64() < deadline) {
            Endpoint from;
            const int n = socket.Receive(buf.data(), buf.size(), from, 10);
            if (n <= 0) continue;
            std::vector<uint8_t> plain;
            uint32_t sender = 0;
            if (channel.Open(buf.data(), static_cast<size_t>(n), plain, sender) && !plain.empty()) {
                out.push_back(std::move(plain));
            }
        }
        return out;
    }

    bool PingPong() {
        Writer w;
        w.U8(static_cast<uint8_t>(Msg::Ping));
        w.U64(42);
        Send(w);
        for (const auto& m : Collect(500)) {
            if (m[0] == static_cast<uint8_t>(Msg::Pong)) return true;
        }
        return false;
    }
};

// The real client against a scripted server: a WELCOME that does not echo the
// client's own random (a recorded one, say) must be ignored; the right one
// must connect.
static void TestWelcomeBinding() {
    printf("welcome binding\n");
    const std::wstring key = L"binding-test-key";
    const Key master = DeriveMasterKey(key);

    UdpSocket fake;
    Check(fake.Open(0), "scripted server socket");
    StreamClient client;
    client.Connect(L"127.0.0.1", fake.LocalPort(), key, nullptr);

    // Wait for the client's HELLO and read its session and random.
    SecureChannel hello;
    hello.SetKey(master);
    std::vector<uint8_t> buf(kMaxDatagram + 64), plain;
    Endpoint from;
    uint32_t clientSession = 0;
    uint8_t clientRandom[kRandomBytes]{};
    bool gotHello = false;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!gotHello && GetTickCount64() < deadline) {
        const int n = fake.Receive(buf.data(), buf.size(), from, 50);
        if (n > 0 && hello.Open(buf.data(), static_cast<size_t>(n), plain, clientSession) &&
            plain.size() >= 3 + kRandomBytes && plain[0] == static_cast<uint8_t>(Msg::Hello)) {
            memcpy(clientRandom, plain.data() + 3, kRandomBytes);
            gotHello = true;
        }
    }
    Check(gotHello, "client sends HELLO");

    const auto welcome = [&](const uint8_t* echo) {
        SecureChannel w;
        w.SetKey(master);
        uint64_t start = 0;
        RandomBytes(&start, sizeof(start));
        w.BeginSend(0x5151, start >> 1);
        uint8_t serverRandom[kRandomBytes];
        RandomBytes(serverRandom, kRandomBytes);
        Writer m;
        m.U8(static_cast<uint8_t>(Msg::Welcome));
        m.Bytes(serverRandom, kRandomBytes);
        m.Bytes(echo, kRandomBytes);
        std::vector<uint8_t> gram;
        w.Seal(m.Data().data(), m.Data().size(), gram);
        fake.SendTo(from, gram.data(), gram.size());
    };

    const uint8_t stranger[kRandomBytes] = { 1, 2, 3 };
    welcome(stranger);
    Check(!WaitFor([&] { return client.Connected(); }, 800),
          "a WELCOME answering someone else's HELLO is ignored");
    welcome(clientRandom);
    Check(WaitFor([&] { return client.Connected(); }, 2000), "the WELCOME answering its own HELLO connects");
    client.Disconnect();
}

static void SendId(RawPeer& peer, Msg msg, uint32_t id) {
    Writer w;
    w.U8(static_cast<uint8_t>(msg));
    w.U32(id);
    peer.Send(w);
}

// Everything the audit found a client could abuse, tried against a live
// server: each must stay bounded.
static void TestHostileClients() {
    printf("hostile clients\n");
    if (HardwareEncoderName().empty()) {
        printf("  SKIP  no hardware H.264 encoder on this machine\n");
        return;
    }
    auto& g = Gfx::Get();
    const std::wstring key = L"hostile-test-key";

    StreamSettings settings;
    settings.enabled = true;
    settings.port = 0;   // Any free port, read back once started.
    settings.key = key;
    settings.bitrateKbps = 4000;
    settings.fps = 60;

    // 100 mirrors with long names: more than one datagram could carry whole.
    std::vector<MirrorInfo> list;
    for (uint32_t id = 1; id <= 100; ++id) {
        list.push_back({ id, L"Mirror " + std::to_wstring(id) + L" " + std::wstring(200, L'x'), 640, 360 });
    }

    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = 640; bd.Height = 360; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> source;
    Check(SUCCEEDED(g.d3d->CreateTexture2D(&bd, nullptr, source.put())), "source texture");
    const RECT crop{ 0, 0, 640, 360 };

    StreamServer server;
    std::mutex requestsMutex;
    std::map<uint32_t, int> requests;
    server.SetFrameRequester([&](uint32_t id) {
        {
            std::lock_guard lock(requestsMutex);
            ++requests[id];
        }
        std::lock_guard<std::mutex> device(g.deviceMutex);
        server.SubmitFrame(id, source.get(), crop);
    });
    const auto requestsFor = [&](uint32_t id) {
        std::lock_guard lock(requestsMutex);
        return requests[id];
    };
    Check(server.Start(settings), "server starts");
    const uint16_t kPort = server.Port();
    server.SetMirrorList(list);

    RawPeer a;
    Check(a.Open(kPort, key) && a.Handshake(), "raw peer completes the handshake");
    Check(server.ClientCount() == 0, "a HELLO alone takes no client slot");
    Check(a.PingPong() && server.ClientCount() == 1,
          "the first message under the session key admits the client");

    // Replays of that HELLO, from elsewhere and from the client's own address.
    RawPeer b;
    b.Open(kPort, key);
    for (int i = 0; i < 20; ++i) {
        b.SendRaw(a.hello);
        a.SendRaw(a.hello);
    }
    Sleep(300);
    Check(server.ClientCount() == 1, "replayed HELLOs are never admitted");
    Check(a.PingPong(), "and do not displace the real client");

    // The list must arrive whole, names shortened to fit one datagram.
    {
        Writer w;
        w.U8(static_cast<uint8_t>(Msg::ListReq));
        a.Send(w);
        size_t count = 0;
        for (const auto& m : a.Collect(500)) {
            if (m[0] == static_cast<uint8_t>(Msg::ListResp) && m.size() > 1) count = m[1];
        }
        Check(count == 100, "a 100-mirror list with long names still arrives");
    }

    // Subscribing to mirrors that do not exist allocates nothing.
    for (uint32_t id = 1000; id < 3000; ++id) SendId(a, Msg::Subscribe, id);
    Sleep(300);
    Check(server.StreamCount() == 0, "subscribing to unknown ids creates no streams");

    // One real subscription, then a keyframe flood.
    SendId(a, Msg::Subscribe, 1);
    Check(WaitFor([&] { return requestsFor(1) >= 1; }, 1000), "a subscription asks for a frame");
    Sleep(400);
    const int before = requestsFor(1);
    for (int i = 0; i < 300; ++i) SendId(a, Msg::KeyframeReq, 1);
    Sleep(700);
    const int flood = requestsFor(1) - before;
    printf("  300 keyframe requests -> %d frame requests\n", flood);
    // 700 ms at one keyframe per 250 ms is 3 or 4; the margin is for a busy
    // machine's scheduling, far below the 300 asked for.
    Check(flood >= 1 && flood <= 6, "keyframe requests are rate-limited per stream");
    // A keyframe request for a listed mirror the client is not subscribed to
    // means its Subscribe was lost: the request subscribes it instead.
    SendId(a, Msg::KeyframeReq, 7);
    Check(WaitFor([&] { return requestsFor(7) >= 1 && server.StreamCount() == 2; }, 1000),
          "a keyframe request for a listed mirror stands in for a lost Subscribe");
    SendId(a, Msg::KeyframeReq, 5000);
    Sleep(100);
    Check(server.StreamCount() == 2, "but never for a mirror that is not listed");

    // A NACK naming one packet 500 times gets it once.
    {
        uint32_t seq = 0;
        for (int attempt = 0; attempt < 20 && seq == 0; ++attempt) {
            for (const auto& m : a.Collect(100)) {
                Reader r(m.data(), m.size());
                uint8_t type = 0;
                FrameHeader h;
                if (r.U8(type) && type == static_cast<uint8_t>(Msg::Frame) && ReadFrameHeader(r, h) &&
                    h.mirrorId == 1) {
                    seq = h.frameSeq;
                }
            }
        }
        Check(seq != 0, "frames arrive for the subscribed mirror");

        const auto nack = [&](uint32_t mirror) {
            Writer w;
            w.U8(static_cast<uint8_t>(Msg::Nack));
            w.U32(mirror);
            w.U32(seq);
            w.U16(500);
            for (int i = 0; i < 500; ++i) w.U16(0);
            a.Send(w);
            int resent = 0;
            for (const auto& m : a.Collect(300)) {
                Reader r(m.data(), m.size());
                uint8_t type = 0;
                FrameHeader h;
                if (r.U8(type) && type == static_cast<uint8_t>(Msg::Frame) && ReadFrameHeader(r, h) &&
                    h.frameSeq == seq && h.pktIdx == 0) {
                    ++resent;
                }
            }
            return resent;
        };
        Check(nack(1) == 1, "a NACK repeating one index resends it once");
        Check(nack(5) == 0, "a NACK for an unwatched mirror resends nothing");
    }

    // Subscriptions are capped per client.
    for (uint32_t id = 2; id <= 100; ++id) SendId(a, Msg::Subscribe, id);
    Check(WaitFor([&] { return server.StreamCount() == 64; }, 2000) && server.StreamCount() == 64,
          "one client can hold at most 64 subscriptions");

    // Leaving frees everything; so does a mirror leaving the list.
    for (uint32_t id = 1; id <= 100; ++id) SendId(a, Msg::Unsubscribe, id);
    Check(WaitFor([&] { return server.StreamCount() == 0; }, 2000), "unsubscribed streams are freed");
    SendId(a, Msg::Subscribe, 1);
    Check(WaitFor([&] { return server.StreamCount() == 1; }, 1000), "resubscribe");
    server.SetMirrorList({});
    Check(WaitFor([&] { return server.StreamCount() == 0; }, 3000),
          "a mirror leaving the list frees its stream");

    server.Stop();
}

// A 60 fps source against a 10 fps cap: about 10 frames a second get through,
// evenly, and when the source stops, its last picture is still delivered.
static void TestFrameRateCap() {
    printf("frame-rate cap\n");
    if (HardwareEncoderName().empty()) {
        printf("  SKIP  no hardware H.264 encoder on this machine\n");
        return;
    }
    auto& g = Gfx::Get();

    StreamSettings settings;
    settings.enabled = true;
    settings.port = 0;   // Any free port, read back once started.
    settings.key = L"cap-test-key";
    settings.bitrateKbps = 4000;
    settings.fps = 10;

    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = 640; bd.Height = 360; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> source;
    g.d3d->CreateTexture2D(&bd, nullptr, source.put());
    const RECT crop{ 0, 0, 640, 360 };

    StreamServer server;
    std::atomic<int> requests{ 0 };
    server.SetFrameRequester([&](uint32_t id) {
        ++requests;
        std::lock_guard<std::mutex> device(g.deviceMutex);
        server.SubmitFrame(id, source.get(), crop);
    });
    Check(server.Start(settings), "server starts with a 10 fps cap");
    const uint16_t kPort = server.Port();
    server.SetMirrorList({ { 1, L"Capped", 640, 360 } });

    StreamClient client;
    client.Connect(L"127.0.0.1", kPort, L"cap-test-key", nullptr);
    Check(WaitFor([&] { return client.Connected() && !client.Mirrors().empty(); }, 5000), "client connects");
    client.SetSubscribed(1, true);
    Check(WaitFor([&] {
              auto v = client.Views();
              return !v.empty() && v[0].frames > 0;
          }, 3000), "first frame arrives");
    Sleep(300);

    const uint64_t before = FramesOf(client);
    const ULONGLONG start = GetTickCount64();
    std::vector<uint8_t> pixels(640 * 360 * 4);
    for (int f = 0; GetTickCount64() - start < 2000; ++f) {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = static_cast<uint8_t>(f * 7); pixels[i + 1] = 100; pixels[i + 2] = 50; pixels[i + 3] = 255;
        }
        {
            std::lock_guard<std::mutex> device(g.deviceMutex);
            g.ctx->UpdateSubresource(source.get(), 0, nullptr, pixels.data(), 640 * 4, 0);
            server.SubmitFrame(1, source.get(), crop);
        }
        Sleep(16);
    }
    // Two frames back to back: the second is certainly inside the cap's gap,
    // so the source's final picture is one the cap skipped.
    const int requestsBefore = requests.load();
    for (int i = 0; i < 2; ++i) {
        std::lock_guard<std::mutex> device(g.deviceMutex);
        server.SubmitFrame(1, source.get(), crop);
    }
    Sleep(400);
    const uint64_t delivered = FramesOf(client) - before;
    printf("  2 s of a ~60 fps source -> %llu frames\n", static_cast<unsigned long long>(delivered));
    // 20 expected; the margin absorbs timer jitter on a loaded machine while
    // still far from the ~120 an uncapped source would deliver.
    Check(delivered >= 14 && delivered <= 30, "about 10 frames a second get through");
    Check(requests.load() > requestsBefore,
          "a picture the cap skipped is fetched again once the source goes still");

    client.Disconnect();
    server.Stop();
}

// End to end at 120 fps: a source changing every 8.3 ms, through the server's
// capture tee, encoder, packetiser and the client's decoder. Paced with the
// performance counter, since Sleep() here rounds to 15.6 ms.
static void TestHighFrameRate() {
    printf("high frame rate\n");
    if (HardwareEncoderName().empty()) {
        printf("  SKIP  no hardware H.264 encoder on this machine\n");
        return;
    }
    auto& g = Gfx::Get();

    StreamSettings settings;
    settings.enabled = true;
    settings.port = 0;   // Any free port, read back once started.
    settings.key = L"fast-test-key";
    settings.bitrateKbps = 20000;
    settings.fps = 120;

    const UINT W = 1280, H = 720;
    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = W; bd.Height = H; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> source;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    g.d3d->CreateTexture2D(&bd, nullptr, source.put());
    g.d3d->CreateRenderTargetView(source.get(), nullptr, rtv.put());
    const RECT crop{ 0, 0, static_cast<LONG>(W), static_cast<LONG>(H) };

    StreamServer server;
    server.SetFrameRequester([&](uint32_t id) {
        std::lock_guard<std::mutex> device(g.deviceMutex);
        server.SubmitFrame(id, source.get(), crop);
    });
    Check(server.Start(settings), "server starts at 120 fps, 20 Mbps");
    const uint16_t kPort = server.Port();
    server.SetMirrorList({ { 1, L"Fast", W, H } });

    StreamClient client;
    client.Connect(L"127.0.0.1", kPort, L"fast-test-key", nullptr);
    Check(WaitFor([&] { return client.Connected() && !client.Mirrors().empty(); }, 5000), "client connects");
    client.SetSubscribed(1, true);
    Check(WaitFor([&] {
              auto v = client.Views();
              return !v.empty() && v[0].frames > 0;
          }, 3000), "first frame arrives");
    Sleep(300);

    LARGE_INTEGER f{}, t{};
    QueryPerformanceFrequency(&f);
    const auto nowMs = [&] {
        QueryPerformanceCounter(&t);
        return t.QuadPart * 1000.0 / f.QuadPart;
    };
    const uint64_t before = FramesOf(client);
    const double start = nowMs();
    int submitted = 0;
    for (; nowMs() - start < 2000.0; ++submitted) {
        {
            std::lock_guard<std::mutex> device(g.deviceMutex);
            const float c[4] = { (submitted % 120) / 120.0f, 0.4f, 0.6f, 1.0f };
            g.ctx->ClearRenderTargetView(rtv.get(), c);
            server.SubmitFrame(1, source.get(), crop);
        }
        const double due = start + (submitted + 1) * (1000.0 / 120.0);
        while (nowMs() < due) {}
    }
    Sleep(300);
    const uint64_t delivered = FramesOf(client) - before;
    printf("  %d frames at 120 fps over 2 s -> %llu decoded by the client\n", submitted,
           static_cast<unsigned long long>(delivered));
    Check(delivered >= static_cast<uint64_t>(submitted) * 3 / 4,
          "at least 75% of a 120 fps stream reaches the client");

    client.Disconnect();
    server.Stop();
}

// `--bench`: how fast can this machine's encoder path go? Feeds changing
// frames back to back, the way the server's encode thread does, and reports
// the time each Encode() call takes and the frame rate that implies.
static int Bench() {
    Gfx::Get().Init();
    auto& g = Gfx::Get();

    LARGE_INTEGER f{}, a{}, b{};
    QueryPerformanceFrequency(&f);
    const auto ms = [&](LARGE_INTEGER x, LARGE_INTEGER y) {
        return (y.QuadPart - x.QuadPart) * 1000.0 / f.QuadPart;
    };

    QueryPerformanceCounter(&a);
    for (int i = 0; i < 50; ++i) Sleep(1);
    QueryPerformanceCounter(&b);
    printf("Sleep(1) actually sleeps %.2f ms\n", ms(a, b) / 50);

    const UINT W = 1280, H = 720;
    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = W; bd.Height = H; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_TEXTURE2D_DESC nd = bd;
    nd.Format = DXGI_FORMAT_NV12;
    winrt::com_ptr<ID3D11Texture2D> bgra, nv12;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    g.d3d->CreateTexture2D(&bd, nullptr, bgra.put());
    g.d3d->CreateTexture2D(&nd, nullptr, nv12.put());
    g.d3d->CreateRenderTargetView(bgra.get(), nullptr, rtv.put());

    VideoConverter conv;
    H264Encoder enc;
    if (!conv.Init() || !enc.Init(W, H, 120, 20'000'000)) {
        printf("encoder unavailable\n");
        return 1;
    }
    printf("encoder: %ls at %ux%u\n", enc.Name().c_str(), W, H);

    constexpr int kFrames = 240;
    size_t outputs = 0;
    double slowest = 0;
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    for (int i = 0; i < kFrames; ++i) {
        {
            std::lock_guard<std::mutex> device(g.deviceMutex);
            const float c[4] = { (i % 60) / 60.0f, 0.3f, 1.0f - (i % 60) / 60.0f, 1.0f };
            g.ctx->ClearRenderTargetView(rtv.get(), c);
            conv.Convert(bgra.get(), 0, nullptr, nv12.get());
        }
        std::vector<EncodedFrame> out;
        QueryPerformanceCounter(&a);
        enc.EncodeSync(nv12.get(), out);
        QueryPerformanceCounter(&b);
        slowest = (std::max)(slowest, ms(a, b));
        outputs += out.size();
    }
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    const double total = ms(start, end);
    printf("%d frames in %.0f ms: %.2f ms per Encode (slowest %.2f), %zu outputs\n", kFrames, total,
           total / kFrames, slowest, outputs);
    printf("=> the encode thread tops out near %.0f fps\n", kFrames * 1000.0 / total);
    return 0;
}

// `--live`: connect a headless client to the app's own running server, using
// the key from its settings file, and report what arrives. Splits a "no
// frames" report into a server-side or client-side problem.
// Luma PSNR between two NV12 pictures of the same size, read back through
// staging copies. Only the Y plane: text sharpness lives there.
static double LumaPsnr(ID3D11Texture2D* a, UINT aSlice, ID3D11Texture2D* b, UINT w, UINT h) {
    auto& g = Gfx::Get();
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_NV12; sd.SampleDesc = { 1, 0 };
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> sa, sb;
    if (FAILED(g.d3d->CreateTexture2D(&sd, nullptr, sa.put())) ||
        FAILED(g.d3d->CreateTexture2D(&sd, nullptr, sb.put()))) {
        return 0.0;
    }
    std::lock_guard<std::mutex> device(g.deviceMutex);
    const D3D11_BOX box{ 0, 0, 0, w, h, 1 };
    g.ctx->CopySubresourceRegion(sa.get(), 0, 0, 0, 0, a, aSlice, &box);
    g.ctx->CopySubresourceRegion(sb.get(), 0, 0, 0, 0, b, 0, &box);
    D3D11_MAPPED_SUBRESOURCE ma{}, mb{};
    if (FAILED(g.ctx->Map(sa.get(), 0, D3D11_MAP_READ, 0, &ma))) return 0.0;
    if (FAILED(g.ctx->Map(sb.get(), 0, D3D11_MAP_READ, 0, &mb))) {
        g.ctx->Unmap(sa.get(), 0);
        return 0.0;
    }
    double sse = 0.0;
    for (UINT y = 0; y < h; ++y) {
        const auto* ra = static_cast<const uint8_t*>(ma.pData) + static_cast<size_t>(y) * ma.RowPitch;
        const auto* rb = static_cast<const uint8_t*>(mb.pData) + static_cast<size_t>(y) * mb.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            const double d = static_cast<double>(ra[x]) - rb[x];
            sse += d * d;
        }
    }
    g.ctx->Unmap(sa.get(), 0);
    g.ctx->Unmap(sb.get(), 0);
    const double mse = sse / (static_cast<double>(w) * h);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// `--presets`: what the encoder's quality-vs-speed setting does here. A
// desktop-sized, text-like page scrolls for 240 frames at 240 fps; each
// setting reports its time per frame (how busy the video engine is kept) and
// the text quality it delivers at the same bitrate.
static int PresetBench() {
    Gfx::Get().Init();
    auto& g = Gfx::Get();
    constexpr UINT W = 4096, H = 1152, kFps = 240, kBitrate = 20'000'000;
    constexpr int kFrames = 240;

    std::vector<uint32_t> page(static_cast<size_t>(W) * H);
    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            const bool glyphRow = (y % 18) < 12 && (x % 9) < 7;
            uint32_t hsh = (x / 2) * 73856093u ^ (y / 2) * 19349663u;
            hsh ^= hsh >> 13;
            hsh *= 0x5bd1e995u;
            page[static_cast<size_t>(y) * W + x] = (glyphRow && (hsh & 0x100u)) ? 0xFF202428u : 0xFFF2F2F0u;
        }
    }
    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = W; bd.Height = H; bd.MipLevels = 1; bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc = { 1, 0 };
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const D3D11_SUBRESOURCE_DATA init{ page.data(), W * 4, 0 };
    D3D11_TEXTURE2D_DESC nd = bd;
    nd.Format = DXGI_FORMAT_NV12;
    winrt::com_ptr<ID3D11Texture2D> background, work, nv12;
    if (FAILED(g.d3d->CreateTexture2D(&bd, &init, background.put())) ||
        FAILED(g.d3d->CreateTexture2D(&bd, nullptr, work.put())) ||
        FAILED(g.d3d->CreateTexture2D(&nd, nullptr, nv12.put()))) {
        printf("textures unavailable\n");
        return 1;
    }
    VideoConverter conv;
    if (!conv.Init()) return 1;

    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    printf("%ux%u at %u fps, %u Mbps, %d frames of scrolling text\n", W, H, kFps, kBitrate / 1'000'000,
           kFrames);
    printf("  setting   ms/frame  slowest  serial fps  Mbps   text PSNR\n");
    const UINT settings[] = { H264Encoder::kEncoderDefault, 0, 25, 33, 50, 66, 75, 100 };
    for (const UINT q : settings) {
        H264Encoder enc;
        H264Decoder dec;
        if (!enc.Init(W, H, kFps, kBitrate, q) || !dec.Init()) {
            printf("  %7s   encoder unavailable\n", q == H264Encoder::kEncoderDefault ? "default" : std::to_string(q).c_str());
            continue;
        }
        double total = 0, slowest = 0, psnrSum = 0;
        int psnrCount = 0;
        size_t bytes = 0;
        for (int i = 0; i < kFrames; ++i) {
            {
                std::lock_guard<std::mutex> device(g.deviceMutex);
                const UINT s = static_cast<UINT>(i * 6) % W;
                const D3D11_BOX right{ s, 0, 0, W, H, 1 };
                g.ctx->CopySubresourceRegion(work.get(), 0, 0, 0, 0, background.get(), 0, &right);
                if (s > 0) {
                    const D3D11_BOX left{ 0, 0, 0, s, H, 1 };
                    g.ctx->CopySubresourceRegion(work.get(), 0, W - s, 0, 0, background.get(), 0, &left);
                }
                conv.Convert(work.get(), 0, nullptr, nv12.get());
            }
            std::vector<EncodedFrame> out;
            LARGE_INTEGER a{}, b{};
            QueryPerformanceCounter(&a);
            enc.EncodeSync(nv12.get(), out, 500);
            QueryPerformanceCounter(&b);
            const double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
            total += ms;
            slowest = (std::max)(slowest, ms);
            for (const auto& frame : out) {
                bytes += frame.data.size();
                std::vector<DecodedFrame> decoded;
                if (dec.Decode(frame.data.data(), frame.data.size(), decoded) && !decoded.empty() &&
                    i % 30 == 29) {
                    psnrSum += LumaPsnr(decoded.back().texture.get(), decoded.back().subresource,
                                        nv12.get(), W, H);
                    ++psnrCount;
                }
            }
        }
        const double perFrame = total / kFrames;
        printf("  %7s   %8.2f  %7.2f  %10.0f  %5.1f   %6.2f dB\n",
               q == H264Encoder::kEncoderDefault ? "default" : std::to_string(q).c_str(), perFrame,
               slowest, 1000.0 / perFrame, bytes * 8.0 / 1e6 * kFps / kFrames,
               psnrCount ? psnrSum / psnrCount : 0.0);
    }
    return 0;
}

static int LiveProbe() {
    const std::wstring path = ConfigDir() + L"\\stream.ini";
    const int port = static_cast<int>(GetPrivateProfileIntW(L"Stream", L"Port", 5901, path.c_str()));
    const int enabled = static_cast<int>(GetPrivateProfileIntW(L"Stream", L"Enabled", 0, path.c_str()));
    wchar_t hex[4096]{};
    GetPrivateProfileStringW(L"Stream", L"KeyBlob", L"", hex, ARRAYSIZE(hex), path.c_str());
    std::wstring key;
    if (!UnprotectSecret(FromHex(hex), key)) {
        printf("could not read the stream key from %ls\n", path.c_str());
        return 1;
    }
    printf("settings: enabled=%d port=%d key=%zu chars\n", enabled, port, key.size());

    Gfx::Get().Init();
    StreamClient client;
    client.Connect(L"127.0.0.1", static_cast<uint16_t>(port), key, nullptr);

    if (!WaitFor([&] { return client.Connected(); }, 5000)) {
        printf("NOT CONNECTED: %ls\n", client.Status().c_str());
        return 1;
    }
    printf("connected: %ls\n", client.Status().c_str());

    if (!WaitFor([&] { return !client.Mirrors().empty(); }, 3000)) {
        printf("connected but the mirror list is empty\n");
        return 1;
    }
    for (const auto& m : client.Mirrors()) {
        printf("mirror id=%u  %ux%u  %ls\n", m.id, m.width, m.height, m.name.c_str());
    }

    const uint32_t id = client.Mirrors()[0].id;
    client.SetSubscribed(id, true);
    printf("subscribed to %u; watching for 8 seconds\n", id);
    for (int i = 0; i < 16; ++i) {
        Sleep(500);
        auto views = client.Views();
        const uint64_t frames = views.empty() ? 0 : views[0].frames;
        printf("  t=%4dms  frames=%llu  size=%ux%u  rtt=%d  status=%ls\n", (i + 1) * 500,
               static_cast<unsigned long long>(frames),
               views.empty() ? 0u : views[0].width, views.empty() ? 0u : views[0].height,
               client.RttMs(), client.Status().c_str());
        if (frames >= 10) break;
    }
    client.Disconnect();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--presets") == 0) {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        LogOpen(L"test");
        return PresetBench();
    }
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        return Bench();
    }
    if (argc > 1 && strcmp(argv[1], "--live") == 0) {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        return LiveProbe();
    }

    // STA, exactly like both real applications' main threads: worker threads
    // must then set up their own apartments, which a multi-threaded init here
    // would have silently papered over.
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Encoder and server diagnostics land in %APPDATA%\RearViewMirror\test.log,
    // so a run on another machine records exactly what its GPU did.
    LogOpen(L"test");
    printf("GPU encoders, own adapter first:\n");
    for (const auto& name : [] {
             std::vector<std::wstring> names;
             try { Gfx::Get().Init(); } catch (...) {}
             for (const auto& a : HardwareEncoders()) {
                 wchar_t* s = nullptr;
                 UINT32 len = 0;
                 if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len))) {
                     names.emplace_back(s, len);
                     CoTaskMemFree(s);
                 }
             }
             return names;
         }()) {
        printf("  %ls\n", name.c_str());
    }
    TestCrypto();
    TestChannel();
    TestPacketizer();
    try {
        Gfx::Get().Init();
        TestCodec();
        TestEncoderSizes();
        TestRateControl();
        TestEncoderPresets();
        TestConverterSources();
        TestDesktopCapture();
        TestLoopback();
        TestWelcomeBinding();
        TestHostileClients();
        TestFrameRateCap();
        TestHighFrameRate();
    } catch (...) {
        Check(false, "graphics device initialised");
    }
    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
