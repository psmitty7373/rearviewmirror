#pragma once
#include "gfx.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

namespace rvm::net {

struct EncodedFrame {
    std::vector<uint8_t> data;   // Annex B access unit.
    bool keyframe = false;
};

struct DecodedFrame {
    winrt::com_ptr<ID3D11Texture2D> texture;   // NV12, often an array slice.
    UINT subresource = 0;
    // The picture within the texture: DXVA pads to macroblock rows, and the
    // padding holds garbage. Always blit from this, never the whole surface.
    RECT display{};
};

class EncoderEventRelay;

// H.264 through the GPU's Media Foundation encoder. Input is an NV12 texture
// on the shared device; nothing is read back to the CPU except the bitstream.
//
// Hardware encoders are asynchronous: they announce "want input" and "have
// output" as events. Those arrive on a Media Foundation thread, are queued,
// and wake whoever waits on the event given to SetWakeEvent(). All work on the
// encoder itself happens in Service() and Encode(), on one thread, and
// nothing blocks: a frame costs only the encoder's own time.
class H264Encoder {
public:
    ~H264Encoder();

    // Signalled whenever the encoder has something to say. Duplicated, so the
    // caller may close its handle whenever it likes. Set before Init().
    void SetWakeEvent(HANDLE wake) { wake_ = wake; }

    bool Init(UINT width, UINT height, UINT fps, UINT bitrateBps);
    void Shutdown();
    bool Ready() const { return mft_ != nullptr; }

    UINT Width()  const { return width_; }
    UINT Height() const { return height_; }

    // Handles every event that has arrived: collects finished frames into
    // `out`, notes requests for input. Never blocks. False if the encoder has
    // failed and should be recreated.
    bool Service(std::vector<EncodedFrame>& out);
    bool Drain(std::vector<EncodedFrame>& out) { return Service(out); }

    // Whether Encode() will take a frame now.
    bool WantsInput() const { return inputsWanted_ > 0; }

    // Hands one frame to the encoder, if it wants input; the encoded result
    // arrives later through Service(). Anything already finished is
    // collected into `out`. False if the frame was not taken.
    bool Encode(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out);

    // For tests and benchmarks: waits for the encoder to want input, feeds
    // it, and waits until at least one frame comes out, up to `timeoutMs`.
    bool EncodeSync(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out, DWORD timeoutMs = 200);

    // The next encoded frame will be an IDR. Safe from any thread.
    void RequestKeyframe() { forceKeyframe_.store(true, std::memory_order_relaxed); }

    // Some encoders (Intel Quick Sync among them) keep a frame back until the
    // next one arrives, however low-latency they are asked to be. True when
    // the last real frame has produced nothing for a while: the caller should
    // feed the same picture again with Repeat() to push it out.
    bool NeedsNudge(uint64_t nowMs) const;
    bool Repeat(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out);

    const std::wstring& Name() const { return name_; }

private:
    bool TryInit(IMFActivate* activate);
    bool WaitForEvents(DWORD timeoutMs);
    bool CollectOutput(std::vector<EncodedFrame>& out);
    bool RenegotiateOutput();
    void LogError(const wchar_t* step, HRESULT hr);

    static constexpr int kMaxErrorLogs = 50;
    static constexpr int kTraceFrames = 3;   // First inputs and outputs of each encoder.
    int errorsLogged_ = 0;
    int inputsTraced_ = 0;
    int outputsTraced_ = 0;
    std::vector<uint8_t> sequenceHeader_;   // SPS/PPS from the output format, Annex B.

    winrt::com_ptr<IMFTransform>           mft_;
    winrt::com_ptr<IMFMediaEventGenerator> events_;
    winrt::com_ptr<ICodecAPI>              codec_;
    EncoderEventRelay* relay_ = nullptr;   // COM-refcounted; see codec.cpp.
    HANDLE wake_ = nullptr;
    bool   failed_ = false;
    DWORD inputId_ = 0, outputId_ = 0;
    bool  providesSamples_ = false;
    int   inputsWanted_ = 0;
    std::atomic<bool> forceKeyframe_{ false };
    UINT  width_ = 0, height_ = 0, fps_ = 60, bitrate_ = 0;
    LONGLONG frameIndex_ = 0;
    std::wstring name_;

    bool     awaitingOutput_ = false;   // A real frame went in; nothing has come out since.
    uint64_t lastFedMs_ = 0;
    int      repeats_ = 0;
};

// Every hardware H.264 encoder, the ones on the shared device's own adapter
// first. An encoder on another GPU cannot read that device's textures.
std::vector<winrt::com_ptr<IMFActivate>> HardwareEncoders();

// Media Foundation H.264 decoder with DXVA output: decoded frames land in NV12
// textures on the shared device. A decoded texture belongs to the decoder's
// pool and must be consumed before the next call.
class H264Decoder {
public:
    ~H264Decoder();

    bool Init();
    void Shutdown();
    bool Ready() const { return mft_ != nullptr; }

    UINT Width()  const { return width_; }
    UINT Height() const { return height_; }

    bool Decode(const uint8_t* data, size_t len, std::vector<DecodedFrame>& out);

private:
    bool NegotiateOutput();
    bool Drain(std::vector<DecodedFrame>& out);

    winrt::com_ptr<IMFTransform> mft_;
    DWORD inputId_ = 0, outputId_ = 0;
    bool  providesSamples_ = false;
    UINT  width_ = 0, height_ = 0;   // Display size, after cropping.
    RECT  display_{};
    LONGLONG frameIndex_ = 0;
};

// Human-readable name of the hardware encoder in use, or empty if none.
std::wstring HardwareEncoderName();

}  // namespace rvm::net
