#pragma once

#include <d3d12.h>

namespace farever {

struct LoadedTexture {
    ID3D12Resource*             resource = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    UINT                        width    = 0;
    UINT                        height   = 0;
};

// PNG/WIC → D3D12 texture upload at SRV heap `slot`. Returns false on
// any failure (file missing, decode error, allocation failure). On
// success the caller owns the resource; release with release_texture.
bool load_texture_from_file(ID3D12Device* device,
                            ID3D12DescriptorHeap* srv_heap, UINT slot,
                            const wchar_t* path, LoadedTexture* out);

void release_texture(LoadedTexture* tex);

}  // namespace farever
