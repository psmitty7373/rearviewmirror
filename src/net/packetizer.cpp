#include "net/packetizer.h"

namespace rvm::net {

namespace {

uint8_t* PutLe(uint8_t* p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) *p++ = static_cast<uint8_t>(v >> (8 * i));
    return p;
}

}  // namespace

// The header is written in place, field for field as WriteFrameHeader does.
PacketizedFrame::PacketizedFrame(uint32_t mirrorId, uint32_t frameSeq, bool keyframe,
                                 const uint8_t* data, size_t len)
    : seq_(frameSeq) {
    count_ = (std::max)(static_cast<size_t>(1), (len + kMaxChunk - 1) / kMaxChunk);
    buf_.resize(count_ * kFrameHeaderBytes + len);
    for (size_t i = 0; i < count_; ++i) {
        const size_t offset = i * kMaxChunk;
        const size_t chunk = (std::min)(kMaxChunk, len - offset);
        uint8_t* p = buf_.data() + i * kMaxPlain;
        *p++ = static_cast<uint8_t>(Msg::Frame);
        p = PutLe(p, mirrorId, 4);
        p = PutLe(p, frameSeq, 4);
        p = PutLe(p, static_cast<uint32_t>(i), 2);
        p = PutLe(p, static_cast<uint32_t>(count_), 2);
        *p++ = keyframe ? kFlagKeyframe : 0;
        if (chunk) memcpy(p, data + offset, chunk);
    }
}

PacketizedFrame::Packet PacketizedFrame::operator[](size_t i) const {
    if (i >= count_) return {};
    const size_t offset = i * kMaxPlain;
    return { buf_.data() + offset, (std::min)(kMaxPlain, buf_.size() - offset) };
}

const PacketizedFrame& FrameSender::Packetize(uint32_t frameSeq, bool keyframe,
                                              const uint8_t* data, size_t len) {
    static const PacketizedFrame dropped;
    const auto frame = Add(frameSeq, keyframe, data, len);
    return frame ? *frame : dropped;
}

std::shared_ptr<const PacketizedFrame> FrameSender::Add(uint32_t frameSeq, bool keyframe,
                                                        const uint8_t* data, size_t len) {
    if (len > kMaxFramePackets * kMaxChunk) {
        Log(L"server: mirror %u frame %u is %zu bytes, more than a frame may be; dropped",
            mirrorId_, frameSeq, len);
        return nullptr;
    }
    auto frame = std::make_shared<const PacketizedFrame>(mirrorId_, frameSeq, keyframe, data, len);
    history_.push_back(frame);
    while (history_.size() > kHistory) history_.pop_front();
    return frame;
}

std::shared_ptr<const PacketizedFrame> FrameSender::Find(uint32_t frameSeq) const {
    for (const auto& f : history_) {
        if (f->Seq() == frameSeq) return f;
    }
    return nullptr;
}

const PacketizedFrame* FrameSender::Packets(uint32_t frameSeq) const {
    for (const auto& f : history_) {
        if (f->Seq() == frameSeq) return f.get();
    }
    return nullptr;
}

PacketizedFrame::Packet FrameSender::Lookup(uint32_t frameSeq, uint16_t pktIdx) const {
    const auto* packets = Packets(frameSeq);
    return packets ? (*packets)[pktIdx] : PacketizedFrame::Packet{};
}

void FrameAssembler::Reset() {
    Clear();
    started_ = false;
}

void FrameAssembler::Clear() {
    pending_.clear();
    pendingPackets_ = 0;
    needKeyframe_ = true;
    gapNackMs_ = 0;
}

FrameAssembler::PendingMap::iterator FrameAssembler::Erase(PendingMap::iterator it) {
    pendingPackets_ -= it->second.count;
    return pending_.erase(it);
}

void FrameAssembler::Accept(const FrameHeader& h, const uint8_t* chunk, size_t len,
                            uint64_t nowMs, std::vector<Frame>& out) {
    if (h.pktCount == 0 || h.pktCount > kMaxFramePackets || h.pktIdx >= h.pktCount) return;
    // Every chunk but the last is full: that is what lets chunks land at
    // fixed offsets.
    const bool last = h.pktIdx + 1 == h.pktCount;
    if (len > kMaxChunk || (!last && len != kMaxChunk)) return;
    if (started_ && static_cast<int32_t>(h.frameSeq - nextSeq_) < 0) return;   // Stale.

    unansweredKeyframeReqs_ = 0;
    if (!started_ || static_cast<int32_t>(h.frameSeq - newestSeq_) > 0) newestSeq_ = h.frameSeq;
    if (!started_) {
        started_ = true;
        nextSeq_ = h.frameSeq;
    }

    auto it = pending_.find(h.frameSeq);
    if (it == pending_.end()) {
        if (pending_.size() >= kMaxPending || pendingPackets_ + h.pktCount > kMaxPendingPackets) {
            // Hopelessly behind; start over from the next keyframe.
            Clear();
        }
        it = pending_.emplace(h.frameSeq, Pending{}).first;
        Pending& p = it->second;
        p.count = h.pktCount;
        p.flags = h.flags;
        p.firstMs = nowMs;
        p.data.resize(static_cast<size_t>(h.pktCount) * kMaxChunk);
        p.got.resize(h.pktCount);
        pendingPackets_ += h.pktCount;
    }
    Pending& p = it->second;
    if (p.count != h.pktCount || p.got[h.pktIdx]) return;   // Duplicate or bogus.

    if (len) memcpy(p.data.data() + static_cast<size_t>(h.pktIdx) * kMaxChunk, chunk, len);
    if (last) p.lastLen = len;
    p.got[h.pktIdx] = true;
    p.lastMs = nowMs;
    if (++p.have == p.count) Deliver(out);
}

// Hands out every frame that is complete and next in sequence. After a loss,
// only a keyframe can restart the chain, and anything before it is discarded.
void FrameAssembler::Deliver(std::vector<Frame>& out) {
    for (;;) {
        if (needKeyframe_) {
            // Find the first complete keyframe; drop everything before it.
            auto it = pending_.begin();
            while (it != pending_.end() &&
                   !((it->second.flags & kFlagKeyframe) && it->second.have == it->second.count)) {
                ++it;
            }
            if (it == pending_.end()) return;
            while (pending_.begin() != it) Erase(pending_.begin());
            nextSeq_ = it->first;
            needKeyframe_ = false;
            gapNackMs_ = 0;
        }

        auto it = pending_.find(nextSeq_);
        if (it == pending_.end() || it->second.have != it->second.count) return;

        Pending& p = it->second;
        Frame f;
        f.frameSeq = it->first;
        f.keyframe = (p.flags & kFlagKeyframe) != 0;
        p.data.resize(static_cast<size_t>(p.count - 1) * kMaxChunk + p.lastLen);
        f.data = std::move(p.data);
        out.push_back(std::move(f));

        Erase(it);
        ++nextSeq_;
    }
}

FrameAssembler::PendingMap::const_iterator FrameAssembler::FirstAfterGap(uint32_t& gapStart) const {
    uint32_t expect = nextSeq_;
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->first != expect) {
            gapStart = expect;
            return it;
        }
        expect = it->first + 1;
    }
    return pending_.end();
}

void FrameAssembler::Poll(uint64_t nowMs, std::vector<Missing>& nacks) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        Pending& p = it->second;
        if (p.have == p.count) {
            ++it;
            continue;
        }
        if (nowMs - p.lastMs > kDropAfterMs) {
            it = Erase(it);
            needKeyframe_ = true;
            continue;
        }

        // Only NACK once a later frame has started arriving: until then the
        // missing chunks may simply be in flight.
        const bool laterSeen = static_cast<int32_t>(newestSeq_ - it->first) > 0;
        const bool due = (p.lastNackMs == 0) ? nowMs - p.firstMs >= kNackAfterMs
                                             : nowMs - p.lastNackMs >= kNackRepeatMs;
        if (laterSeen && due) {
            Missing m;
            m.frameSeq = it->first;
            for (uint16_t i = 0; i < p.count; ++i) {
                if (!p.got[i]) m.indices.push_back(i);
            }
            nacks.push_back(std::move(m));
            p.lastNackMs = nowMs;
        }
        ++it;
    }

    // A frame lost whole leaves no trace but a gap before a later one. Its
    // first packet is asked for, which says how many more there are.
    if (needKeyframe_) return;
    uint32_t gapStart = 0;
    const auto after = FirstAfterGap(gapStart);
    if (after == pending_.end()) {
        gapNackMs_ = 0;
        return;
    }
    const uint64_t since = after->second.firstMs;
    if (gapStart == nextSeq_ && nowMs - since > kDropAfterMs) {
        needKeyframe_ = true;
        gapNackMs_ = 0;
        return;
    }
    const bool due = gapNackMs_ == 0 ? nowMs - since >= kNackAfterMs
                                     : nowMs - gapNackMs_ >= kNackRepeatMs;
    if (!due) return;
    for (uint32_t seq = gapStart; seq != after->first && nacks.size() < kMaxGapNacks; ++seq) {
        nacks.push_back({ seq, { 0 } });
    }
    gapNackMs_ = nowMs;
}

bool FrameAssembler::KeyframeArriving(uint64_t nowMs) const {
    return std::any_of(pending_.begin(), pending_.end(), [&](const auto& entry) {
        const Pending& p = entry.second;
        return (p.flags & kFlagKeyframe) && p.have < p.count && nowMs - p.lastMs <= kDropAfterMs;
    });
}

uint64_t FrameAssembler::KeyframeRetryMs() const {
    const int step = (std::min)((std::max)(unansweredKeyframeReqs_, 1), kKeyframeBackoffSteps);
    return kKeyframeRetryMs << (step - 1);
}

bool FrameAssembler::KeyframeRequestDue(uint64_t nowMs) const {
    return needKeyframe_ && nowMs - lastKeyframeReqMs_ >= KeyframeRetryMs() &&
           !KeyframeArriving(nowMs);
}

void FrameAssembler::MarkKeyframeRequested(uint64_t nowMs) {
    lastKeyframeReqMs_ = nowMs;
    unansweredKeyframeReqs_ = (std::min)(unansweredKeyframeReqs_ + 1, kKeyframeBackoffSteps);
}

uint64_t FrameAssembler::NextDueMs() const {
    uint64_t due = UINT64_MAX;
    bool arriving = false;
    for (const auto& [seq, p] : pending_) {
        if (p.have == p.count) continue;
        due = (std::min)(due, p.lastMs + kDropAfterMs + 1);
        if (static_cast<int32_t>(newestSeq_ - seq) > 0) {
            due = (std::min)(due, p.lastNackMs == 0 ? p.firstMs + kNackAfterMs
                                                    : p.lastNackMs + kNackRepeatMs);
        }
        arriving = arriving || (p.flags & kFlagKeyframe);
    }
    if (needKeyframe_) {
        // An arriving keyframe's own drop time above covers the wait for it.
        if (!arriving) due = (std::min)(due, lastKeyframeReqMs_ + KeyframeRetryMs());
        return due;
    }
    uint32_t gapStart = 0;
    const auto after = FirstAfterGap(gapStart);
    if (after != pending_.end()) {
        const uint64_t since = after->second.firstMs;
        due = (std::min)(due, gapNackMs_ == 0 ? since + kNackAfterMs : gapNackMs_ + kNackRepeatMs);
        if (gapStart == nextSeq_) due = (std::min)(due, since + kDropAfterMs + 1);
    }
    return due;
}

}  // namespace rvm::net
