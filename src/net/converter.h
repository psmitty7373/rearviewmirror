#pragma once
#include "gfx.h"

namespace rvm::net {

// GPU colour conversion through the D3D11 video processor: BGRA -> NV12 for
// the encoder, NV12 -> BGRA for display. The video context is part of the
// immediate context, so callers hold Gfx::deviceMutex.
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

    winrt::com_ptr<ID3D11VideoDevice>              device_;
    winrt::com_ptr<ID3D11VideoContext>             context_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor>           processor_;
    UINT inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;

    // Input views need a render-target or decoder binding. Sources without
    // one are copied through this texture first.
    winrt::com_ptr<ID3D11Texture2D> scratch_;
    UINT scratchW_ = 0, scratchH_ = 0;
    DXGI_FORMAT scratchFormat_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace rvm::net
