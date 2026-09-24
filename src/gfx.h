#pragma once
#include "common.h"

namespace rvm {

// Process-wide graphics objects. One D3D11 device backs capture, every mirror
// renderer and the Direct2D UI, so frames never leave the GPU.
struct Gfx {
    static Gfx& Get();
    void Init();

    winrt::com_ptr<ID3D11Device>        d3d;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<IDXGIDevice>         dxgi;
    winrt::com_ptr<IDXGIFactory2>       factory;
    winrt::com_ptr<ID2D1Factory1>       d2dFactory;
    winrt::com_ptr<ID2D1Device>         d2dDevice;
    winrt::com_ptr<IDWriteFactory>      dwrite;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{ nullptr };

    // Mirrors draw from capture threads, Direct2D from the UI thread, on one
    // shared immediate context. ID3D11Multithread only makes individual calls
    // atomic: a mirror frame landing inside a D2D BeginDraw/EndDraw pair still
    // clobbers its state. Hold this across a whole draw, but never across Present.
    std::mutex deviceMutex;
};

// A DirectComposition-backed presentation surface for a borderless window with
// per-pixel alpha. Shared by the mirror windows and the overlays.
class CompSurface {
public:
    bool Create(HWND hwnd, UINT width, UINT height);
    bool Resize(UINT width, UINT height);
    void Present(UINT syncInterval);

    IDXGISwapChain1* Swap() const { return swap_.get(); }
    UINT Width()  const { return width_; }
    UINT Height() const { return height_; }
    bool Valid()  const { return swap_ != nullptr; }

private:
    winrt::com_ptr<IDCompositionDevice> dcomp_;
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<IDXGISwapChain1>     swap_;
    UINT width_ = 0, height_ = 0;
};

}  // namespace rvm
