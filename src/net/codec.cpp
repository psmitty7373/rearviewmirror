#include "net/codec.h"

#include <codecapi.h>
#include <deque>
#include <mferror.h>

namespace rvm::net {

namespace {

constexpr LONGLONG kHns = 10000000;   // 100-ns units per second.

struct MfLifetime {
    MfLifetime()  { MFStartup(MF_VERSION, MFSTARTUP_LITE); }
    ~MfLifetime() { MFShutdown(); }
};

void EnsureMf() {
    static MfLifetime lifetime;
}

// One DXGI device manager for the process, wrapping the shared device.
IMFDXGIDeviceManager* DeviceManager() {
    static winrt::com_ptr<IMFDXGIDeviceManager> manager = [] {
        winrt::com_ptr<IMFDXGIDeviceManager> m;
        UINT token = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, m.put()))) {
            m->ResetDevice(Gfx::Get().d3d.get(), token);
        }
        return m;
    }();
    return manager.get();
}

winrt::com_ptr<IMFActivate> FindTransform(GUID category, UINT32 flags,
                                          GUID inSubtype, GUID outSubtype) {
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, inSubtype };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, outSubtype };
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    winrt::com_ptr<IMFActivate> result;
    if (SUCCEEDED(MFTEnumEx(category, flags, &in, &out, &list, &count)) && count > 0) {
        result.copy_from(list[0]);
        for (UINT32 i = 0; i < count; ++i) list[i]->Release();
        CoTaskMemFree(list);
    }
    return result;
}

LONGLONG FrameDuration(UINT fps) {
    return kHns / (std::max)(fps, 1u);
}

std::wstring FriendlyName(IMFActivate* activate) {
    wchar_t* name = nullptr;
    UINT32 len = 0;
    std::wstring out;
    if (activate && SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
        out.assign(name, len);
        CoTaskMemFree(name);
    }
    return out;
}

bool DeviceAdapterLuid(LUID& luid) {
    auto& dxgi = Gfx::Get().dxgi;
    if (!dxgi) return false;
    winrt::com_ptr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(dxgi->GetAdapter(adapter.put())) || FAILED(adapter->GetDesc(&desc))) return false;
    luid = desc.AdapterLuid;
    return true;
}

// True if the Annex B stream carries a sequence parameter set (NAL type 7).
bool HasSps(const std::vector<uint8_t>& d) {
    for (size_t i = 0; i + 3 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1F) == 7) return true;
    }
    return false;
}

constexpr int      kMaxNudges = 8;
constexpr uint64_t kNudgeAfterMs = 40;

}  // namespace

// Receives an asynchronous encoder's events on a Media Foundation thread,
// queues them, and signals. It is reference counted like any COM object: the
// encoder holds one reference and a pending BeginGetEvent holds another, so it
// outlives whichever lets go last. Stop() drops its hold on the encoder, so no
// cycle survives a shutdown, and every handle it signals is its own.
class EncoderEventRelay final : public IMFAsyncCallback {
public:
    EncoderEventRelay(IMFMediaEventGenerator* generator, HANDLE wake) {
        generator_.copy_from(generator);
        own_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake) {
            DuplicateHandle(GetCurrentProcess(), wake, GetCurrentProcess(), &wake_, 0, FALSE,
                            DUPLICATE_SAME_ACCESS);
        }
    }

    bool Start() {
        std::lock_guard lock(mutex_);
        return generator_ && SUCCEEDED(generator_->BeginGetEvent(this, nullptr));
    }

    void Stop() {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        generator_ = nullptr;
        queue_.clear();
    }

    std::vector<winrt::com_ptr<IMFMediaEvent>> Take() {
        std::lock_guard lock(mutex_);
        std::vector<winrt::com_ptr<IMFMediaEvent>> out(std::make_move_iterator(queue_.begin()),
                                                        std::make_move_iterator(queue_.end()));
        queue_.clear();
        return out;
    }

    bool Failed() const {
        std::lock_guard lock(mutex_);
        return failed_;
    }

    HANDLE Own() const { return own_; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *out = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // IMFAsyncCallback
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        winrt::com_ptr<IMFMediaEventGenerator> generator;
        {
            std::lock_guard lock(mutex_);
            generator = generator_;
        }
        if (!generator) return S_OK;   // Stopped: the encoder is going away.

        winrt::com_ptr<IMFMediaEvent> event;
        const HRESULT hr = generator->EndGetEvent(result, event.put());
        bool rearm = false;
        {
            std::lock_guard lock(mutex_);
            if (stopped_) {
                generator_ = nullptr;
            } else if (FAILED(hr)) {
                failed_ = true;
                generator_ = nullptr;
            } else {
                queue_.push_back(std::move(event));
                rearm = true;
            }
        }
        if (rearm && FAILED(generator->BeginGetEvent(this, nullptr))) {
            std::lock_guard lock(mutex_);
            failed_ = !stopped_;
            generator_ = nullptr;
        }
        SetEvent(own_);
        if (wake_) SetEvent(wake_);
        return S_OK;
    }

private:
    ~EncoderEventRelay() {
        if (own_) CloseHandle(own_);
        if (wake_) CloseHandle(wake_);
    }

    std::atomic<ULONG> refs_{ 1 };
    mutable std::mutex mutex_;
    winrt::com_ptr<IMFMediaEventGenerator> generator_;
    std::deque<winrt::com_ptr<IMFMediaEvent>> queue_;
    bool stopped_ = false;
    bool failed_ = false;
    HANDLE own_ = nullptr;
    HANDLE wake_ = nullptr;
};

std::vector<winrt::com_ptr<IMFActivate>> HardwareEncoders() {
    EnsureMf();
    std::vector<winrt::com_ptr<IMFActivate>> out;
    std::vector<GUID> seen;
    const auto append = [&](IMFActivate** list, UINT32 count) {
        for (UINT32 i = 0; i < count; ++i) {
            GUID clsid{};
            const bool known = SUCCEEDED(list[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid));
            if (!known || std::find(seen.begin(), seen.end(), clsid) == seen.end()) {
                if (known) seen.push_back(clsid);
                winrt::com_ptr<IMFActivate> a;
                a.copy_from(list[i]);
                out.push_back(std::move(a));
            }
            list[i]->Release();
        }
        CoTaskMemFree(list);
    };

    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType{ MFMediaType_Video, MFVideoFormat_H264 };
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

    LUID luid{};
    winrt::com_ptr<IMFAttributes> attrs;
    if (DeviceAdapterLuid(luid) && SUCCEEDED(MFCreateAttributes(attrs.put(), 1)) &&
        SUCCEEDED(attrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid),
                                 sizeof(luid)))) {
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outType, attrs.get(),
                               &list, &count))) {
            append(list, count);
        }
    }
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outType, &list, &count))) {
        append(list, count);
    }
    return out;
}

std::wstring HardwareEncoderName() {
    const auto encoders = HardwareEncoders();
    return encoders.empty() ? std::wstring() : FriendlyName(encoders.front().get());
}

// ---------------------------------------------------------------------------
// Encoder

H264Encoder::~H264Encoder() {
    Shutdown();
}

void H264Encoder::Shutdown() {
    if (relay_) relay_->Stop();   // Before the MFT goes: no event may reach us now.
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        // Completes the pending BeginGetEvent, so the relay is released.
        if (auto shutdown = mft_.try_as<IMFShutdown>()) shutdown->Shutdown();
    }
    if (relay_) {
        relay_->Release();
        relay_ = nullptr;
    }
    failed_ = false;
    codec_ = nullptr;
    events_ = nullptr;
    mft_ = nullptr;
    inputsWanted_ = 0;
    frameIndex_ = 0;
    awaitingOutput_ = false;
    repeats_ = 0;
    errorsLogged_ = 0;
    inputsTraced_ = 0;
    outputsTraced_ = 0;
    sequenceHeader_.clear();
}

// Tries each hardware encoder in turn, the one on our own GPU first, and
// logs exactly where any of them refused.
bool H264Encoder::Init(UINT width, UINT height, UINT fps, UINT bitrateBps) {
    EnsureMf();
    Shutdown();
    width_ = width & ~1u;
    height_ = height & ~1u;
    fps_ = (std::max)(fps, 1u);
    bitrate_ = bitrateBps;
    if (width_ == 0 || height_ == 0) return false;

    const auto encoders = HardwareEncoders();
    if (encoders.empty()) Log(L"encoder: no hardware H.264 encoder found");
    for (const auto& activate : encoders) {
        name_ = FriendlyName(activate.get());
        if (TryInit(activate.get())) {
            // Hardware encoders ask for their first input a moment after
            // starting; wait for that so the caller's first frame is taken.
            std::vector<EncodedFrame> none;
            const ULONGLONG deadline = GetTickCount64() + 200;
            while (!WantsInput() && Service(none) && GetTickCount64() < deadline) {
                WaitForEvents(static_cast<DWORD>(deadline - GetTickCount64()));
            }
            Log(L"encoder: '%s' ready at %ux%u, %u fps, %u kbps%s", name_.c_str(), width_, height_,
                fps_, bitrate_ / 1000, WantsInput() ? L"" : L" (no input request yet)");
            return true;
        }
        Shutdown();
        activate->ShutdownObject();
    }
    name_.clear();
    return false;
}

bool H264Encoder::TryInit(IMFActivate* activate) {
    const auto fail = [&](const wchar_t* step, HRESULT hr) {
        Log(L"encoder: '%s' %ux%u failed at %s (0x%08X)", name_.c_str(), width_, height_, step,
            static_cast<unsigned>(hr));
        return false;
    };

    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(mft_.put()));
    if (FAILED(hr)) return fail(L"activation", hr);

    // Hardware encoders are asynchronous MFTs and must be unlocked explicitly.
    winrt::com_ptr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(attrs.put()))) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    events_ = mft_.try_as<IMFMediaEventGenerator>();
    if (!events_) return fail(L"event generator", E_NOINTERFACE);

    IMFDXGIDeviceManager* manager = DeviceManager();
    if (!manager) return fail(L"device manager", E_FAIL);
    hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager));
    if (FAILED(hr)) return fail(L"SET_D3D_MANAGER (encoder on another GPU?)", hr);

    DWORD inCount = 0, outCount = 0;
    mft_->GetStreamCount(&inCount, &outCount);
    if (FAILED(mft_->GetStreamIDs(1, &inputId_, 1, &outputId_))) {
        inputId_ = 0;
        outputId_ = 0;
    }

    codec_ = mft_.try_as<ICodecAPI>();
    if (codec_) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = eAVEncCommonRateControlMode_CBR;
        codec_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.ulVal = bitrate_;
        codec_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        v.ulVal = 0;
        codec_->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        v.ulVal = fps_ * 4;
        codec_->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codec_->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    // Output type first: hardware encoders derive the input they accept from it.
    winrt::com_ptr<IMFMediaType> outType;
    MFCreateMediaType(outType.put());
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate_);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = mft_->SetOutputType(outputId_, outType.get(), 0);
    if (FAILED(hr)) return fail(L"SetOutputType", hr);

    winrt::com_ptr<IMFMediaType> inType;
    MFCreateMediaType(inType.put());
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(inType.get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(inType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = mft_->SetInputType(inputId_, inType.get(), 0);
    if (FAILED(hr)) return fail(L"SetInputType", hr);

    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(outputId_, &info);
    providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return fail(L"BEGIN_STREAMING", hr);
    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return fail(L"START_OF_STREAM", hr);

    // Events queue inside the encoder until asked for, so starting to listen
    // after START_OF_STREAM loses nothing.
    relay_ = new EncoderEventRelay(events_.get(), wake_);
    if (!relay_->Start()) return fail(L"BeginGetEvent", E_FAIL);
    return true;
}

// The encoder changed its output format mid-stream, which Intel's does on its
// first frame to fill in the sequence headers. Accept whatever it now offers.
bool H264Encoder::RenegotiateOutput() {
    winrt::com_ptr<IMFMediaType> type;
    HRESULT hr = mft_->GetOutputAvailableType(outputId_, 0, type.put());
    if (SUCCEEDED(hr)) hr = mft_->SetOutputType(outputId_, type.get(), 0);
    Log(L"encoder: '%s' changed its output format -> %s (0x%08X)", name_.c_str(),
        SUCCEEDED(hr) ? L"renegotiated" : L"FAILED", static_cast<unsigned>(hr));
    if (FAILED(hr)) return false;
    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(outputId_, &info);
    providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    UINT32 size = 0;
    if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0 && size < 4096) {
        sequenceHeader_.resize(size);
        if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), size, nullptr))) {
            sequenceHeader_.clear();
        }
        Log(L"encoder: '%s' output format carries a %u-byte sequence header", name_.c_str(), size);
    }
    return true;
}

// Every error, up to a cap per encoder: sampling hid the pattern once.
void H264Encoder::LogError(const wchar_t* step, HRESULT hr) {
    if (errorsLogged_ >= kMaxErrorLogs) return;
    if (++errorsLogged_ == kMaxErrorLogs) {
        Log(L"encoder: '%s' %s failed (0x%08X); further errors not logged", name_.c_str(), step,
            static_cast<unsigned>(hr));
    } else {
        Log(L"encoder: '%s' %s failed (0x%08X)", name_.c_str(), step, static_cast<unsigned>(hr));
    }
}

bool H264Encoder::NeedsNudge(uint64_t nowMs) const {
    return mft_ && awaitingOutput_ && repeats_ < kMaxNudges && nowMs - lastFedMs_ >= kNudgeAfterMs;
}

bool H264Encoder::Repeat(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out) {
    ++repeats_;
    const bool wasAwaiting = awaitingOutput_;
    const size_t before = out.size();
    const bool fed = Encode(nv12, out);
    // A repeat is filler: it must not itself be waited on once the real
    // frame it was pushing out has appeared.
    if (out.size() == before) awaitingOutput_ = wasAwaiting;
    RVM_LOG_SAMPLED(50, L"encoder: '%s' held its frame; repeat %d%s", name_.c_str(), repeats_,
                    fed ? L"" : L" (not taken)");
    return fed;
}

bool H264Encoder::WaitForEvents(DWORD timeoutMs) {
    return relay_ && WaitForSingleObject(relay_->Own(), timeoutMs) == WAIT_OBJECT_0;
}

bool H264Encoder::Service(std::vector<EncodedFrame>& out) {
    if (!mft_ || !relay_) return false;
    for (auto& event : relay_->Take()) {
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput) {
            ++inputsWanted_;
        } else if (type == METransformHaveOutput) {
            CollectOutput(out);
        } else if (type == MEError) {
            HRESULT status = S_OK;
            event->GetStatus(&status);
            LogError(L"MEError event", status);
            failed_ = true;
        }
    }
    if (relay_->Failed() && !failed_) {
        LogError(L"event queue", E_FAIL);
        failed_ = true;
    }
    return !failed_;
}

bool H264Encoder::EncodeSync(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    const auto remaining = [&] {
        const ULONGLONG now = GetTickCount64();
        return static_cast<DWORD>(now >= deadline ? 0 : deadline - now);
    };
    while (!WantsInput()) {
        if (!Service(out)) return false;
        if (WantsInput()) break;
        if (remaining() == 0) return false;
        WaitForEvents(remaining());
    }
    const size_t before = out.size();
    if (!Encode(nv12, out)) return false;
    while (out.size() == before && remaining() > 0) {
        WaitForEvents(remaining());
        if (!Service(out)) return false;
    }
    return true;
}

bool H264Encoder::CollectOutput(std::vector<EncodedFrame>& out) {
    MFT_OUTPUT_DATA_BUFFER db{};
    db.dwStreamID = outputId_;
    winrt::com_ptr<IMFSample> sample;
    if (!providesSamples_) {
        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(outputId_, &info);
        winrt::com_ptr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateSample(sample.put())) ||
            FAILED(MFCreateMemoryBuffer((std::max)(static_cast<UINT>(info.cbSize), 1u << 20),
                                        buffer.put()))) {
            return false;
        }
        sample->AddBuffer(buffer.get());
        db.pSample = sample.get();
    }

    DWORD status = 0;
    const HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
    if (db.pEvents) db.pEvents->Release();
    if (FAILED(hr) && providesSamples_ && db.pSample) db.pSample->Release();

    // An asynchronous encoder that changes its output format sends a fresh
    // METransformHaveOutput once the new type is set; asking again before
    // that event is refused with E_UNEXPECTED.
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        RenegotiateOutput();
        return false;
    }
    if (FAILED(hr)) {
        if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) LogError(L"ProcessOutput", hr);
        return false;
    }

    winrt::com_ptr<IMFSample> produced;
    if (providesSamples_) produced.attach(db.pSample);
    else                  produced = sample;
    if (!produced) return false;

    winrt::com_ptr<IMFMediaBuffer> contiguous;
    if (FAILED(produced->ConvertToContiguousBuffer(contiguous.put()))) return false;

    BYTE* bytes = nullptr;
    DWORD length = 0;
    if (FAILED(contiguous->Lock(&bytes, nullptr, &length))) return false;

    EncodedFrame frame;
    frame.data.assign(bytes, bytes + length);
    frame.keyframe = MFGetAttributeUINT32(produced.get(), MFSampleExtension_CleanPoint, 0) != 0;
    contiguous->Unlock();
    if (frame.data.empty()) return false;

    // A decoder cannot start without the sequence headers. An encoder that
    // moved them into its output format is made to carry them in-band again.
    if (frame.keyframe && !sequenceHeader_.empty() && !HasSps(frame.data)) {
        frame.data.insert(frame.data.begin(), sequenceHeader_.begin(), sequenceHeader_.end());
        if (outputsTraced_ < kTraceFrames) {
            Log(L"encoder: '%s' keyframe had no SPS; prepended %zu header bytes", name_.c_str(),
                sequenceHeader_.size());
        }
    }
    if (outputsTraced_ < kTraceFrames) {
        ++outputsTraced_;
        // The first bytes show whether sequence headers (NAL type 7, 8) are
        // in the bitstream: a decoder cannot start without them.
        wchar_t head[64]{};
        for (size_t i = 0; i < (std::min)(frame.data.size(), static_cast<size_t>(12)); ++i) {
            swprintf_s(head + i * 3, 4, L"%02X ", frame.data[i]);
        }
        Log(L"encoder: '%s' output %zu bytes%s: %s", name_.c_str(), frame.data.size(),
            frame.keyframe ? L" (keyframe)" : L"", head);
    }
    out.push_back(std::move(frame));
    awaitingOutput_ = false;
    repeats_ = 0;
    return true;
}

bool H264Encoder::Encode(ID3D11Texture2D* nv12, std::vector<EncodedFrame>& out) {
    if (!mft_ || !nv12 || failed_) return false;
    if (!Service(out) || inputsWanted_ <= 0) return false;

    if (forceKeyframe_.exchange(false, std::memory_order_relaxed) && codec_) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        codec_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    }

    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::com_ptr<IMFSample> sample;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, buffer.put())) ||
        FAILED(MFCreateSample(sample.put()))) {
        return false;
    }
    sample->AddBuffer(buffer.get());
    const LONGLONG duration = FrameDuration(fps_);
    sample->SetSampleTime(frameIndex_ * duration);
    sample->SetSampleDuration(duration);
    ++frameIndex_;

    const HRESULT hr = mft_->ProcessInput(inputId_, sample.get(), 0);
    if (FAILED(hr)) {
        LogError(L"ProcessInput", hr);
        return false;
    }
    --inputsWanted_;
    awaitingOutput_ = true;
    lastFedMs_ = GetTickCount64();
    if (inputsTraced_ < kTraceFrames) {
        ++inputsTraced_;
        Log(L"encoder: '%s' input %lld accepted", name_.c_str(), frameIndex_ - 1);
    }
    Service(out);   // Anything the encoder had already finished.
    return true;
}

// ---------------------------------------------------------------------------
// Decoder

H264Decoder::~H264Decoder() {
    Shutdown();
}

void H264Decoder::Shutdown() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    mft_ = nullptr;
    width_ = height_ = 0;
    frameIndex_ = 0;
}

bool H264Decoder::Init() {
    EnsureMf();
    Shutdown();

    auto activate = FindTransform(MFT_CATEGORY_VIDEO_DECODER,
                                  MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                  MFVideoFormat_H264, MFVideoFormat_NV12);
    if (!activate || FAILED(activate->ActivateObject(IID_PPV_ARGS(mft_.put())))) return false;

    winrt::com_ptr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(attrs.put()))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    }
    if (auto codec = mft_.try_as<ICodecAPI>()) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    // DXVA: decoded frames come back as textures on our device.
    IMFDXGIDeviceManager* manager = DeviceManager();
    if (!manager || FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                reinterpret_cast<ULONG_PTR>(manager)))) {
        Shutdown();
        return false;
    }

    if (FAILED(mft_->GetStreamIDs(1, &inputId_, 1, &outputId_))) {
        inputId_ = 0;
        outputId_ = 0;
    }

    winrt::com_ptr<IMFMediaType> inType;
    MFCreateMediaType(inType.put());
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(mft_->SetInputType(inputId_, inType.get(), 0)) || !NegotiateOutput()) {
        Shutdown();
        return false;
    }

    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
        Shutdown();
        return false;
    }
    return true;
}

bool H264Decoder::NegotiateOutput() {
    for (DWORD i = 0;; ++i) {
        winrt::com_ptr<IMFMediaType> type;
        if (FAILED(mft_->GetOutputAvailableType(outputId_, i, type.put()))) return false;
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype != MFVideoFormat_NV12) continue;
        if (FAILED(mft_->SetOutputType(outputId_, type.get(), 0))) return false;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &w, &h);
        display_ = RECT{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };

        // The coded size is padded; the aperture is the picture the encoder
        // was actually given.
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                    sizeof(area), nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            display_.left   = area.OffsetX.value;
            display_.top    = area.OffsetY.value;
            display_.right  = display_.left + area.Area.cx;
            display_.bottom = display_.top + area.Area.cy;
        }
        width_  = static_cast<UINT>(RectW(display_));
        height_ = static_cast<UINT>(RectH(display_));

        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(outputId_, &info);
        providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        return true;
    }
}

bool H264Decoder::Drain(std::vector<DecodedFrame>& out) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER db{};
        db.dwStreamID = outputId_;

        winrt::com_ptr<IMFSample> own;
        if (!providesSamples_) {
            // Without DXVA the decoder wants a buffer from us; keep it simple
            // and let it allocate by asking for provided samples anyway.
            return false;
        }

        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
        if (db.pEvents) db.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (db.pSample) db.pSample->Release();
            if (!NegotiateOutput()) return false;
            continue;
        }
        if (FAILED(hr)) {
            if (db.pSample) db.pSample->Release();
            return false;
        }

        winrt::com_ptr<IMFSample> sample;
        sample.attach(db.pSample);
        if (!sample) continue;

        winrt::com_ptr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, buffer.put()))) continue;
        auto dxgi = buffer.try_as<IMFDXGIBuffer>();
        if (!dxgi) continue;

        DecodedFrame frame;
        if (FAILED(dxgi->GetResource(IID_PPV_ARGS(frame.texture.put())))) continue;
        dxgi->GetSubresourceIndex(&frame.subresource);
        frame.display = display_;
        out.push_back(std::move(frame));
    }
}

bool H264Decoder::Decode(const uint8_t* data, size_t len, std::vector<DecodedFrame>& out) {
    if (!mft_ || !data || len == 0) return false;

    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::com_ptr<IMFSample> sample;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(len), buffer.put())) ||
        FAILED(MFCreateSample(sample.put()))) {
        return false;
    }
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return false;
    memcpy(dst, data, len);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(len));
    sample->AddBuffer(buffer.get());

    const LONGLONG duration = FrameDuration(60);
    sample->SetSampleTime(frameIndex_ * duration);
    sample->SetSampleDuration(duration);
    ++frameIndex_;

    HRESULT hr = mft_->ProcessInput(inputId_, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!Drain(out)) return false;
        hr = mft_->ProcessInput(inputId_, sample.get(), 0);
    }
    if (FAILED(hr)) return false;
    return Drain(out);
}

}  // namespace rvm::net
