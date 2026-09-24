#include "stream_client.h"

namespace rvm {

using namespace net;

namespace {

constexpr uint64_t kHelloIntervalMs   = 1000;
constexpr int      kHelloAttempts     = 8;
constexpr uint64_t kHelloRetryMs      = 3000;   // After the quick attempts, indefinitely.
constexpr uint64_t kPingIntervalMs    = 2000;
constexpr uint64_t kServerTimeoutMs   = 10000;
constexpr uint64_t kKeyframeThrottleMs = 200;
constexpr size_t   kMaxQueuedPerStream = 6;
constexpr uint64_t kListRetryMs       = 1000;    // Until the first list arrives.
constexpr uint64_t kListRefreshMs     = 15000;   // In case a change notice was lost.
constexpr uint64_t kUnwantedMs        = 1000;    // Unsubscribe again at most this often.
constexpr size_t   kMaxNackIndices    = 500;     // Per message, to fit one datagram.

uint64_t NowMs() {
    return GetTickCount64();
}

}  // namespace

struct StreamClient::Stream {
    explicit Stream(uint32_t streamId) : id(streamId) {}

    const uint32_t id;
    FrameAssembler assembler;   // Net thread.
    H264Decoder    decoder;     // Decode thread.
    VideoConverter converter;   // Decode thread, under the device lock.
    bool converterReady = false;
    // The session this stream was last subscribed in; 0 is never. A session
    // number rather than a flag, so a subscribe racing a session drop can
    // never leave a stale "already subscribed" behind.
    std::atomic<uint64_t> subscribedIn{ 0 };

    // Guarded by StreamClient::stateMutex_.
    winrt::com_ptr<ID3D11Texture2D> texture;
    UINT width = 0, height = 0;
    uint64_t frames = 0;
    size_t queued = 0;
};

StreamClient::~StreamClient() {
    Disconnect();
}

void StreamClient::Connect(const std::wstring& host, uint16_t port, const std::wstring& key,
                           HWND notify, uint32_t tag) {
    Disconnect();
    host_ = host;
    port_ = port;
    key_ = key;
    notify_ = notify;
    tag_ = tag;

    if (key.empty()) {
        // Its saved key could not be decrypted on this Windows account.
        SetStatus(L"Key unreadable here; remove and add again.");
        return;
    }
    if (!socket_.Open(0)) {
        SetStatus(L"Could not open a UDP socket.");
        return;
    }
    if (!UdpSocket::Resolve(host, port, server_)) {
        SetStatus(L"Could not resolve " + host + L".");
        socket_.Close();
        return;
    }

    running_ = true;
    SetStatus(L"Connecting to " + host + L":" + std::to_wstring(port) + L"…");
    netThread_ = std::thread([this] { NetLoop(); });
    decodeThread_ = std::thread([this] { DecodeLoop(); });
}

void StreamClient::Disconnect() {
    if (running_.exchange(false)) {
        if (connected_) {
            Writer w;
            w.U8(static_cast<uint8_t>(Msg::Bye));
            Send(w.Data());
        }
        queueCv_.notify_all();
        if (netThread_.joinable()) netThread_.join();
        if (decodeThread_.joinable()) decodeThread_.join();
    }
    connected_ = false;
    socket_.Close();
    {
        std::lock_guard lock(queueMutex_);
        queue_.clear();
    }
    {
        std::lock_guard lock(stateMutex_);
        streams_.clear();
        mirrors_.clear();
        wanted_.clear();
    }
    rttMs_ = -1;
    SecureZeroMemory(masterKey_.data(), masterKey_.size());
}

// Fresh handshake randoms, and a new session number: every stream's
// "subscribed in" is then out of date, so the next list re-subscribes it.
void StreamClient::BeginSession() {
    RandomBytes(clientRandom_, kRandomBytes);
    do { RandomBytes(&clientSession_, sizeof(clientSession_)); } while (clientSession_ == 0);
    ++session_;
}

// The link is gone but the wish list stays: the next session re-subscribes.
// Net thread. Textures are kept so tiles show the last frame meanwhile.
void StreamClient::DropSession(std::wstring status) {
    Log(L"client: session with %s dropped: %s", server_.ToString().c_str(), status.c_str());
    connected_ = false;
    rttMs_ = -1;

    std::vector<std::shared_ptr<Stream>> streams;
    {
        std::lock_guard lock(stateMutex_);
        for (const auto& [id, s] : streams_) streams.push_back(s);
    }
    {
        std::lock_guard lock(queueMutex_);
        queue_.clear();
        for (auto& s : streams) s->queued = 0;
    }
    for (auto& s : streams) {
        s->assembler.Reset();
    }
    BeginSession();
    SetStatus(std::move(status));
}

std::wstring StreamClient::Status() const {
    std::lock_guard lock(stateMutex_);
    return status_;
}

void StreamClient::SetStatus(std::wstring status) {
    {
        std::lock_guard lock(stateMutex_);
        status_ = std::move(status);
    }
    Notify(ClientEvent::StatusChanged);
}

void StreamClient::Notify(ClientEvent event, LPARAM lp) {
    if (!notify_) return;
    if (!PostMessageW(notify_, WM_RVM_CLIENT_EVENT, MakeClientEvent(event, tag_), lp) &&
        event == ClientEvent::FrameReady) {
        framePosted_ = false;   // Never posted, so never acknowledged: let the next one try.
    }
}

std::vector<RemoteMirror> StreamClient::Mirrors() const {
    std::lock_guard lock(stateMutex_);
    return mirrors_;
}

std::shared_ptr<StreamClient::Stream> StreamClient::FindStream(uint32_t id) const {
    std::lock_guard lock(stateMutex_);
    auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : it->second;
}

bool StreamClient::IsSubscribed(uint32_t id) const {
    std::lock_guard lock(stateMutex_);
    return wanted_.count(id) != 0;
}

void StreamClient::SetSubscribed(uint32_t id, bool on) {
    std::shared_ptr<Stream> stream;
    {
        std::lock_guard lock(stateMutex_);
        if (on) {
            if (!wanted_.insert(id).second) return;
            auto& slot = streams_[id];
            if (!slot) slot = std::make_shared<Stream>(id);
            stream = slot;
        } else {
            if (!wanted_.erase(id)) return;
            streams_.erase(id);
        }
    }
    // Off-line, the wish is kept and acted on when the list next arrives. The
    // session number is read before sending: if the session drops meanwhile,
    // the stream is marked with the old one and the next list re-subscribes.
    const uint64_t session = session_.load();
    if (connected_) {
        Writer w;
        w.U8(static_cast<uint8_t>(on ? Msg::Subscribe : Msg::Unsubscribe));
        w.U32(id);
        Send(w.Data());
        if (stream) stream->subscribedIn = session;
        Log(L"client: %s mirror %u", on ? L"subscribed to" : L"unsubscribed from", id);
    }
    Notify(ClientEvent::FrameReady);
}

std::vector<StreamView> StreamClient::Views() const {
    std::lock_guard lock(stateMutex_);
    std::vector<StreamView> views;
    for (const auto& [id, s] : streams_) {
        views.push_back({ id, s->texture, s->width, s->height, s->frames });
    }
    return views;
}

void StreamClient::Send(const std::vector<uint8_t>& plain) {
    std::lock_guard lock(sendMutex_);
    std::vector<uint8_t> datagram;
    if (channel_.Seal(plain.data(), plain.size(), datagram)) {
        socket_.SendTo(server_, datagram.data(), datagram.size());
    }
}

// HELLO is sealed under the master key with a random starting counter (see
// SecureChannel::BeginSend); the reply switches both sides to session keys.
void StreamClient::SendHello() {
    uint64_t start = 0;
    RandomBytes(&start, sizeof(start));
    master_.BeginSend(clientSession_, start >> 1);

    Writer w;
    w.U8(static_cast<uint8_t>(Msg::Hello));
    w.U16(kVersion);
    w.Bytes(clientRandom_, kRandomBytes);
    std::vector<uint8_t> datagram;
    if (master_.Seal(w.Data().data(), w.Data().size(), datagram)) {
        socket_.SendTo(server_, datagram.data(), datagram.size());
    }
}

void StreamClient::RequestList(uint64_t nowMs) {
    Writer w;
    w.U8(static_cast<uint8_t>(Msg::ListReq));
    Send(w.Data());
    lastListReqMs_ = nowMs;
}

// ---------------------------------------------------------------------------
// Network thread

void StreamClient::NetLoop() {
    // Stretching the passphrase takes a noticeable fraction of a second: here,
    // off the UI thread. One master-key channel serves the whole connection.
    masterKey_ = DeriveMasterKey(key_);
    if (!master_.SetKey(masterKey_)) {
        SetStatus(L"Could not set up encryption.");
        running_ = false;
        return;
    }
    BeginSession();

    std::vector<uint8_t> buffer(kMaxDatagram + 64);
    uint64_t lastHello = 0, lastPing = 0, lastFromServer = 0;
    int helloAttempts = 0;

    while (running_) {
        const uint64_t now = NowMs();

        if (!connected_) {
            // Quick attempts first, then a slower retry that never gives up:
            // the server may simply not be running yet. Each slow retry is a
            // fresh handshake, so the server's replay filter never mistakes a
            // long wait for a replay.
            const bool slow = helloAttempts >= kHelloAttempts;
            const uint64_t interval = slow ? kHelloRetryMs : kHelloIntervalMs;
            if (now - lastHello >= interval) {
                if (helloAttempts == kHelloAttempts) {
                    SetStatus(L"No response from " + host_ + L":" + std::to_wstring(port_) +
                              L". Check the port forward and the key. Retrying…");
                }
                if (slow) BeginSession();
                if (helloAttempts <= kHelloAttempts) ++helloAttempts;
                SendHello();
                lastHello = now;
            }
        } else {
            if (now - lastPing >= kPingIntervalMs) {
                Writer w;
                w.U8(static_cast<uint8_t>(Msg::Ping));
                w.U64(now);
                Send(w.Data());
                lastPing = now;
            }
            if (now - lastFromServer > kServerTimeoutMs) {
                DropSession(L"Connection to " + host_ + L" timed out. Reconnecting…");
                helloAttempts = 0;
                lastHello = 0;
                continue;
            }
            // The list request is plain UDP: repeat it until answered, and
            // refresh the list now and then in case a change notice was lost.
            const uint64_t listEvery = listPending_ ? kListRetryMs : kListRefreshMs;
            if (now - lastListReqMs_ >= listEvery) RequestList(now);
            PollStreams(now);
        }

        Endpoint from;
        const int n = socket_.Receive(buffer.data(), buffer.size(), from, 2);
        if (n > 0 && from == server_) {
            const bool wasConnected = connected_;
            // Only a datagram that authenticated counts as a sign of life:
            // anyone can send from a forged address.
            if (HandleDatagram(buffer.data(), static_cast<size_t>(n), now) && connected_) {
                lastFromServer = now;
            }
            if (!wasConnected && connected_) {
                lastPing = now;
                helloAttempts = 0;
            }
            if (wasConnected && !connected_) {   // The server said goodbye.
                helloAttempts = 0;
                lastHello = now;
            }
        }
    }

    connected_ = false;
    queueCv_.notify_all();
    Notify(ClientEvent::StatusChanged);
}

// True if the datagram authenticated.
bool StreamClient::HandleDatagram(const uint8_t* data, size_t len, uint64_t nowMs) {
    if (!connected_) {
        // Expecting the WELCOME that answers this session's own HELLO: it must
        // echo our random, so a WELCOME recorded earlier is worthless.
        std::vector<uint8_t> plain;
        uint32_t serverSession = 0;
        if (!master_.Open(data, len, plain, serverSession) || plain.size() < 1 + 2 * kRandomBytes) {
            return false;
        }
        if (plain[0] != static_cast<uint8_t>(Msg::Welcome)) return false;
        if (memcmp(plain.data() + 1 + kRandomBytes, clientRandom_, kRandomBytes) != 0) return false;

        uint8_t serverRandom[kRandomBytes];
        memcpy(serverRandom, plain.data() + 1, kRandomBytes);
        {
            std::lock_guard lock(sendMutex_);
            channel_.SetKeys(
                DeriveSessionKey(masterKey_, clientRandom_, serverRandom, Direction::kClientToServer),
                DeriveSessionKey(masterKey_, clientRandom_, serverRandom, Direction::kServerToClient));
            channel_.BeginSend(clientSession_);
            channel_.BeginRecv(serverSession);
        }
        connected_ = true;
        Log(L"client: session established with %s", server_.ToString().c_str());
        SetStatus(L"Connected to " + host_ + L":" + std::to_wstring(port_));

        listPending_ = true;
        RequestList(nowMs);
        return true;
    }

    std::vector<uint8_t> plain;
    uint32_t sender = 0;
    if (!channel_.Open(data, len, plain, sender) || plain.empty()) return false;
    Reader r(plain.data(), plain.size());
    HandleMessage(r, nowMs);
    return true;
}

void StreamClient::HandleMessage(Reader& r, uint64_t nowMs) {
    uint8_t type = 0;
    if (!r.U8(type)) return;

    switch (static_cast<Msg>(type)) {
    case Msg::Frame: {
        FrameHeader h;
        if (!ReadFrameHeader(r, h)) return;
        auto s = FindStream(h.mirrorId);
        RVM_LOG_SAMPLED(500, L"client: frame packet mirror %u seq %u %u/%u flags %u stream=%s",
                        h.mirrorId, h.frameSeq, h.pktIdx, h.pktCount, h.flags, s ? L"yes" : L"NO");
        if (!s) {
            // Frames for a mirror we no longer want: our Unsubscribe was lost.
            uint64_t& last = unwantedMs_[h.mirrorId];
            if (nowMs - last >= kUnwantedMs) {
                last = nowMs;
                Writer w;
                w.U8(static_cast<uint8_t>(Msg::Unsubscribe));
                w.U32(h.mirrorId);
                Send(w.Data());
            }
            return;
        }

        std::vector<FrameAssembler::Frame> done;
        s->assembler.Accept(h, r.Ptr(), r.Left(), nowMs, done);
        if (done.empty()) return;
        RVM_LOG_SAMPLED(300, L"client: assembled frame seq %u, %zu bytes%s", done[0].frameSeq,
                        done[0].data.size(), done[0].keyframe ? L" (keyframe)" : L"");

        std::lock_guard lock(queueMutex_);
        for (auto& f : done) {
            // A decoder that cannot keep up must not build a latency backlog:
            // drop what is queued and restart from the next keyframe.
            if (s->queued >= kMaxQueuedPerStream) {
                for (auto it = queue_.begin(); it != queue_.end();) {
                    it = (it->first == s) ? queue_.erase(it) : it + 1;
                }
                s->queued = 0;
                s->assembler.Reset();
                break;
            }
            queue_.emplace_back(s, std::move(f));
            ++s->queued;
        }
        queueCv_.notify_one();
        break;
    }

    case Msg::ListResp: {
        uint8_t count = 0;
        if (!r.U8(count)) return;
        std::vector<RemoteMirror> list;
        for (uint8_t i = 0; i < count; ++i) {
            RemoteMirror m;
            uint16_t w = 0, h = 0;
            std::string name;
            if (!r.U32(m.id) || !r.U16(w) || !r.U16(h) || !r.Str(name)) break;
            m.width = w;
            m.height = h;
            m.name = FromUtf8(name);
            list.push_back(std::move(m));
        }
        Log(L"client: mirror list has %zu entries", list.size());
        for (const auto& m : list) Log(L"client:   id=%u %ux%u '%s'", m.id, m.width, m.height, m.name.c_str());
        listPending_ = false;
        unwantedMs_.clear();
        const uint64_t session = session_.load();
        std::vector<uint32_t> subscribe;
        {
            std::lock_guard lock(stateMutex_);
            mirrors_ = std::move(list);
            const auto present = [&](uint32_t id) {
                return std::any_of(mirrors_.begin(), mirrors_.end(),
                                   [&](const RemoteMirror& m) { return m.id == id; });
            };
            // Mirrors that no longer exist are dropped, wish and all.
            for (auto it = streams_.begin(); it != streams_.end();) {
                it = present(it->first) ? std::next(it) : streams_.erase(it);
            }
            for (auto it = wanted_.begin(); it != wanted_.end();) {
                it = present(*it) ? std::next(it) : wanted_.erase(it);
            }
            // Wanted but not yet subscribed in this session: chosen while the
            // link was down, or carried over a reconnect.
            for (uint32_t id : wanted_) {
                auto& slot = streams_[id];
                if (!slot) slot = std::make_shared<Stream>(id);
                if (slot->subscribedIn.exchange(session) != session) subscribe.push_back(id);
            }
        }
        for (uint32_t id : subscribe) {
            Writer w;
            w.U8(static_cast<uint8_t>(Msg::Subscribe));
            w.U32(id);
            Send(w.Data());
            Log(L"client: subscribed to mirror %u (restored)", id);
        }
        Notify(ClientEvent::ListUpdated);
        break;
    }

    case Msg::MirrorsChanged:
        listPending_ = true;
        RequestList(nowMs);
        break;

    case Msg::Pong: {
        uint64_t sent = 0;
        if (r.U64(sent)) rttMs_ = static_cast<int>(nowMs - sent);
        Notify(ClientEvent::StatusChanged);
        break;
    }

    case Msg::Bye:
        DropSession(L"The server closed the connection. Reconnecting…");
        break;

    default:
        break;
    }
}

void StreamClient::PollStreams(uint64_t nowMs) {
    std::vector<std::shared_ptr<Stream>> streams;
    {
        std::lock_guard lock(stateMutex_);
        for (auto& [id, s] : streams_) streams.push_back(s);
    }

    for (auto& s : streams) {
        std::vector<FrameAssembler::Missing> nacks;
        s->assembler.Poll(nowMs, nacks);
        for (const auto& m : nacks) {
            // A frame with very many losses is asked for in several messages,
            // each small enough for one datagram.
            for (size_t first = 0; first < m.indices.size(); first += kMaxNackIndices) {
                const size_t n = (std::min)(kMaxNackIndices, m.indices.size() - first);
                Writer w;
                w.U8(static_cast<uint8_t>(Msg::Nack));
                w.U32(s->id);
                w.U32(m.frameSeq);
                w.U16(static_cast<uint16_t>(n));
                for (size_t i = 0; i < n; ++i) w.U16(m.indices[first + i]);
                Send(w.Data());
            }
        }
        if (s->assembler.NeedKeyframe() &&
            nowMs - s->assembler.LastKeyframeRequestMs() >= kKeyframeThrottleMs) {
            Writer w;
            w.U8(static_cast<uint8_t>(Msg::KeyframeReq));
            w.U32(s->id);
            Send(w.Data());
            s->assembler.MarkKeyframeRequested(nowMs);
            RVM_LOG_SAMPLED(25, L"client: keyframe requested for mirror %u", s->id);
        }
    }
}

// ---------------------------------------------------------------------------
// Decode thread

void StreamClient::DecodeLoop() {
    // The decoders are Media Foundation objects, which live in the
    // multithreaded apartment; this thread joins it explicitly.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_) {
        std::pair<std::shared_ptr<Stream>, FrameAssembler::Frame> item;
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (!running_) break;
            item = std::move(queue_.front());
            queue_.pop_front();
            if (item.first->queued > 0) --item.first->queued;
        }
        DecodeOne(*item.first, item.second);
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void StreamClient::DecodeOne(Stream& s, FrameAssembler::Frame& frame) {
    if (!s.decoder.Ready()) {
        const bool ok = s.decoder.Init();
        Log(L"client: decoder init for mirror %u -> %s", s.id, ok ? L"ok" : L"FAILED");
        if (!ok) return;
    }
    if (!s.converterReady) s.converterReady = s.converter.Init();
    if (!s.converterReady) {
        Log(L"client: video processor unavailable for mirror %u", s.id);
        return;
    }

    auto& g = Gfx::Get();
    std::vector<DecodedFrame> decoded;
    bool ok = false;
    {
        std::lock_guard device(g.deviceMutex);
        ok = s.decoder.Decode(frame.data.data(), frame.data.size(), decoded);
    }
    RVM_LOG_SAMPLED(300, L"client: decode mirror %u seq %u %zu bytes%s -> ok=%d out=%zu", s.id,
                    frame.frameSeq, frame.data.size(), frame.keyframe ? L" (keyframe)" : L"",
                    ok ? 1 : 0, decoded.size());
    if (!ok || decoded.empty()) return;

    // Only the newest decoded frame matters for display.
    const DecodedFrame& d = decoded.back();
    const UINT picW = static_cast<UINT>(RectW(d.display));
    const UINT picH = static_cast<UINT>(RectH(d.display));
    if (picW == 0 || picH == 0) return;

    winrt::com_ptr<ID3D11Texture2D> target;
    {
        std::lock_guard lock(stateMutex_);
        if (!s.texture || s.width != picW || s.height != picH) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = picW; td.Height = picH; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            winrt::com_ptr<ID3D11Texture2D> fresh;
            const HRESULT hr = g.d3d->CreateTexture2D(&td, nullptr, fresh.put());
            if (FAILED(hr)) {
                g.CheckDevice(hr);
                return;
            }
            s.texture = fresh;
            s.width = picW;
            s.height = picH;
        }
        target = s.texture;
    }

    {
        std::lock_guard device(g.deviceMutex);
        s.converter.Convert(d.texture.get(), d.subresource, &d.display, target.get());
    }
    {
        std::lock_guard lock(stateMutex_);
        ++s.frames;
    }
    if (!framePosted_.exchange(true)) Notify(ClientEvent::FrameReady, s.id);
}

}  // namespace rvm
