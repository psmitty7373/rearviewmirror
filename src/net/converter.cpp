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

    inputs_.clear();    // Views belong to the enumerator going away.
    outputs_.clear();
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
    // A straight conversion: no driver denoising, edge enhancement or other
    // "improvements" the processor may otherwise apply on its own.
    context_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
    inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH;
    return true;
}

ID3D11VideoProcessorInputView* VideoConverter::InputView(ID3D11Texture2D* src, UINT slice) {
    for (size_t i = 0; i < inputs_.size(); ++i) {
        if (inputs_[i].texture == src && inputs_[i].slice == slice) {
            std::rotate(inputs_.begin() + static_cast<ptrdiff_t>(i),
                        inputs_.begin() + static_cast<ptrdiff_t>(i) + 1, inputs_.end());
            return inputs_.back().view.get();
        }
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = slice;
    winrt::com_ptr<ID3D11VideoProcessorInputView> view;
    const HRESULT hr = device_->CreateVideoProcessorInputView(src, enumerator_.get(), &ivd, view.put());
    if (FAILED(hr)) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        RVM_LOG_SAMPLED(300, L"converter: input view failed 0x%08X (bind 0x%X)",
                        static_cast<unsigned>(hr), sd.BindFlags);
        Gfx::Get().CheckDevice(hr);
        return nullptr;
    }
    if (inputs_.size() >= kMaxViews) inputs_.erase(inputs_.begin());
    inputs_.push_back({ src, slice, std::move(view) });
    return inputs_.back().view.get();
}

ID3D11VideoProcessorOutputView* VideoConverter::OutputView(ID3D11Texture2D* dst) {
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (outputs_[i].texture == dst) {
            std::rotate(outputs_.begin() + static_cast<ptrdiff_t>(i),
                        outputs_.begin() + static_cast<ptrdiff_t>(i) + 1, outputs_.end());
            return outputs_.back().view.get();
        }
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> view;
    const HRESULT hr = device_->CreateVideoProcessorOutputView(dst, enumerator_.get(), &ovd, view.put());
    if (FAILED(hr)) {
        D3D11_TEXTURE2D_DESC dd{};
        dst->GetDesc(&dd);
        RVM_LOG_SAMPLED(300, L"converter: output view failed 0x%08X (bind 0x%X)",
                        static_cast<unsigned>(hr), dd.BindFlags);
        Gfx::Get().CheckDevice(hr);
        return nullptr;
    }
    if (outputs_.size() >= kMaxViews) outputs_.erase(outputs_.begin());
    outputs_.push_back({ dst, std::move(view) });
    return outputs_.back().view.get();
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

    ID3D11VideoProcessorInputView* input = InputView(src, srcSubresource);
    ID3D11VideoProcessorOutputView* output = input ? OutputView(dst) : nullptr;
    if (!output) return false;

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
    stream.pInputSurface = input;
    const HRESULT hr = context_->VideoProcessorBlt(processor_.get(), output, 0, 1, &stream);
    if (FAILED(hr)) {
        RVM_LOG_SAMPLED(300, L"converter: blt failed 0x%08X", static_cast<unsigned>(hr));
        Gfx::Get().CheckDevice(hr);
    }
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Nv12Packer

namespace {

// Two passes over one R8 target. The luma pass fills the top `height` rows.
// The chroma pass fills the `height / 2` rows beneath: row y holds frame rows
// 2y and 2y+1 as Cb, Cr pairs, even columns Cb and odd ones Cr, each pair for
// the 2x2 block it covers. Sampling at a block's centre averages the block.
constexpr char kPackSource[] = R"HLSL(
cbuffer Constants : register(b0)
{
    float4 uvRect;   // Source rectangle: u0, v0, u1, v1
    float4 frame;    // Frame width, height in pixels
};

Texture2D    srcTex : register(t0);
SamplerState srcSmp : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);   // BT.709

float4 VSMain(uint vid : SV_VertexID) : SV_POSITION
{
    float2 t = float2((vid << 1) & 2, vid & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

// The source colour at a point in frame pixels.
float3 Rgb(float2 at)
{
    return srcTex.SampleLevel(srcSmp, lerp(uvRect.xy, uvRect.zw, at / frame.xy), 0).rgb;
}

float LumaMain(float4 pos : SV_POSITION) : SV_TARGET
{
    return (16.0 + 219.0 * dot(Rgb(pos.xy), kLuma)) / 255.0;
}

float ChromaMain(float4 pos : SV_POSITION) : SV_TARGET
{
    float2 block = floor(float2(pos.x * 0.5, pos.y - frame.y));
    float3 rgb = Rgb(block * 2.0 + 1.0);
    float y = dot(rgb, kLuma);
    float c = fmod(floor(pos.x), 2.0) < 0.5 ? (rgb.b - y) / 1.8556 : (rgb.r - y) / 1.5748;
    return (128.0 + 224.0 * c) / 255.0;
}
)HLSL";

struct PackPipeline {
    winrt::com_ptr<ID3D11VertexShader>    vs;
    winrt::com_ptr<ID3D11PixelShader>     luma, chroma;
    winrt::com_ptr<ID3D11SamplerState>    sampler;
    winrt::com_ptr<ID3D11Buffer>          constants;   // Shared: used only under the device lock.
    winrt::com_ptr<ID3D11RasterizerState> raster;
    bool ok = false;
};

struct PackConstants {
    float uvRect[4];
    float frame[4];
};

winrt::com_ptr<ID3DBlob> CompilePackStage(const char* entry, const char* target) {
    winrt::com_ptr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kPackSource, sizeof(kPackSource) - 1, "rvm-pack", nullptr, nullptr,
                                  entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                  code.put(), errors.put());
    if (FAILED(hr)) {
        if (errors) Log(L"converter: %S failed to compile: %S", entry,
                        static_cast<const char*>(errors->GetBufferPointer()));
        return nullptr;
    }
    return code;
}

const PackPipeline& GetPackPipeline() {
    static PackPipeline pipeline;
    static std::once_flag once;
    std::call_once(once, [] {
        auto& g = Gfx::Get();
        auto vsCode = CompilePackStage("VSMain", "vs_4_0");
        auto lumaCode = CompilePackStage("LumaMain", "ps_4_0");
        auto chromaCode = CompilePackStage("ChromaMain", "ps_4_0");
        if (!vsCode || !lumaCode || !chromaCode) return;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(PackConstants);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;

        pipeline.ok =
            SUCCEEDED(g.d3d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                                nullptr, pipeline.vs.put())) &&
            SUCCEEDED(g.d3d->CreatePixelShader(lumaCode->GetBufferPointer(), lumaCode->GetBufferSize(),
                                               nullptr, pipeline.luma.put())) &&
            SUCCEEDED(g.d3d->CreatePixelShader(chromaCode->GetBufferPointer(), chromaCode->GetBufferSize(),
                                               nullptr, pipeline.chroma.put())) &&
            SUCCEEDED(g.d3d->CreateSamplerState(&sd, pipeline.sampler.put())) &&
            SUCCEEDED(g.d3d->CreateBuffer(&bd, nullptr, pipeline.constants.put())) &&
            SUCCEEDED(g.d3d->CreateRasterizerState(&rd, pipeline.raster.put()));
    });
    return pipeline;
}

}  // namespace

bool Nv12Packer::Init() {
    return GetPackPipeline().ok;
}

bool Nv12Packer::EnsureTarget(UINT width, UINT rows) {
    if (target_ && targetW_ == width && targetRows_ == rows) return true;
    rtv_ = nullptr;
    target_ = nullptr;

    D3D11_TEXTURE2D_DESC d{};
    d.Width      = width;
    d.Height     = rows;
    d.MipLevels  = 1;
    d.ArraySize  = 1;
    d.Format     = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc = { 1, 0 };
    d.Usage      = D3D11_USAGE_DEFAULT;
    d.BindFlags  = D3D11_BIND_RENDER_TARGET;
    auto& g = Gfx::Get();
    HRESULT hr = g.d3d->CreateTexture2D(&d, nullptr, target_.put());
    if (SUCCEEDED(hr)) hr = g.d3d->CreateRenderTargetView(target_.get(), nullptr, rtv_.put());
    if (FAILED(hr)) {   // Half-made must not pass for made next time.
        Log(L"converter: packing target %ux%u failed 0x%08X", width, rows, static_cast<unsigned>(hr));
        g.CheckDevice(hr);
        rtv_ = nullptr;
        target_ = nullptr;
        return false;
    }
    targetW_ = width;
    targetRows_ = rows;
    return true;
}

ID3D11ShaderResourceView* Nv12Packer::SourceView(ID3D11Texture2D* src) {
    if (srcTex_.get() == src && srcSrv_) return srcSrv_.get();
    srcSrv_ = nullptr;
    srcTex_ = nullptr;
    const HRESULT hr = Gfx::Get().d3d->CreateShaderResourceView(src, nullptr, srcSrv_.put());
    if (FAILED(hr)) {
        RVM_LOG_SAMPLED(300, L"converter: source view failed 0x%08X", static_cast<unsigned>(hr));
        Gfx::Get().CheckDevice(hr);
        return nullptr;
    }
    srcTex_.copy_from(src);
    return srcSrv_.get();
}

bool Nv12Packer::Pack(ID3D11Texture2D* src, const RECT& srcRect, ID3D11Texture2D* staging) {
    const auto& pipeline = GetPackPipeline();
    if (!pipeline.ok || !src || !staging) return false;

    D3D11_TEXTURE2D_DESC sd{}, dd{};
    src->GetDesc(&sd);
    staging->GetDesc(&dd);
    const UINT width = dd.Width, rows = dd.Height, height = rows / 3 * 2;
    if (dd.Format != DXGI_FORMAT_R8_UNORM || rows % 3 != 0 || height == 0 || (height & 1) ||
        (width & 1) || srcRect.left < 0 || srcRect.top < 0 || RectW(srcRect) <= 0 ||
        RectH(srcRect) <= 0 || static_cast<UINT>(srcRect.right) > sd.Width ||
        static_cast<UINT>(srcRect.bottom) > sd.Height) {
        return false;
    }
    if (!EnsureTarget(width, rows)) return false;
    ID3D11ShaderResourceView* srv = SourceView(src);
    if (!srv) return false;

    auto& g = Gfx::Get();
    PackConstants constants{};
    constants.uvRect[0] = srcRect.left   / static_cast<float>(sd.Width);
    constants.uvRect[1] = srcRect.top    / static_cast<float>(sd.Height);
    constants.uvRect[2] = srcRect.right  / static_cast<float>(sd.Width);
    constants.uvRect[3] = srcRect.bottom / static_cast<float>(sd.Height);
    constants.frame[0] = static_cast<float>(width);
    constants.frame[1] = static_cast<float>(height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = g.ctx->Map(pipeline.constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        g.CheckDevice(hr);
        return false;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    g.ctx->Unmap(pipeline.constants.get(), 0);

    // Every piece of state the draws depend on is set here, as the renderer
    // does: whatever drew last on the shared context left its own behind.
    ID3D11RenderTargetView* targets[]{ rtv_.get() };
    g.ctx->OMSetRenderTargets(1, targets, nullptr);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g.ctx->RSSetState(pipeline.raster.get());
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(pipeline.vs.get(), nullptr, 0);
    ID3D11Buffer* cbs[]{ pipeline.constants.get() };
    g.ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[]{ srv };
    ID3D11SamplerState* samplers[]{ pipeline.sampler.get() };
    g.ctx->PSSetShaderResources(0, 1, srvs);
    g.ctx->PSSetSamplers(0, 1, samplers);

    const D3D11_VIEWPORT lumaArea{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                                   0.0f, 1.0f };
    g.ctx->RSSetViewports(1, &lumaArea);
    g.ctx->PSSetShader(pipeline.luma.get(), nullptr, 0);
    g.ctx->Draw(3, 0);

    const D3D11_VIEWPORT chromaArea{ 0.0f, static_cast<float>(height), static_cast<float>(width),
                                     static_cast<float>(height / 2), 0.0f, 1.0f };
    g.ctx->RSSetViewports(1, &chromaArea);
    g.ctx->PSSetShader(pipeline.chroma.get(), nullptr, 0);
    g.ctx->Draw(3, 0);

    // Unbind, so the source can be written again by the next frame.
    ID3D11ShaderResourceView* none[]{ nullptr };
    g.ctx->PSSetShaderResources(0, 1, none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    g.ctx->CopyResource(staging, target_.get());
    return true;
}

}  // namespace rvm::net
