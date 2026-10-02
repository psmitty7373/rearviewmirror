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

enum class EncoderKind {
    Hardware,   // Input: an NV12 texture on the shared device.
    Software,   // Windows' CPU encoder. Input: an R8 staging texture packed as NV12 (see Nv12Packer).
};

// H.264 through Media Foundation, used from one thread. A hardware encoder is
// asynchronous: its events wake the SetWakeEvent() handle and are handled in
// Service(). The software encoder is synchronous: Encode() returns its output.
class H264Encoder {
public:
    H264Encoder() = default;
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;
    ~H264Encoder();

    // Signalled whenever the encoder has something to say. Duplicated, so the
    // caller may close its handle whenever it likes. Set before Init().
    void SetWakeEvent(HANDLE wake) { wake_ = wake; }

    // `qualityVsSpeed`: 0 fastest to 100 best, as the encoder's presets see
    // it; kEncoderDefault leaves the encoder's own choice.
    static constexpr UINT kEncoderDefault = 0xFFFFFFFFu;
    bool Init(UINT width, UINT height, UINT fps, UINT bitrateBps,
              UINT qualityVsSpeed = kEncoderDefault, EncoderKind kind = EncoderKind::Hardware);
    void Shutdown();
    bool Ready() const { return mft_ != nullptr; }
    EncoderKind Kind() const { return kind_; }

    UINT Width()  const { return width_; }
    UINT Height() const { return height_; }

    // Handles queued events, collecting finished frames. Never blocks; false
    // if the encoder failed and should be recreated.
    bool Service(std::vector<EncodedFrame>& out);

    bool WantsInput() const { return inputsWanted_ > 0; }

    // Feeds one frame if the encoder wants input; false if not taken. Output
    // comes through Service(), or at once from the software encoder.
    // `released` runs exactly once: when the encoder lets go of the texture
    // (perhaps later, on another thread), or at once if not taken. The
    // software encoder maps the texture under Gfx::deviceMutex: never call
    // it with that held.
    using Released = std::function<void()>;
    bool Encode(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out, Released released = {});

    // For tests and benchmarks: waits for the encoder to want input, feeds
    // it, and waits until at least one frame comes out, up to `timeoutMs`.
    bool EncodeSync(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out, DWORD timeoutMs = 200);

    // The next encoded frame will be an IDR. Safe from any thread.
    void RequestKeyframe() { forceKeyframe_.store(true, std::memory_order_relaxed); }

    // Some encoders hold a frame back until the next arrives. From this tick
    // (GetTickCount64), unless kNoNudge, Repeat() the last picture to push it out.
    static constexpr uint64_t kNoNudge = ~0ull;
    uint64_t NudgeDueMs() const;
    bool Repeat(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out, Released released = {});

    const std::wstring& Name() const { return name_; }

private:
    bool TryInit(IMFActivate* activate);
    void FinishInFlight();
    bool WaitForEvents(DWORD timeoutMs);
    HRESULT CollectOutput(std::vector<EncodedFrame>& out);
    bool RenegotiateOutput();
    void ReadOutputStreamInfo();
    void ApplyKeyframeRequest();
    bool EncodeSoftware(ID3D11Texture2D* packed, std::vector<EncodedFrame>& out, Released released);
    void DrainSoftware(std::vector<EncodedFrame>& out);
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
    EncoderEventRelay* relay_ = nullptr;   // COM-refcounted; see codec.cpp. Hardware only.
    std::vector<winrt::com_ptr<IMFMediaEvent>> taken_;   // Service()'s batch, kept for its capacity.

    // Reused every frame while the encoder has let go of them.
    winrt::com_ptr<IMFSample>      inSample_, outSample_;
    winrt::com_ptr<IMFMediaBuffer> inBuffer_, outBuffer_;
    DWORD  outputSize_ = 0;      // The output stream's buffer size.
    HANDLE gpuDone_ = nullptr;   // Software: the frame's readback may begin.
    EncoderKind kind_ = EncoderKind::Hardware;
    HANDLE wake_ = nullptr;
    bool   failed_ = false;
    bool   drained_ = false;   // METransformDrainComplete arrived.
    DWORD inputId_ = 0, outputId_ = 0;
    bool  providesSamples_ = false;
    int   inputsWanted_ = 0;
    std::atomic<bool> forceKeyframe_{ false };
    UINT  width_ = 0, height_ = 0, fps_ = 60, bitrate_ = 0;
    ULONG gopSize_ = 0;   // Frames between keyframes the encoder accepted; 0 if none.
    UINT  qualityVsSpeed_ = kEncoderDefault;
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

    // Decoding in two steps: Stage copies an access unit in, which needs no
    // device lock; Decode feeds it to the decoder, which does.
    bool Stage(const uint8_t* data, size_t len);
    bool Decode(std::vector<DecodedFrame>& out);
    bool Decode(const uint8_t* data, size_t len, std::vector<DecodedFrame>& out) {
        return Stage(data, len) && Decode(out);
    }

private:
    bool NegotiateOutput();
    bool Drain(std::vector<DecodedFrame>& out);

    winrt::com_ptr<IMFTransform> mft_;
    winrt::com_ptr<IMFSample>      inSample_;   // Reused while the decoder has let go of it.
    winrt::com_ptr<IMFMediaBuffer> inBuffer_;
    bool  staged_ = false;
    DWORD inputId_ = 0, outputId_ = 0;
    bool  providesSamples_ = false;
    RECT  display_{};
    LONGLONG frameIndex_ = 0;
};

// Human-readable name of the hardware encoder in use, or empty if none.
std::wstring HardwareEncoderName();

// The H.264 encoder Windows provides on the CPU, or null if there is none
// (N editions without the Media Feature Pack).
winrt::com_ptr<IMFActivate> SoftwareEncoder();

// What streaming encodes with here: hardware needs the video processor too,
// else the CPU. `name` is empty if there is no encoder at all.
struct EncoderChoice {
    std::wstring name;
    EncoderKind  kind = EncoderKind::Hardware;
};
EncoderChoice ChooseEncoder();

}  // namespace rvm::net
