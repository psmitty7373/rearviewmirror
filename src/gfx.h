#pragma once
#include "common.h"

namespace rvm {

// Process-wide graphics: one D3D11 device for capture, mirrors and Direct2D.
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

    // Capture threads and the UI share one immediate context, and
    // ID3D11Multithread only makes single calls atomic. Hold this across a
    // whole draw, but never across Present.
    std::mutex deviceMutex;

    // Pass any failing GPU result. On the first sign of device removal the
    // handler runs once, on the thread that saw it.
    void CheckDevice(HRESULT hr);
    void SetDeviceLostHandler(std::function<void()> handler);

    // One DirectComposition device for every window, made on first use. Hold
    // compositionMutex while using it: Commit sends every pending change.
    IDCompositionDevice* Composition();
    std::mutex compositionMutex;

private:
    winrt::com_ptr<IDCompositionDevice> dcomp_;
    std::mutex lostMutex_;
    std::function<void()> onLost_;
    std::atomic<bool> lost_{ false };
};

// A DirectComposition swapchain for a window with per-pixel alpha.
class CompSurface {
public:
    CompSurface() = default;
    CompSurface(const CompSurface&) = delete;
    CompSurface& operator=(const CompSurface&) = delete;
    ~CompSurface();

    bool Create(HWND hwnd, UINT width, UINT height);
    bool Resize(UINT width, UINT height);
    void Present(UINT syncInterval);

    IDXGISwapChain1* Swap() const { return swap_.get(); }
    UINT Width()  const { return width_; }
    UINT Height() const { return height_; }
    bool Valid()  const { return swap_ != nullptr; }

private:
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<IDXGISwapChain1>     swap_;
    UINT width_ = 0, height_ = 0;
};

}  // namespace rvm
