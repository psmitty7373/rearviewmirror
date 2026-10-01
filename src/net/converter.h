#pragma once
#include "gfx.h"

namespace rvm::net {

// GPU colour conversion through the D3D11 video processor: BGRA -> NV12 for
// the hardware encoder, NV12 -> BGRA for display. The video context is part of
// the immediate context, so callers hold Gfx::deviceMutex.
class VideoConverter {
public:
    bool Init();

    // Converts `srcRect` of `src` (array slice `srcSubresource`) into all of
    // `dst`, scaling if the sizes differ. Pass nullptr for the whole source.
    bool Convert(ID3D11Texture2D* src, UINT srcSubresource, const RECT* srcRect,
                 ID3D11Texture2D* dst);

private:
    bool Ensure(UINT inW, UINT inH, UINT outW, UINT outH);
    bool EnsureScratch(DXGI_FORMAT format, UINT w, UINT h);
    ID3D11VideoProcessorInputView*  InputView(ID3D11Texture2D* src, UINT slice);
    ID3D11VideoProcessorOutputView* OutputView(ID3D11Texture2D* dst);

    winrt::com_ptr<ID3D11VideoDevice>              device_;
    winrt::com_ptr<ID3D11VideoContext>             context_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor>           processor_;
    UINT inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;

    // Views of the textures seen lately: the same few (a stream's slots, a
    // decoder's array) come round every frame. A view holds its texture, so
    // a recycled address cannot alias a dead one; the lists are short and are
    // dropped whenever the processor is rebuilt for new sizes.
    struct InputEntry {
        ID3D11Texture2D* texture;
        UINT slice;
        winrt::com_ptr<ID3D11VideoProcessorInputView> view;
    };
    struct OutputEntry {
        ID3D11Texture2D* texture;
        winrt::com_ptr<ID3D11VideoProcessorOutputView> view;
    };
    static constexpr size_t kMaxViews = 8;
    std::vector<InputEntry>  inputs_;    // Least recently used first.
    std::vector<OutputEntry> outputs_;

    // Input views need a render-target or decoder binding. Sources without
    // one are copied through this texture first.
    winrt::com_ptr<ID3D11Texture2D> scratch_;
    UINT scratchW_ = 0, scratchH_ = 0;
    DXGI_FORMAT scratchFormat_ = DXGI_FORMAT_UNKNOWN;
};

// BGRA -> NV12 with shaders, for the CPU encoder. Needs no video processor, so
// it works on basic display adapters, WARP and remote sessions too. The frame
// is drawn into one R8 texture just as NV12 lays it out in memory, the luma
// rows and then the interleaved chroma rows beneath them, so one copy to a
// staging texture is all the CPU has to read. BT.709 with studio range, like
// VideoConverter. Callers hold Gfx::deviceMutex.
class Nv12Packer {
public:
    bool Init();

    // Draws `srcRect` of `src`, scaled to fill the frame, into `staging`: an
    // R8 staging texture `width` x `height * 3 / 2` for a `width` x `height`
    // frame. `src` must allow a shader resource view.
    bool Pack(ID3D11Texture2D* src, const RECT& srcRect, ID3D11Texture2D* staging);

private:
    bool EnsureTarget(UINT width, UINT rows);
    ID3D11ShaderResourceView* SourceView(ID3D11Texture2D* src);

    winrt::com_ptr<ID3D11Texture2D>        target_;   // Drawn into, then copied out.
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
    UINT targetW_ = 0, targetRows_ = 0;

    // The source's view. Frames keep coming from the same texture; holding it
    // means a recycled address cannot alias a dead one.
    winrt::com_ptr<ID3D11Texture2D>          srcTex_;
    winrt::com_ptr<ID3D11ShaderResourceView> srcSrv_;
};

}  // namespace rvm::net
