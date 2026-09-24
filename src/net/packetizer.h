#pragma once
#include "net/protocol.h"

namespace rvm::net {

// Server side. Splits an encoded frame into datagram-sized plaintexts and keeps
// recent frames so a NACK can be answered without re-encoding.
class FrameSender {
public:
    explicit FrameSender(uint32_t mirrorId) : mirrorId_(mirrorId) {}

    // Plaintext payloads for `data`, retained for retransmission.
    const std::vector<std::vector<uint8_t>>& Packetize(uint32_t frameSeq, bool keyframe,
                                                       const uint8_t* data, size_t len);

    const std::vector<uint8_t>* Lookup(uint32_t frameSeq, uint16_t pktIdx) const;

    // Every packet of a retained frame, or nullptr if it has aged out.
    const std::vector<std::vector<uint8_t>>* Packets(uint32_t frameSeq) const;

private:
    struct Stored {
        uint32_t seq;
        std::vector<std::vector<uint8_t>> packets;
    };
    static constexpr size_t kHistory = 32;

    uint32_t mirrorId_;
    std::deque<Stored> history_;
};

// Client side. Reassembles frames from chunks, hands them out in order, and
// decides when to ask for a resend or give up and ask for a keyframe.
class FrameAssembler {
public:
    struct Frame {
        uint32_t frameSeq = 0;
        bool keyframe = false;
        std::vector<uint8_t> data;
    };

    struct Missing {
        uint32_t frameSeq = 0;
        std::vector<uint16_t> indices;
    };

    // Feed one chunk. Completed frames, in order, are appended to `out`.
    void Accept(const FrameHeader& h, const uint8_t* chunk, size_t len, uint64_t nowMs,
                std::vector<Frame>& out);

    // Housekeeping: NACKs for frames stuck behind a gap, and dropping the
    // hopeless ones. Call a few hundred times a second.
    void Poll(uint64_t nowMs, std::vector<Missing>& nacks);

    // A frame was lost for good; decoding cannot resume until a keyframe.
    bool NeedKeyframe() const { return needKeyframe_; }

    // Time since the last keyframe request, so the client can throttle them.
    uint64_t LastKeyframeRequestMs() const { return lastKeyframeReqMs_; }
    void MarkKeyframeRequested(uint64_t nowMs) { lastKeyframeReqMs_ = nowMs; }

    void Reset();

private:
    struct Pending {
        uint16_t count = 0;
        uint16_t have = 0;
        uint8_t  flags = 0;
        uint64_t firstMs = 0;
        uint64_t lastNackMs = 0;
        std::vector<std::vector<uint8_t>> chunks;
    };

    static constexpr uint64_t kNackAfterMs = 4;    // Once a later frame has begun.
    static constexpr uint64_t kNackRepeatMs = 15;
    static constexpr uint64_t kDropAfterMs = 80;
    static constexpr size_t   kMaxPending = 64;

    void Deliver(std::vector<Frame>& out);

    std::map<uint32_t, Pending> pending_;   // Ordered by frameSeq.
    uint32_t nextSeq_ = 0;
    uint32_t newestSeq_ = 0;
    bool started_ = false;
    bool needKeyframe_ = true;
    uint64_t lastKeyframeReqMs_ = 0;
};

}  // namespace rvm::net
