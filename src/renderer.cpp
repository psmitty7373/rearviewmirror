#include "renderer.h"

namespace rvm {

namespace {

constexpr float kCornerRadius = 6.0f;

// Fullscreen triangle. The pixel shader applies opacity, an antialiased
// rounded-rect mask and a hover border, premultiplied for DirectComposition.
constexpr char kShaderSource[] = R"HLSL(
cbuffer Constants : register(b0)
{
    float4 uvRect;   // u0, v0, u1, v1
    float4 params;   // opacity, cornerRadius, widthPx, heightPx
    float4 border;   // r, g, b, intensity
};

Texture2D    srcTex : register(t0);
SamplerState srcSmp : register(s0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 t = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    o.uv  = lerp(uvRect.xy, uvRect.zw, t);
    return o;
}

float RoundedBoxSDF(float2 p, float2 halfSize, float r)
{
    float2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    float3 color = srcTex.Sample(srcSmp, input.uv).rgb;

    float2 size = params.zw;
    float  d    = RoundedBoxSDF(input.pos.xy - size * 0.5, size * 0.5, params.y);

    float alpha = saturate(0.5 - d) * params.x;

    float bw   = 1.5;
    float edge = saturate(1.0 - abs(d + bw * 0.5) / (bw * 0.5)) * border.w;
    color = lerp(color, border.rgb, saturate(edge));

    return float4(color * alpha, alpha);
}
)HLSL";

struct Pipeline {
    winrt::com_ptr<ID3D11VertexShader>    vs;
    winrt::com_ptr<ID3D11PixelShader>     ps;
    winrt::com_ptr<ID3D11SamplerState>    sampler;
    winrt::com_ptr<ID3D11Buffer>          constants;
    winrt::com_ptr<ID3D11RasterizerState> raster;
    bool ok = false;
};

struct Constants {
    float uvRect[4];
    float params[4];
    float border[4];
};

winrt::com_ptr<ID3DBlob> CompileStage(const char* entry, const char* target) {
    winrt::com_ptr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "rvm", nullptr,
                                  nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS,
                                  0, code.put(), errors.put());
    if (FAILED(hr)) {
        if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        return nullptr;
    }
    return code;
}

const Pipeline& GetPipeline() {
    static Pipeline pipeline;
    static std::once_flag once;
    std::call_once(once, [] {
        auto& g = Gfx::Get();

        auto vsCode = CompileStage("VSMain", "vs_4_0");
        auto psCode = CompileStage("PSMain", "ps_4_0");
        if (!vsCode || !psCode) return;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(Constants);
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
            SUCCEEDED(g.d3d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                               nullptr, pipeline.ps.put())) &&
            SUCCEEDED(g.d3d->CreateSamplerState(&sd, pipeline.sampler.put())) &&
            SUCCEEDED(g.d3d->CreateBuffer(&bd, nullptr, pipeline.constants.put())) &&
            SUCCEEDED(g.d3d->CreateRasterizerState(&rd, pipeline.raster.put()));
    });
    return pipeline;
}

}  // namespace

bool MirrorRenderer::Init(HWND hwnd, UINT width, UINT height) {
    std::lock_guard lock(mutex_);
    return comp_.Create(hwnd, width, height) && GetPipeline().ok;
}

void MirrorRenderer::Shutdown() {
    std::lock_guard lock(mutex_);
    rtv_ = nullptr;
    cacheSrv_ = nullptr;
    cacheTex_ = nullptr;
    cropSrv_ = nullptr;
    cropTex_ = nullptr;
    cropDirty_ = true;
}

void MirrorRenderer::Present(UINT syncInterval) {
    std::lock_guard lock(presentMutex_);
    comp_.Present(syncInterval);
}

void MirrorRenderer::Resize(UINT width, UINT height) {
    bool drew = false;
    {
        std::lock_guard lock(mutex_);
        if (!comp_.Valid()) return;
        if (width == comp_.Width() && height == comp_.Height()) return;

        // The render target view must go before the buffers are resized, and
        // nothing may be presenting meanwhile.
        rtv_ = nullptr;
        {
            std::lock_guard present(presentMutex_);
            if (!comp_.Resize(width, height)) return;
        }
        if (cacheTex_) {
            std::lock_guard device(Gfx::Get().deviceMutex);
            drew = RenderLocked();
        }
    }
    if (drew) Present(0);
}

void MirrorRenderer::SetCrop(const RECT& crop, const SIZE& baseSize) {
    std::lock_guard lock(mutex_);
    crop_ = crop;
    baseSize_ = baseSize;
    cropDirty_ = true;
    // Seed it so sizing works before the first frame lands.
    effCropW_.store(RectW(crop), std::memory_order_relaxed);
    effCropH_.store(RectH(crop), std::memory_order_relaxed);
}

void MirrorRenderer::SetTracking(TrackMode mode) {
    std::lock_guard lock(mutex_);
    track_ = mode;
    cropDirty_ = true;
}

RECT MirrorRenderer::EffectiveCrop() const {
    std::lock_guard lock(mutex_);
    return ComputeCropLocked();
}

SIZE MirrorRenderer::EffectiveCropSize() const {
    return SIZE{ effCropW_.load(std::memory_order_relaxed),
                 effCropH_.load(std::memory_order_relaxed) };
}

// Maps the stored crop onto the source's current size, clamps, and falls back
// to the whole source only if nothing of it remains in range.
RECT MirrorRenderer::ComputeCropLocked() const {
    RECT crop = crop_;

    if (track_ == TrackMode::Proportional && baseSize_.cx > 0 && baseSize_.cy > 0 &&
        cacheW_ > 0 && cacheH_ > 0 &&
        (static_cast<UINT>(baseSize_.cx) != cacheW_ ||
         static_cast<UINT>(baseSize_.cy) != cacheH_)) {
        const float sx = static_cast<float>(cacheW_) / static_cast<float>(baseSize_.cx);
        const float sy = static_cast<float>(cacheH_) / static_cast<float>(baseSize_.cy);
        crop.left   = static_cast<LONG>(std::lround(crop.left   * sx));
        crop.right  = static_cast<LONG>(std::lround(crop.right  * sx));
        crop.top    = static_cast<LONG>(std::lround(crop.top    * sy));
        crop.bottom = static_cast<LONG>(std::lround(crop.bottom * sy));
    }

    const int w = static_cast<int>(cacheW_), h = static_cast<int>(cacheH_);
    crop.left   = ClampI(crop.left,   0, w);
    crop.top    = ClampI(crop.top,    0, h);
    crop.right  = ClampI(crop.right,  0, w);
    crop.bottom = ClampI(crop.bottom, 0, h);

    if (RectW(crop) <= 0 || RectH(crop) <= 0) crop = RECT{ 0, 0, w, h };
    return crop;
}

void MirrorRenderer::SetOpacity(float opacity) {
    std::lock_guard lock(mutex_);
    opacity_ = Clampf(opacity, 0.05f, 1.0f);
}

void MirrorRenderer::SetBorder(float r, float g, float b, float a) {
    std::lock_guard lock(mutex_);
    border_[0] = r; border_[1] = g; border_[2] = b; border_[3] = a;
}

bool MirrorRenderer::EnsureCache(UINT width, UINT height) {
    if (cacheTex_ && cacheW_ == width && cacheH_ == height) return true;

    cacheSrv_ = nullptr;
    cacheTex_ = nullptr;
    cacheW_ = width;
    cacheH_ = height;
    cropDirty_ = true;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width      = width;
    desc.Height     = height;
    desc.MipLevels  = 1;
    desc.ArraySize  = 1;
    desc.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    // Render-target binding lets the stream server's video processor read
    // this texture directly instead of copying it through a scratch surface.
    desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    auto& g = Gfx::Get();
    return SUCCEEDED(g.d3d->CreateTexture2D(&desc, nullptr, cacheTex_.put())) &&
           SUCCEEDED(g.d3d->CreateShaderResourceView(cacheTex_.get(), nullptr, cacheSrv_.put()));
}

bool MirrorRenderer::EnsureCrop(UINT width, UINT height) {
    if (cropTex_ && cropTexW_ == width && cropTexH_ == height) return true;

    cropSrv_ = nullptr;
    cropTex_ = nullptr;
    cropTexW_ = width;
    cropTexH_ = height;
    cropDirty_ = true;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width      = width;
    desc.Height     = height;
    desc.MipLevels  = 0;   // Full chain, for clean minification.
    desc.ArraySize  = 1;
    desc.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags  = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    auto& g = Gfx::Get();
    return SUCCEEDED(g.d3d->CreateTexture2D(&desc, nullptr, cropTex_.put())) &&
           SUCCEEDED(g.d3d->CreateShaderResourceView(cropTex_.get(), nullptr, cropSrv_.put()));
}

bool MirrorRenderer::EnsureRenderTarget() {
    if (rtv_) return true;
    if (!comp_.Valid()) return false;
    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    return SUCCEEDED(comp_.Swap()->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void())) &&
           SUCCEEDED(Gfx::Get().d3d->CreateRenderTargetView(backBuffer.get(), nullptr, rtv_.put()));
}

void MirrorRenderer::SubmitFrame(ID3D11Texture2D* source, UINT contentWidth, UINT contentHeight) {
    if (!source || contentWidth == 0 || contentHeight == 0) return;

    // While the source grows, the frame's surface is still the old pool size
    // and only part of the new content fits. Recreating the pool delivers a
    // full frame next; drawing this one would show uninitialised texture.
    D3D11_TEXTURE2D_DESC sd{};
    source->GetDesc(&sd);
    if (contentWidth > sd.Width || contentHeight > sd.Height) return;

    bool drew = false;
    {
        std::lock_guard lock(mutex_);
        if (!comp_.Valid()) return;

        auto& g = Gfx::Get();
        std::lock_guard device(g.deviceMutex);
        if (!EnsureCache(contentWidth, contentHeight)) return;

        // WGC recycles pool textures, but we still need the pixels when only
        // the crop or window size changes.
        D3D11_BOX box{ 0, 0, 0, contentWidth, contentHeight, 1 };
        g.ctx->CopySubresourceRegion(cacheTex_.get(), 0, 0, 0, 0, source, 0, &box);
        cropDirty_ = true;
        if (presenting_.load()) drew = RenderLocked();
        RVM_LOG_SAMPLED(600, L"renderer: frame %ux%u, sink=%s", contentWidth, contentHeight,
                        sink_ ? L"set" : L"none");
        if (sink_) sink_(cacheTex_.get(), ComputeCropLocked());
    }
    if (drew) Present(1);
}

void MirrorRenderer::SetFrameSink(FrameSink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void MirrorRenderer::RepushFrame() {
    std::lock_guard lock(mutex_);
    RVM_LOG_SAMPLED(100, L"renderer: repush requested, sink=%s cache=%s",
                    sink_ ? L"set" : L"NOT SET", cacheTex_ ? L"present" : L"NONE");
    if (!sink_ || !cacheTex_) return;
    std::lock_guard device(Gfx::Get().deviceMutex);
    sink_(cacheTex_.get(), ComputeCropLocked());
}

void MirrorRenderer::Redraw() {
    if (!presenting_.load()) return;
    bool drew = false;
    {
        std::lock_guard lock(mutex_);
        if (!cacheTex_) return;
        std::lock_guard device(Gfx::Get().deviceMutex);
        drew = RenderLocked();
    }
    if (drew) Present(0);
}

bool MirrorRenderer::RenderLocked() {
    if (!comp_.Valid() || !cacheTex_ || cacheW_ == 0 || cacheH_ == 0) return false;

    auto& g = Gfx::Get();
    const auto& pipeline = GetPipeline();
    if (!pipeline.ok) return false;

    const RECT crop = ComputeCropLocked();
    if (RectW(crop) < 1 || RectH(crop) < 1) return false;

    effCropW_.store(RectW(crop), std::memory_order_relaxed);
    effCropH_.store(RectH(crop), std::memory_order_relaxed);

    const UINT outW = comp_.Width();
    const UINT outH = comp_.Height();
    const UINT cropW = static_cast<UINT>(RectW(crop));
    const UINT cropH = static_cast<UINT>(RectH(crop));

    ID3D11ShaderResourceView* srv = cacheSrv_.get();
    Constants constants{};

    const bool minifying = cropW > outW || cropH > outH;
    if (minifying) {
        // Mipped blit so heavy downscales do not shimmer.
        if (!EnsureCrop(cropW, cropH)) return false;
        if (cropDirty_ || !EqualRect(&crop, &lastCrop_)) {
            D3D11_BOX box{ static_cast<UINT>(crop.left),  static_cast<UINT>(crop.top),    0,
                           static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1 };
            g.ctx->CopySubresourceRegion(cropTex_.get(), 0, 0, 0, 0, cacheTex_.get(), 0, &box);
            g.ctx->GenerateMips(cropSrv_.get());
            lastCrop_ = crop;
            cropDirty_ = false;
        }
        srv = cropSrv_.get();
        constants.uvRect[0] = 0.0f; constants.uvRect[1] = 0.0f;
        constants.uvRect[2] = 1.0f; constants.uvRect[3] = 1.0f;
    } else {
        const float fw = static_cast<float>(cacheW_);
        const float fh = static_cast<float>(cacheH_);
        constants.uvRect[0] = crop.left   / fw;
        constants.uvRect[1] = crop.top    / fh;
        constants.uvRect[2] = crop.right  / fw;
        constants.uvRect[3] = crop.bottom / fh;
    }

    constants.params[0] = opacity_;
    constants.params[1] = kCornerRadius;
    constants.params[2] = static_cast<float>(outW);
    constants.params[3] = static_cast<float>(outH);
    for (int i = 0; i < 4; ++i) constants.border[i] = border_[i];

    if (!EnsureRenderTarget()) return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g.ctx->Map(pipeline.constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &constants, sizeof(constants));
        g.ctx->Unmap(pipeline.constants.get(), 0);
    }

    const float clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11RenderTargetView* targets[]{ rtv_.get() };
    g.ctx->OMSetRenderTargets(1, targets, nullptr);
    g.ctx->ClearRenderTargetView(rtv_.get(), clear);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(outW), static_cast<float>(outH), 0.0f, 1.0f };
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->RSSetState(pipeline.raster.get());

    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(pipeline.vs.get(), nullptr, 0);
    g.ctx->PSSetShader(pipeline.ps.get(), nullptr, 0);

    ID3D11Buffer* cbs[]{ pipeline.constants.get() };
    g.ctx->VSSetConstantBuffers(0, 1, cbs);
    g.ctx->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[]{ srv };
    ID3D11SamplerState* samplers[]{ pipeline.sampler.get() };
    g.ctx->PSSetShaderResources(0, 1, srvs);
    g.ctx->PSSetSamplers(0, 1, samplers);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    g.ctx->Draw(3, 0);

    // Unbind so the next frame can write the same texture.
    ID3D11ShaderResourceView* none[]{ nullptr };
    g.ctx->PSSetShaderResources(0, 1, none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

}  // namespace rvm
