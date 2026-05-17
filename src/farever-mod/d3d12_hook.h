#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farever {

// MinHook-based D3D12 vtable hook. v0.4.16 reduced surface area: only
// Present + ResizeBuffers are hooked. ExecuteCommandLists was dropped
// because the no_d3d12 bisection (rami v0.4.15.2) showed the D3D12
// vtable patches are the AV trigger and ECL is the highest-frequency
// hook. Overlay now creates its own DIRECT command queue (see
// overlay_init in overlay.cpp) instead of capturing the game's.
//
// Idempotent: calling install() twice is a no-op.
bool d3d12_hook_install();
void d3d12_hook_uninstall();

}  // namespace farever
