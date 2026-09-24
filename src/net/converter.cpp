#include "net/converter.h"

namespace rvm::net {

bool VideoConverter::Init() {
    auto& g = Gfx::Get();
    device_  = g.d3d.try_as<ID3D11VideoDevice>();
    context_ = g.ctx.try_as<ID3D11VideoContext>();
    return device_ && context_;
}

bool VideoConverter::Ensure(UINT inW, UINT inH, UINT outW, UINT outH) {
    if (processor_ && inW == inW_ && inH == inH_ && outW == outW_ && outH == outH_) return true;

    processor_ = nullptr;
    enumerator_ = nullptr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate   = { 60, 1 };
    desc.InputWidth       = inW;
    desc.InputHeight      = inH;
    desc.OutputFrameRate  = { 60, 1 };
    desc.OutputWidth      = outW;
    desc.OutputHeight     = outH;
    desc.Usage            = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(device_->CreateVideoProcessorEnumerator(&desc, enumerator_.put())) ||
        FAILED(device_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()))) {
        processor_ = nullptr;
        enumerator_ = nullptr;
        return false;
    }
    inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH;
    return true;
}

bool VideoConverter::EnsureScratch(DXGI_FORMAT format, UINT w, UINT h) {
    if (scratch_ && scratchFormat_ == format && scratchW_ == w && scratchH_ == h) return true;
    scratch_ = nullptr;

    D3D11_TEXTURE2D_DESC d{};
    d.Width      = w;
    d.Height     = h;
    d.MipLevels  = 1;
    d.ArraySize  = 1;
    d.Format     = format;
    d.SampleDesc = { 1, 0 };
    d.Usage      = D3D11_USAGE_DEFAULT;
    d.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const HRESULT hr = Gfx::Get().d3d->CreateTexture2D(&d, nullptr, scratch_.put());
    if (FAILED(hr)) {
        Log(L"converter: scratch %ux%u failed 0x%08X", w, h, static_cast<unsigned>(hr));
        return false;
    }
    scratchFormat_ = format;
    scratchW_ = w;
    scratchH_ = h;
    return true;
}

bool VideoConverter::Convert(ID3D11Texture2D* src, UINT srcSubresource, const RECT* srcRect,
                             ID3D11Texture2D* dst) {
    if (!device_ || !src || !dst) return false;

    D3D11_TEXTURE2D_DESC sd{}, dd{};
    src->GetDesc(&sd);
    dst->GetDesc(&dd);

    if (!(sd.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DECODER))) {
        const RECT whole{ 0, 0, static_cast<LONG>(sd.Width), static_cast<LONG>(sd.Height) };
        const RECT r = srcRect ? *srcRect : whole;
        if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
            r.right > whole.right || r.bottom > whole.bottom) {
            return false;
        }
        if (!EnsureScratch(sd.Format, static_cast<UINT>(RectW(r)), static_cast<UINT>(RectH(r)))) return false;
        const D3D11_BOX box{ static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0,
                             static_cast<UINT>(r.right), static_cast<UINT>(r.bottom), 1 };
        Gfx::Get().ctx->CopySubresourceRegion(scratch_.get(), 0, 0, 0, 0, src, srcSubresource, &box);
        src = scratch_.get();
        srcSubresource = 0;
        srcRect = nullptr;
        src->GetDesc(&sd);
    }

    if (!Ensure(sd.Width, sd.Height, dd.Width, dd.Height)) {
        RVM_LOG_SAMPLED(300, L"converter: no video processor for %ux%u -> %ux%u",
                        sd.Width, sd.Height, dd.Width, dd.Height);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = srcSubresource;
    winrt::com_ptr<ID3D11VideoProcessorInputView> input;
    HRESULT hr = device_->CreateVideoProcessorInputView(src, enumerator_.get(), &ivd, input.put());
    if (FAILED(hr)) {
        RVM_LOG_SAMPLED(300, L"converter: input view failed 0x%08X (bind 0x%X)",
                        static_cast<unsigned>(hr), sd.BindFlags);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> output;
    hr = device_->CreateVideoProcessorOutputView(dst, enumerator_.get(), &ovd, output.put());
    if (FAILED(hr)) {
        RVM_LOG_SAMPLED(300, L"converter: output view failed 0x%08X (bind 0x%X)",
                        static_cast<unsigned>(hr), dd.BindFlags);
        return false;
    }

    // BT.709, full-range RGB on the RGB side and studio-range YCbCr on the
    // other; the processor applies whichever direction the formats imply.
    const bool srcIsRgb = sd.Format != DXGI_FORMAT_NV12;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.RGB_Range     = 0;
    in.YCbCr_Matrix  = 1;
    in.Nominal_Range = srcIsRgb ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = in;
    out.Nominal_Range = srcIsRgb ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
                                 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    context_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &in);
    context_->VideoProcessorSetOutputColorSpace(processor_.get(), &out);

    const RECT full{ 0, 0, static_cast<LONG>(dd.Width), static_cast<LONG>(dd.Height) };
    context_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0,
                                                 D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    context_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, srcRect != nullptr, srcRect);
    context_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &full);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.get();
    hr = context_->VideoProcessorBlt(processor_.get(), output.get(), 0, 1, &stream);
    if (FAILED(hr)) RVM_LOG_SAMPLED(300, L"converter: blt failed 0x%08X", static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}

}  // namespace rvm::net
