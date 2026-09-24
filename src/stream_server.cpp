#include "stream_server.h"

namespace rvm {

using namespace net;

namespace {

constexpr size_t   kMaxClients = 8;
constexpr uint64_t kClientTimeoutMs = 10000;
constexpr UINT     kMinStreamDim = 16;

// Handshakes. A client resends HELLO once a second until answered, so these
// leave plenty of room for real clients while bounding what replays can do.
constexpr size_t   kMaxPending = 16;
constexpr uint64_t kPendingTimeoutMs = 5000;
constexpr double   kAdmitPerSec = 5.0;
constexpr double   kAdmitBurst = 10.0;

// Per client. A viewer needs one subscription per mirror it shows.
constexpr size_t kMaxSubscriptions = 64;

// Retransmits per client: well above what loss recovery needs at the
// configured bitrates, far below what an abusive NACK stream would ask for.
constexpr double kResendPerSec = 2000.0;
constexpr double kResendBurst  = 1000.0;

// A forced IDR and a repush of the mirror's last frame, at most this often
// per stream, however many clients ask. Requests inside the gap are deferred
// to its end, not dropped, so a newcomer always gets its keyframe.
constexpr uint64_t kKeyframeGapMs = 250;

// Hardware encoders refuse small frames: NVENC accepts 256x128 but not
// 128x128. A small crop is scaled up uniformly until both dimensions clear
// this floor on the way into the encoder; the client just shows the larger
// picture.
constexpr UINT kMinEncodeDim = 256;

uint64_t NowMs() {
    return GetTickCount64();
}

// Frame pacing needs better than the tick count's 15.6 ms granularity.
int64_t NowUs() {
    static const int64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart / frequency * 1'000'000 + (c.QuadPart % frequency) * 1'000'000 / frequency;
}

uint32_t RandomSession() {
    uint32_t id = 0;
    do { RandomBytes(&id, sizeof(id)); } while (id == 0);
    return id;
}

uint64_t RandomCounter() {
    uint64_t c = 0;
    RandomBytes(&c, sizeof(c));
    return c >> 1;   // Headroom so it cannot wrap during a session.
}

// Cuts a UTF-8 string to at most `max` bytes without splitting a character.
std::string TruncateUtf8(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

}  // namespace

struct StreamServer::Client {
    Endpoint endpoint;
    std::mutex sendMutex;   // Seal + send are one unit: the counter must not interleave.
    SecureChannel channel;

    // Net thread only.
    std::set<uint32_t> subscriptions;
    uint64_t lastSeenMs = 0;
    double   resendTokens = kResendBurst;
    uint64_t resendRefillMs = 0;
};

// One encoder per mirror, shared by every client subscribed to it. Frames pass
// through a few texture slots between the capture thread that converts them
// and the encode thread that feeds them in; if the encoder falls behind, the
// newest frame wins and the older one is simply never encoded.
struct StreamServer::Stream {
    explicit Stream(uint32_t id) : mirrorId(id), sender(id) {}

    const uint32_t mirrorId;

    // Frame slots. The encoder reads a texture some time after taking it, so
    // a slot stays off limits to capture until its encoded frame comes out.
    static constexpr int kSlots = 4;
    static constexpr size_t kMaxInflight = 2;   // Beyond this, assume the oldest was consumed.

    std::mutex swap;
    winrt::com_ptr<ID3D11Texture2D> textures[kSlots];
    UINT texW = 0, texH = 0;
    int  ready = -1;           // Latest complete frame, or -1.
    int  busy  = -1;           // Being handed to the encoder, or -1.
    int  held  = -1;           // Last frame encoded, kept intact for Repeat(), or -1.
    std::deque<int> inflight;  // Inside the encoder, oldest first.
    bool reinit = false;

    bool SlotFree(int i) const {
        return i != ready && i != busy && i != held &&
               std::find(inflight.begin(), inflight.end(), i) == inflight.end();
    }

    // Set once an encoder refused the exact size: frames are then scaled to
    // multiples of 16, which every encoder accepts.
    std::atomic<bool> align16{ false };
    uint64_t nextInitMs = 0;     // Encode thread: a failed Init waits before retrying.

    // The frame-rate cap, under `swap`. A frame the cap skipped is owed: if
    // the source then goes still, the encode thread asks for it again.
    int64_t nextAcceptUs = 0;
    bool    skipped = false;

    // Incremented only under streamsMutex_, so a stream seen there with no
    // subscribers cannot gain one while it is being pruned.
    std::atomic<int> subscribers{ 0 };
    std::atomic<bool> wantKeyframe{ false };
    VideoConverter converter;   // Capture thread, under the device lock.
    bool converterReady = false;

    H264Encoder encoder;        // Encode thread only.
    uint32_t seq = 0;

    std::mutex senderMutex;     // Packetize (encode thread) vs NACK (net thread).
    FrameSender sender;

    std::mutex subsMutex;
    std::vector<std::weak_ptr<Client>> subs;

    // Net thread only: the keyframe gate.
    uint64_t nextKeyframeMs = 0;
    bool     keyframeDeferred = false;
};

StreamServer::~StreamServer() {
    Stop();
}

bool StreamServer::Start(const StreamSettings& settings) {
    Stop();
    if (!settings.enabled || settings.key.empty()) return false;
    if (!socket_.Open(settings.port)) return false;

    settings_ = settings;
    fps_ = static_cast<UINT>(ClampI(static_cast<int>(settings.fps), static_cast<int>(kMinStreamFps),
                                    static_cast<int>(kMaxStreamFps)));
    masterKey_ = DeriveMasterKey(settings.key);
    if (!master_.SetKey(masterKey_)) {
        socket_.Close();
        SecureZeroMemory(masterKey_.data(), masterKey_.size());
        return false;
    }
    admitTokens_ = kAdmitBurst;
    admitRefillMs_ = NowMs();
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    running_ = true;
    netThread_ = std::thread([this] { NetLoop(); });
    encodeThread_ = std::thread([this] { EncodeLoop(); });
    Log(L"server: started on udp %u, %u kbps, %u fps, encoder '%s'", settings.port,
        settings.bitrateKbps, settings.fps, HardwareEncoderName().c_str());
    return true;
}

void StreamServer::Stop() {
    if (!running_.exchange(false)) {
        socket_.Close();
        return;
    }
    if (frameEvent_) SetEvent(frameEvent_);
    if (netThread_.joinable()) netThread_.join();
    if (encodeThread_.joinable()) encodeThread_.join();

    {
        std::lock_guard lock(clientsMutex_);
        for (auto& c : clients_) {
            Writer w;
            w.U8(static_cast<uint8_t>(Msg::Bye));
            SendTo(*c, w.Data());
        }
        clients_.clear();
    }
    pending_.clear();
    {
        std::lock_guard lock(streamsMutex_);
        streams_.clear();
    }
    socket_.Close();
    if (frameEvent_) { CloseHandle(frameEvent_); frameEvent_ = nullptr; }
    SecureZeroMemory(masterKey_.data(), masterKey_.size());
}

size_t StreamServer::ClientCount() const {
    std::lock_guard lock(clientsMutex_);
    return clients_.size();
}

size_t StreamServer::StreamCount() const {
    std::lock_guard lock(streamsMutex_);
    return streams_.size();
}

void StreamServer::SetFrameRequester(FrameRequester requester) {
    std::lock_guard lock(requesterMutex_);
    requester_ = std::move(requester);
}

void StreamServer::RequestFrame(uint32_t mirrorId) {
    FrameRequester requester;
    {
        std::lock_guard lock(requesterMutex_);
        requester = requester_;
    }
    RVM_LOG_SAMPLED(100, L"server: requesting a frame for mirror %u (requester %s)", mirrorId,
                    requester ? L"set" : L"NOT SET");
    if (requester) requester(mirrorId);
}

// Net thread. The one place a keyframe and a frame request are issued, so the
// rate is per stream no matter how many clients ask or how often.
void StreamServer::ForceKeyframe(Stream& s, uint64_t nowMs) {
    if (nowMs < s.nextKeyframeMs) {
        s.keyframeDeferred = true;
        return;
    }
    s.keyframeDeferred = false;
    s.nextKeyframeMs = nowMs + kKeyframeGapMs;
    s.wantKeyframe = true;
    RequestFrame(s.mirrorId);   // The source may be still; a keyframe needs a frame.
}

void StreamServer::ServiceDeferredKeyframes(uint64_t nowMs) {
    std::vector<std::shared_ptr<Stream>> due;
    {
        std::lock_guard lock(streamsMutex_);
        for (auto& [id, s] : streams_) {
            if (s->keyframeDeferred && nowMs >= s->nextKeyframeMs) due.push_back(s);
        }
    }
    for (auto& s : due) ForceKeyframe(*s, nowMs);
}

void StreamServer::SetMirrorList(std::vector<MirrorInfo> list) {
    Log(L"server: mirror list has %zu entries", list.size());
    for (const auto& m : list) Log(L"server:   id=%u %ux%u '%s'", m.id, m.width, m.height, m.name.c_str());
    {
        std::lock_guard lock(listMutex_);
        mirrorList_ = std::move(list);
    }
    if (!running_) return;

    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lock(clientsMutex_);
        clients = clients_;
    }
    Writer w;
    w.U8(static_cast<uint8_t>(Msg::MirrorsChanged));
    for (auto& c : clients) SendTo(*c, w.Data());
}

bool StreamServer::MirrorListed(uint32_t mirrorId) {
    std::lock_guard lock(listMutex_);
    return std::any_of(mirrorList_.begin(), mirrorList_.end(),
                       [&](const MirrorInfo& m) { return m.id == mirrorId; });
}

std::shared_ptr<StreamServer::Client> StreamServer::FindClient(const Endpoint& endpoint) {
    std::lock_guard lock(clientsMutex_);
    for (auto& c : clients_) {
        if (c->endpoint == endpoint) return c;
    }
    return nullptr;
}

std::shared_ptr<StreamServer::Stream> StreamServer::FindStream(uint32_t mirrorId) {
    std::lock_guard lock(streamsMutex_);
    auto it = streams_.find(mirrorId);
    return it == streams_.end() ? nullptr : it->second;
}

std::shared_ptr<StreamServer::Stream> StreamServer::AcquireStream(uint32_t mirrorId) {
    std::lock_guard lock(streamsMutex_);
    auto& slot = streams_[mirrorId];
    if (!slot) slot = std::make_shared<Stream>(mirrorId);
    slot->subscribers.fetch_add(1);
    return slot;
}

void StreamServer::SendTo(Client& client, const std::vector<uint8_t>& plain) {
    std::lock_guard lock(client.sendMutex);
    std::vector<uint8_t> datagram;
    if (client.channel.Seal(plain.data(), plain.size(), datagram)) {
        socket_.SendTo(client.endpoint, datagram.data(), datagram.size());
    }
}

// ---------------------------------------------------------------------------
// Capture side

void StreamServer::SubmitFrame(uint32_t mirrorId, ID3D11Texture2D* cache, const RECT& crop) {
    if (!running_) return;
    auto s = FindStream(mirrorId);
    RVM_LOG_SAMPLED(300, L"server: tee frame for mirror %u crop %dx%d, stream=%s subs=%d",
                    mirrorId, RectW(crop), RectH(crop), s ? L"yes" : L"NO",
                    s ? s->subscribers.load() : 0);
    if (!s || s->subscribers.load(std::memory_order_relaxed) == 0) return;

    const UINT cropW = static_cast<UINT>(RectW(crop));
    const UINT cropH = static_cast<UINT>(RectH(crop));
    if (cropW < kMinStreamDim || cropH < kMinStreamDim) return;

    // Encode size: the crop, upscaled uniformly if it is under the encoder's
    // floor, and even in both dimensions for NV12.
    const float scale = (std::max)(1.0f, (std::max)(static_cast<float>(kMinEncodeDim) / cropW,
                                                    static_cast<float>(kMinEncodeDim) / cropH));
    UINT w = static_cast<UINT>(cropW * scale) & ~1u;
    UINT h = static_cast<UINT>(cropH * scale) & ~1u;
    if (s->align16.load()) {
        w = (w + 15) & ~15u;
        h = (h + 15) & ~15u;
    }

    // Hold each stream to the configured rate, on an even cadence. A frame
    // someone is waiting for (a keyframe, a new viewer) always goes through.
    {
        const int64_t interval = 1'000'000 / static_cast<int64_t>((std::max)(fps_.load(), 1u));
        const int64_t now = NowUs();
        std::lock_guard lock(s->swap);
        if (!s->wantKeyframe.load() && now + interval / 4 < s->nextAcceptUs) {
            s->skipped = true;
            return;
        }
        s->nextAcceptUs = (std::max)(s->nextAcceptUs, now - interval) + interval;
        s->skipped = false;
    }

    if (!s->converterReady) s->converterReady = s->converter.Init();
    if (!s->converterReady) {
        Log(L"server: video processor unavailable for mirror %u", mirrorId);
        return;
    }

    int write = -1;
    {
        std::lock_guard lock(s->swap);
        if (w != s->texW || h != s->texH) {
            if (s->busy >= 0) return;   // Encoder mid-frame; resize on the next one.
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_NV12; d.SampleDesc = { 1, 0 };
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            for (auto& t : s->textures) {
                t = nullptr;
                if (FAILED(Gfx::Get().d3d->CreateTexture2D(&d, nullptr, t.put()))) {
                    Log(L"server: NV12 texture %ux%u failed for mirror %u", w, h, mirrorId);
                    return;
                }
            }
            Log(L"server: stream %u textures %ux%u", mirrorId, w, h);
            s->texW = w;
            s->texH = h;
            s->ready = -1;
            s->held = -1;
            s->inflight.clear();
            s->reinit = true;
        }
        for (int i = 0; i < Stream::kSlots; ++i) {
            if (s->SlotFree(i)) { write = i; break; }
        }
    }
    if (write < 0) return;

    // The whole crop, scaled by the video processor into the encode texture.
    const RECT src{ crop.left, crop.top, crop.left + static_cast<LONG>(cropW),
                    crop.top + static_cast<LONG>(cropH) };
    if (!s->converter.Convert(cache, 0, &src, s->textures[write].get())) {
        RVM_LOG_SAMPLED(300, L"server: convert failed for mirror %u", mirrorId);
        return;
    }

    {
        std::lock_guard lock(s->swap);
        s->ready = write;
    }
    SetEvent(frameEvent_);
}

// ---------------------------------------------------------------------------
// Encode side

void StreamServer::EncodeLoop() {
    while (running_) {
        // Short timeout: an encoder that finished a frame after Encode()
        // stopped waiting for it gets drained here, not on the next input.
        WaitForSingleObject(frameEvent_, 20);
        if (!running_) break;

        std::vector<std::shared_ptr<Stream>> streams;
        {
            std::lock_guard lock(streamsMutex_);
            for (auto& [id, s] : streams_) streams.push_back(s);
        }
        for (auto& s : streams) EncodeStream(*s);
        PruneStreams();
    }
}

// A stream nobody watches is freed: its encoder was shut down by
// EncodeStream on this thread, and its textures and frame history go with it.
// A later subscribe starts a fresh one.
void StreamServer::PruneStreams() {
    std::vector<std::shared_ptr<Stream>> doomed;   // Released outside the lock.
    std::lock_guard lock(streamsMutex_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        Stream& s = *it->second;
        if (s.subscribers.load() == 0 && !s.encoder.Ready()) {
            doomed.push_back(std::move(it->second));
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
}

// Runs whenever a frame is ready or an encoder has news, never waiting on
// either: new frames go in as soon as the encoder asks for one, finished
// frames go out as soon as it announces them.
void StreamServer::EncodeStream(Stream& s) {
    if (s.subscribers.load() == 0) {
        if (s.encoder.Ready()) s.encoder.Shutdown();   // Free the hardware session.
        std::lock_guard lock(s.swap);
        s.inflight.clear();
        s.held = -1;
        return;
    }

    // Encoded frames come out in the order their inputs went in; each one
    // frees the oldest slot the encoder was holding.
    const auto retire = [&](size_t count) {
        std::lock_guard lock(s.swap);
        for (size_t i = 0; i < count && !s.inflight.empty(); ++i) s.inflight.pop_front();
    };

    std::vector<EncodedFrame> produced;
    if (s.encoder.Ready() && !s.encoder.Service(produced)) {
        // The encoder failed; start a fresh one on the next frame.
        Log(L"server: encoder for mirror %u failed; recreating", s.mirrorId);
        s.encoder.Shutdown();
        std::lock_guard lock(s.swap);
        s.inflight.clear();
        s.reinit = true;
    }
    retire(produced.size());

    // Take the newest frame only when the encoder can use it now, or has to
    // be (re)created for it; otherwise it waits, and a newer one may replace it.
    int idx = -1;
    bool reinit = false;
    UINT w = 0, h = 0;
    {
        std::lock_guard lock(s.swap);
        const bool mustInit = !s.encoder.Ready() || s.reinit || s.encoder.Width() != s.texW ||
                              s.encoder.Height() != s.texH;
        if (s.ready >= 0 && (mustInit || s.encoder.WantsInput())) {
            idx = s.ready;
            s.ready = -1;
            s.busy = idx;
            reinit = s.reinit;
            s.reinit = false;
            w = s.texW;
            h = s.texH;
        }
    }

    if (idx < 0) {
        // The cap skipped the source's latest picture and nothing has come
        // since: fetch it again now that the cap allows, or the viewer would
        // be left on an older one.
        bool owed = false;
        {
            const int64_t interval = 1'000'000 / static_cast<int64_t>((std::max)(fps_.load(), 1u));
            std::lock_guard lock(s.swap);
            if (s.skipped && s.ready < 0 && NowUs() >= s.nextAcceptUs + interval / 2) {
                s.skipped = false;
                owed = true;
            }
        }
        if (owed) RequestFrame(s.mirrorId);

        // Nothing came out, and the encoder is sitting on the last frame: the
        // source is still, so no next frame will come to push it out.
        if (produced.empty() && s.encoder.Ready() && s.encoder.WantsInput() &&
            s.encoder.NeedsNudge(GetTickCount64())) {
            int again = -1;
            {
                std::lock_guard lock(s.swap);
                if (s.held >= 0) {
                    again = s.held;
                    s.busy = again;
                }
            }
            if (again >= 0) {
                const size_t before = produced.size();
                const bool fed = s.encoder.Repeat(s.textures[again].get(), produced);
                {
                    std::lock_guard lock(s.swap);
                    s.busy = -1;
                    if (fed) s.inflight.push_back(again);
                    while (s.inflight.size() > Stream::kMaxInflight) s.inflight.pop_front();
                }
                retire(produced.size() - before);
            }
        }
        if (!produced.empty()) SendFrames(s, produced);
        return;
    }

    const auto giveBack = [&] {
        std::lock_guard lock(s.swap);
        s.busy = -1;
        if (s.ready < 0) s.ready = idx;   // Unless a newer frame has arrived meanwhile.
    };

    const uint64_t now = GetTickCount64();
    if (reinit || !s.encoder.Ready() || s.encoder.Width() != w || s.encoder.Height() != h) {
        if (s.align16.load() && ((w | h) & 15u)) {
            // A frame converted before the switch to aligned sizes; the next
            // one will be aligned, so do not spend an Init on this one.
            std::lock_guard lock(s.swap);
            s.busy = -1;
            return;
        }
        bool ok = false;
        if (now >= s.nextInitMs) {
            s.encoder.SetWakeEvent(frameEvent_);
            ok = s.encoder.Init(w, h, settings_.fps, settings_.bitrateKbps * 1000);
            Log(L"server: encoder init %ux%u for mirror %u -> %s", w, h, s.mirrorId,
                ok ? L"ok" : L"FAILED");
            {
                std::lock_guard lock(s.swap);
                s.inflight.clear();
                s.held = -1;
            }
            if (!ok) {
                s.nextInitMs = now + 2000;
                if (!s.align16.exchange(true)) {
                    // Next attempt at an aligned size, with a fresh frame to try it on.
                    Log(L"server: retrying mirror %u at 16-aligned dimensions", s.mirrorId);
                    s.nextInitMs = now;
                    RequestFrame(s.mirrorId);
                }
            } else {
                s.wantKeyframe = true;
            }
        }
        if (!ok) {
            std::lock_guard lock(s.swap);
            s.busy = -1;
            return;
        }
        if (!s.encoder.WantsInput()) {
            // Not asking for input yet; its request will wake this thread.
            giveBack();
            return;
        }
    }
    if (s.wantKeyframe.exchange(false)) s.encoder.RequestKeyframe();

    const size_t before = produced.size();
    const bool fed = s.encoder.Encode(s.textures[idx].get(), produced);
    if (fed) {
        std::lock_guard lock(s.swap);
        s.busy = -1;
        s.held = idx;
        s.inflight.push_back(idx);
        while (s.inflight.size() > Stream::kMaxInflight) s.inflight.pop_front();
    } else {
        giveBack();
    }
    retire(produced.size() - before);
    RVM_LOG_SAMPLED(300, L"server: encode mirror %u -> fed=%d frames=%zu%s", s.mirrorId, fed ? 1 : 0,
                    produced.size(), (!produced.empty() && produced[0].keyframe) ? L" (keyframe)" : L"");
    if (!produced.empty()) SendFrames(s, produced);
}

void StreamServer::SendFrames(Stream& s, const std::vector<EncodedFrame>& frames) {
    std::vector<std::shared_ptr<Client>> targets;
    {
        std::lock_guard lock(s.subsMutex);
        for (auto& weak : s.subs) {
            if (auto c = weak.lock()) targets.push_back(c);
        }
    }

    for (const auto& f : frames) {
        const uint32_t seq = ++s.seq;
        std::lock_guard sender(s.senderMutex);
        const auto& packets = s.sender.Packetize(seq, f.keyframe, f.data.data(), f.data.size());
        RVM_LOG_SAMPLED(300, L"server: send mirror %u seq %u: %zu bytes in %zu packets to %zu clients%s",
                        s.mirrorId, seq, f.data.size(), packets.size(), targets.size(),
                        f.keyframe ? L" (keyframe)" : L"");
        for (auto& c : targets) {
            std::lock_guard send(c->sendMutex);
            std::vector<uint8_t> datagram;
            for (const auto& p : packets) {
                if (c->channel.Seal(p.data(), p.size(), datagram)) {
                    socket_.SendTo(c->endpoint, datagram.data(), datagram.size());
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Network side

void StreamServer::NetLoop() {
    std::vector<uint8_t> buffer(kMaxDatagram + 64);
    uint64_t lastSweep = NowMs();
    uint64_t lastKeyframeService = lastSweep;

    while (running_) {
        Endpoint from;
        const int n = socket_.Receive(buffer.data(), buffer.size(), from, 5);
        const uint64_t now = NowMs();
        if (n > 0) HandleDatagram(from, buffer.data(), static_cast<size_t>(n), now);
        if (now - lastKeyframeService >= 25) {
            ServiceDeferredKeyframes(now);
            lastKeyframeService = now;
        }
        if (now - lastSweep > 1000) {
            ExpireClients(now);
            DropUnlistedSubscriptions();
            lastSweep = now;
        }
    }
}

void StreamServer::HandleDatagram(const Endpoint& from, const uint8_t* data, size_t len,
                                  uint64_t nowMs) {
    uint32_t session = 0;
    if (!SecureChannel::PeekSession(data, len, session)) return;

    std::vector<uint8_t> plain;
    uint32_t sender = 0;

    if (auto client = FindClient(from)) {
        if (client->channel.Open(data, len, plain, sender) && !plain.empty()) {
            client->lastSeenMs = nowMs;
            Reader r(plain.data(), plain.size());
            HandleMessage(client, r, nowMs);
            return;
        }
    }

    // The first datagram under a fresh session key completes that handshake.
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (!(it->client->endpoint == from)) continue;
        if (!it->client->channel.Open(data, len, plain, sender) || plain.empty()) break;
        auto client = it->client;
        pending_.erase(it);
        if (!Promote(client)) return;
        client->lastSeenMs = nowMs;
        Reader r(plain.data(), plain.size());
        HandleMessage(client, r, nowMs);
        return;
    }

    // Otherwise it can only be a HELLO: a new client, or one restarting.
    Handshake(from, data, len, nowMs);
}

bool StreamServer::AdmissionAllowed(uint64_t nowMs) {
    admitTokens_ = (std::min)(kAdmitBurst,
                              admitTokens_ + (nowMs - admitRefillMs_) * kAdmitPerSec / 1000.0);
    admitRefillMs_ = nowMs;
    if (admitTokens_ < 1.0) return false;
    admitTokens_ -= 1.0;
    return true;
}

void StreamServer::Handshake(const Endpoint& from, const uint8_t* data, size_t len,
                             uint64_t nowMs) {
    // A HELLO is sealed under the master key with a fresh client session id.
    // The master channel is keyed once at start, so a stranger's junk costs
    // one failed authentication, nothing more.
    std::vector<uint8_t> plain;
    uint32_t clientSession = 0;
    if (!master_.Open(data, len, plain, clientSession) || plain.empty()) return;

    Reader r(plain.data(), plain.size());
    uint8_t type = 0;
    uint16_t version = 0;
    uint8_t clientRandom[kRandomBytes];
    if (!r.U8(type) || type != static_cast<uint8_t>(Msg::Hello) || !r.U16(version) ||
        version != kVersion || !r.Bytes(clientRandom, kRandomBytes)) {
        return;
    }
    if (!AdmissionAllowed(nowMs)) return;

    // The client resends HELLO until answered. If our WELCOME was merely slow,
    // answer the repeat with the same one: a new server random would give the
    // two sides different session keys.
    for (const auto& p : pending_) {
        if (p.client->endpoint == from && p.clientSession == clientSession) {
            socket_.SendTo(from, p.welcome.data(), p.welcome.size());
            return;
        }
    }

    // Full: no WELCOME at all, so the client keeps retrying and says so,
    // rather than believing it is connected. A client already here from this
    // address may replace itself.
    if (ClientCount() >= kMaxClients && !FindClient(from)) return;

    auto client = std::make_shared<Client>();
    client->endpoint = from;
    client->lastSeenMs = nowMs;
    client->resendRefillMs = nowMs;

    uint8_t serverRandom[kRandomBytes];
    RandomBytes(serverRandom, kRandomBytes);
    const uint32_t serverSession = RandomSession();

    // WELCOME goes back under the master key; everything after it is under
    // the session key, on fresh counters and windows.
    master_.BeginSend(serverSession, RandomCounter());
    Writer w;
    w.U8(static_cast<uint8_t>(Msg::Welcome));
    w.Bytes(serverRandom, kRandomBytes);
    std::vector<uint8_t> datagram;
    if (!master_.Seal(w.Data().data(), w.Data().size(), datagram)) return;

    if (!client->channel.SetKey(DeriveSessionKey(masterKey_, clientRandom, serverRandom))) return;
    client->channel.BeginSend(serverSession);
    client->channel.BeginRecv(clientSession);

    // One pending handshake per address; the oldest makes way when full.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return p.client->endpoint == from; }),
                   pending_.end());
    if (pending_.size() >= kMaxPending) pending_.erase(pending_.begin());
    pending_.push_back({ client, clientSession, datagram, nowMs });

    socket_.SendTo(from, datagram.data(), datagram.size());
    RVM_LOG_SAMPLED(50, L"server: handshake from %s", from.ToString().c_str());
}

// The peer answered under the session key, so it holds the passphrase. Only
// now does it take a slot, or replace an earlier session from its address.
bool StreamServer::Promote(const std::shared_ptr<Client>& client) {
    if (auto old = FindClient(client->endpoint)) RemoveClient(old);

    std::lock_guard lock(clientsMutex_);
    if (clients_.size() >= kMaxClients) return false;
    clients_.push_back(client);
    Log(L"server: client %s admitted", client->endpoint.ToString().c_str());
    return true;
}

// Every name is cut, evenly and on character boundaries, until the whole
// list fits one datagram; a list too long for even bare entries is cut short.
void StreamServer::SendListTo(Client& client) {
    constexpr size_t kEntryFixed = 4 + 2 + 2 + 1;   // id, w, h, name length.
    constexpr size_t kListFixed = 1 + 1;            // Msg, count.

    std::vector<MirrorInfo> list;
    {
        std::lock_guard lock(listMutex_);
        list = mirrorList_;
    }
    const size_t count = (std::min)({ list.size(), static_cast<size_t>(255),
                                      (kMaxPlain - kListFixed) / kEntryFixed });
    const size_t budget = kMaxPlain - kListFixed - count * kEntryFixed;

    std::vector<std::string> names;
    names.reserve(count);
    for (size_t i = 0; i < count; ++i) names.push_back(TruncateUtf8(ToUtf8(list[i].name), 255));

    size_t cap = 255;
    const auto total = [&](size_t c) {
        size_t sum = 0;
        for (const auto& n : names) sum += (std::min)(n.size(), c);
        return sum;
    };
    while (cap > 0 && total(cap) > budget) --cap;

    Writer w;
    w.U8(static_cast<uint8_t>(Msg::ListResp));
    w.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
        w.U32(list[i].id);
        w.U16(static_cast<uint16_t>((std::min)(list[i].width, 65535u)));
        w.U16(static_cast<uint16_t>((std::min)(list[i].height, 65535u)));
        w.Str(TruncateUtf8(names[i], cap));
    }
    SendTo(client, w.Data());
}

void StreamServer::HandleMessage(const std::shared_ptr<Client>& client, Reader& r, uint64_t nowMs) {
    uint8_t type = 0;
    if (!r.U8(type)) return;

    switch (static_cast<Msg>(type)) {
    case Msg::ListReq:
        SendListTo(*client);
        break;

    case Msg::Subscribe: {
        uint32_t id = 0;
        if (!r.U32(id) || client->subscriptions.count(id)) break;
        if (client->subscriptions.size() >= kMaxSubscriptions || !MirrorListed(id)) break;
        auto s = AcquireStream(id);
        client->subscriptions.insert(id);
        {
            std::lock_guard lock(s->subsMutex);
            s->subs.push_back(client);
        }
        RVM_LOG_SAMPLED(50, L"server: %s subscribes to mirror %u (subscribers now %d)",
                        client->endpoint.ToString().c_str(), id, s->subscribers.load());
        ForceKeyframe(*s, nowMs);   // A newcomer can only start on an IDR.
        break;
    }

    case Msg::Unsubscribe: {
        uint32_t id = 0;
        if (r.U32(id)) DropSubscription(client, id);
        break;
    }

    case Msg::Nack:
        HandleNack(*client, r, nowMs);
        break;

    case Msg::KeyframeReq: {
        uint32_t id = 0;
        if (!r.U32(id) || !client->subscriptions.count(id)) break;
        RVM_LOG_SAMPLED(50, L"server: keyframe requested for mirror %u", id);
        if (auto s = FindStream(id)) ForceKeyframe(*s, nowMs);
        break;
    }

    case Msg::Ping: {
        uint64_t t = 0;
        if (!r.U64(t)) break;
        Writer w;
        w.U8(static_cast<uint8_t>(Msg::Pong));
        w.U64(t);
        SendTo(*client, w.Data());
        break;
    }

    case Msg::Bye:
        RemoveClient(client);
        break;

    default:
        break;
    }
}

// Resends only for a stream the client watches, each packet at most once per
// NACK, within the client's retransmit budget. The packets are copied out so
// the encoder is never held up while they are sealed and sent.
void StreamServer::HandleNack(Client& client, Reader& r, uint64_t nowMs) {
    uint32_t id = 0, seq = 0;
    uint16_t count = 0;
    if (!r.U32(id) || !r.U32(seq) || !r.U16(count)) return;
    if (!client.subscriptions.count(id)) return;
    auto s = FindStream(id);
    if (!s) return;

    client.resendTokens = (std::min)(kResendBurst, client.resendTokens +
                                     (nowMs - client.resendRefillMs) * kResendPerSec / 1000.0);
    client.resendRefillMs = nowMs;

    std::vector<std::vector<uint8_t>> resend;
    {
        std::lock_guard sender(s->senderMutex);
        const auto* packets = s->sender.Packets(seq);
        if (!packets) return;
        std::vector<bool> seen(packets->size(), false);
        for (uint16_t i = 0; i < count && client.resendTokens >= 1.0; ++i) {
            uint16_t idx = 0;
            if (!r.U16(idx)) break;
            if (idx >= packets->size() || seen[idx]) continue;
            seen[idx] = true;
            resend.push_back((*packets)[idx]);
            client.resendTokens -= 1.0;
        }
    }
    for (const auto& p : resend) SendTo(client, p);
}

void StreamServer::DropSubscription(const std::shared_ptr<Client>& client, uint32_t mirrorId) {
    if (!client->subscriptions.erase(mirrorId)) return;
    if (auto s = FindStream(mirrorId)) {
        {
            std::lock_guard lock(s->subsMutex);
            for (auto it = s->subs.begin(); it != s->subs.end();) {
                auto c = it->lock();
                it = (!c || c == client) ? s->subs.erase(it) : it + 1;
            }
        }
        s->subscribers.fetch_sub(1);
    }
}

// A mirror that left the list (closed, switched off, its source gone) stops
// being streamed even to clients that never unsubscribed, so its stream is
// freed.
void StreamServer::DropUnlistedSubscriptions() {
    std::set<uint32_t> listed;
    {
        std::lock_guard lock(listMutex_);
        for (const auto& m : mirrorList_) listed.insert(m.id);
    }
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lock(clientsMutex_);
        clients = clients_;
    }
    for (auto& c : clients) {
        std::vector<uint32_t> gone;
        for (uint32_t id : c->subscriptions) {
            if (!listed.count(id)) gone.push_back(id);
        }
        for (uint32_t id : gone) DropSubscription(c, id);
    }
}

void StreamServer::RemoveClient(const std::shared_ptr<Client>& client) {
    const std::vector<uint32_t> ids(client->subscriptions.begin(), client->subscriptions.end());
    for (uint32_t id : ids) DropSubscription(client, id);

    std::lock_guard lock(clientsMutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
}

void StreamServer::ExpireClients(uint64_t nowMs) {
    std::vector<std::shared_ptr<Client>> stale;
    {
        std::lock_guard lock(clientsMutex_);
        for (auto& c : clients_) {
            if (nowMs - c->lastSeenMs > kClientTimeoutMs) stale.push_back(c);
        }
    }
    for (auto& c : stale) RemoveClient(c);

    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) {
                       return nowMs - p.createdMs > kPendingTimeoutMs;
                   }),
                   pending_.end());
}

}  // namespace rvm
