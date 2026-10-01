#pragma once
#include "net/protocol.h"

namespace rvm::net {

// One encoded frame as datagram plaintexts, all in one buffer: every packet
// but the last is exactly kMaxPlain bytes.
class PacketizedFrame {
public:
    // A view into the frame's buffer.
    class Packet {
    public:
        Packet() = default;
        Packet(const uint8_t* data, size_t size) : data_(data), size_(size) {}
        const uint8_t* data() const { return data_; }
        size_t size() const { return size_; }
        explicit operator bool() const { return data_ != nullptr; }
        // A copy, for callers that keep a packet beyond the frame's life.
        operator std::vector<uint8_t>() const { return { data_, data_ + size_ }; }

    private:
        const uint8_t* data_ = nullptr;
        size_t size_ = 0;
    };

    class Iterator {
    public:
        Iterator(const PacketizedFrame* frame, size_t i) : frame_(frame), i_(i) {}
        Packet operator*() const { return (*frame_)[i_]; }
        Iterator& operator++() { ++i_; return *this; }
        bool operator!=(const Iterator& o) const { return i_ != o.i_; }

    private:
        const PacketizedFrame* frame_;
        size_t i_;
    };

    PacketizedFrame() = default;
    PacketizedFrame(uint32_t mirrorId, uint32_t frameSeq, bool keyframe, const uint8_t* data,
                    size_t len);

    uint32_t Seq() const { return seq_; }
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    Packet operator[](size_t i) const;
    Iterator begin() const { return { this, 0 }; }
    Iterator end() const { return { this, count_ }; }

private:
    uint32_t seq_ = 0;
    size_t count_ = 0;
    std::vector<uint8_t> buf_;
};

// Server side. Splits encoded frames into packets and keeps recent ones so a
// NACK can be answered without re-encoding.
class FrameSender {
public:
    explicit FrameSender(uint32_t mirrorId) : mirrorId_(mirrorId) {}

    // The packets for `data`, retained for retransmission. A frame of more
    // than kMaxFramePackets is dropped, and the result is empty.
    const PacketizedFrame& Packetize(uint32_t frameSeq, bool keyframe, const uint8_t* data,
                                     size_t len);

    // The same, shared: the caller can seal and send the packets, or answer a
    // NACK from them later, without holding whatever guards this sender. Null
    // for a dropped frame.
    std::shared_ptr<const PacketizedFrame> Add(uint32_t frameSeq, bool keyframe,
                                               const uint8_t* data, size_t len);
    // A retained frame, or null once it has aged out.
    std::shared_ptr<const PacketizedFrame> Find(uint32_t frameSeq) const;

    // Every packet of a retained frame, or nullptr if it has aged out.
    const PacketizedFrame* Packets(uint32_t frameSeq) const;
    PacketizedFrame::Packet Lookup(uint32_t frameSeq, uint16_t pktIdx) const;

private:
    static constexpr size_t kHistory = 32;

    uint32_t mirrorId_;
    std::deque<std::shared_ptr<const PacketizedFrame>> history_;
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

    // NACKs for frames stuck behind a gap, and dropping the hopeless ones.
    // Has work only at NextDueMs().
    void Poll(uint64_t nowMs, std::vector<Missing>& nacks);

    // A frame was lost for good; decoding cannot resume until a keyframe.
    bool NeedKeyframe() const { return needKeyframe_; }

    // A keyframe is needed, none is on its way, and the last request has had
    // its time. Requests that go unanswered are spaced out further.
    bool KeyframeRequestDue(uint64_t nowMs) const;
    // A Subscribe counts too: the server answers it with a keyframe.
    void MarkKeyframeRequested(uint64_t nowMs);

    // When Poll or a keyframe request next has work; UINT64_MAX while no
    // frame is incomplete and no keyframe is wanted.
    uint64_t NextDueMs() const;

    void Reset();

private:
    struct Pending {
        uint16_t count = 0;
        uint16_t have = 0;
        uint8_t  flags = 0;
        size_t   lastLen = 0;    // The last chunk's, which may be short.
        uint64_t firstMs = 0;
        uint64_t lastMs = 0;     // Latest chunk: a frame still arriving is not lost.
        uint64_t lastNackMs = 0;
        std::vector<uint8_t> data;   // Chunk i at i * kMaxChunk.
        std::vector<bool> got;
    };
    using PendingMap = std::map<uint32_t, Pending>;   // Ordered by frameSeq.

    static constexpr uint64_t kNackAfterMs = 4;    // Once a later frame has begun.
    static constexpr uint64_t kNackRepeatMs = 15;
    static constexpr uint64_t kDropAfterMs = 80;   // Without a chunk arriving.
    static constexpr size_t   kMaxPending = 64;
    static constexpr size_t   kMaxPendingPackets = 2 * kMaxFramePackets;
    static constexpr size_t   kMaxGapNacks = 16;
    static constexpr uint64_t kKeyframeRetryMs = 200;
    static constexpr int      kKeyframeBackoffSteps = 4;   // Up to 1.6 s apart.

    void Deliver(std::vector<Frame>& out);
    PendingMap::iterator Erase(PendingMap::iterator it);
    void Clear();
    bool KeyframeArriving(uint64_t nowMs) const;
    uint64_t KeyframeRetryMs() const;
    // The oldest frame after a run of frames that never arrived at all; the
    // missing run starts at `gapStart`. End if there is no such gap.
    PendingMap::const_iterator FirstAfterGap(uint32_t& gapStart) const;

    PendingMap pending_;
    size_t pendingPackets_ = 0;
    uint32_t nextSeq_ = 0;
    uint32_t newestSeq_ = 0;
    bool started_ = false;
    bool needKeyframe_ = true;
    uint64_t gapNackMs_ = 0;
    uint64_t lastKeyframeReqMs_ = 0;
    int unansweredKeyframeReqs_ = 0;
};

}  // namespace rvm::net
