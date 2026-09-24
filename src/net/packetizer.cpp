#include "net/packetizer.h"

namespace rvm::net {

const std::vector<std::vector<uint8_t>>& FrameSender::Packetize(uint32_t frameSeq, bool keyframe,
                                                                const uint8_t* data, size_t len) {
    const size_t count = (std::max)(static_cast<size_t>(1), (len + kMaxChunk - 1) / kMaxChunk);

    Stored stored;
    stored.seq = frameSeq;
    stored.packets.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const size_t offset = i * kMaxChunk;
        const size_t chunk  = (std::min)(kMaxChunk, len - offset);

        Writer w;
        FrameHeader h;
        h.mirrorId = mirrorId_;
        h.frameSeq = frameSeq;
        h.pktIdx   = static_cast<uint16_t>(i);
        h.pktCount = static_cast<uint16_t>(count);
        h.flags    = keyframe ? kFlagKeyframe : 0;
        WriteFrameHeader(w, h);
        w.Bytes(data + offset, chunk);
        stored.packets.push_back(std::move(w.Data()));
    }

    history_.push_back(std::move(stored));
    while (history_.size() > kHistory) history_.pop_front();
    return history_.back().packets;
}

const std::vector<uint8_t>* FrameSender::Lookup(uint32_t frameSeq, uint16_t pktIdx) const {
    const auto* packets = Packets(frameSeq);
    return (packets && pktIdx < packets->size()) ? &(*packets)[pktIdx] : nullptr;
}

const std::vector<std::vector<uint8_t>>* FrameSender::Packets(uint32_t frameSeq) const {
    for (const auto& s : history_) {
        if (s.seq == frameSeq) return &s.packets;
    }
    return nullptr;
}

void FrameAssembler::Reset() {
    pending_.clear();
    started_ = false;
    needKeyframe_ = true;
}

void FrameAssembler::Accept(const FrameHeader& h, const uint8_t* chunk, size_t len,
                            uint64_t nowMs, std::vector<Frame>& out) {
    if (h.pktCount == 0 || h.pktIdx >= h.pktCount) return;
    if (started_ && static_cast<int32_t>(h.frameSeq - nextSeq_) < 0) return;   // Stale.

    if (!started_ || static_cast<int32_t>(h.frameSeq - newestSeq_) > 0) newestSeq_ = h.frameSeq;
    if (!started_) {
        started_ = true;
        nextSeq_ = h.frameSeq;
    }

    Pending& p = pending_[h.frameSeq];
    if (p.count == 0) {
        p.count = h.pktCount;
        p.flags = h.flags;
        p.firstMs = nowMs;
        p.chunks.resize(h.pktCount);
    }
    if (p.count != h.pktCount || !p.chunks[h.pktIdx].empty()) return;   // Duplicate or bogus.

    p.chunks[h.pktIdx].assign(chunk, chunk + len);
    ++p.have;

    if (pending_.size() > kMaxPending) {
        // Hopelessly behind; start over from the next keyframe.
        pending_.clear();
        needKeyframe_ = true;
        return;
    }
    Deliver(out);
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
            pending_.erase(pending_.begin(), it);
            nextSeq_ = it->first;
            needKeyframe_ = false;
        }

        auto it = pending_.find(nextSeq_);
        if (it == pending_.end() || it->second.have != it->second.count) return;

        Frame f;
        f.frameSeq = it->first;
        f.keyframe = (it->second.flags & kFlagKeyframe) != 0;
        size_t total = 0;
        for (const auto& c : it->second.chunks) total += c.size();
        f.data.reserve(total);
        for (const auto& c : it->second.chunks) f.data.insert(f.data.end(), c.begin(), c.end());
        out.push_back(std::move(f));

        pending_.erase(it);
        ++nextSeq_;
    }
}

void FrameAssembler::Poll(uint64_t nowMs, std::vector<Missing>& nacks) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        Pending& p = it->second;
        if (p.have == p.count) {
            ++it;
            continue;
        }

        const uint64_t age = nowMs - p.firstMs;
        if (age > kDropAfterMs) {
            it = pending_.erase(it);
            needKeyframe_ = true;
            continue;
        }

        // Only NACK once a later frame has started arriving: until then the
        // missing chunks may simply be in flight.
        const bool laterSeen = static_cast<int32_t>(newestSeq_ - it->first) > 0;
        const bool due = (p.lastNackMs == 0) ? age >= kNackAfterMs
                                             : nowMs - p.lastNackMs >= kNackRepeatMs;
        if (laterSeen && due) {
            Missing m;
            m.frameSeq = it->first;
            for (uint16_t i = 0; i < p.count; ++i) {
                if (p.chunks[i].empty()) m.indices.push_back(i);
            }
            nacks.push_back(std::move(m));
            p.lastNackMs = nowMs;
        }
        ++it;
    }
}

}  // namespace rvm::net
