#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farever {

// Hook-side callbacks invoked from d3d12_hook.cpp.
void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* captured_queue);
void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT width, UINT height);
void overlay_after_resize(IDXGISwapChain3* swap_chain);
void overlay_shutdown();

}  // namespace farever
