#ifndef D3D12_RENDERER_H
#define D3D12_RENDERER_H

// d3d12_renderer.h - the entry points of the Direct3D 12 backend that a host
// calls directly: adopting a device, queue and swap chain the host owns, and
// dropping the back buffer views before the host resizes. Everything else
// goes through renderer.h.

#include "types.h"
#include "meek_ui_config.h"

typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12CommandQueue ID3D12CommandQueue;
typedef struct IDXGISwapChain IDXGISwapChain;

GUI_API boole renderer__init_adopt_d3d12(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapchain);
GUI_API void  renderer__d3d12_release_render_targets(void);

#endif
