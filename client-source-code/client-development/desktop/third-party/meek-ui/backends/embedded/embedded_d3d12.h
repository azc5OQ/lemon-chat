#ifndef EMBEDDED_D3D12_H
#define EMBEDDED_D3D12_H

// embedded_d3d12.h - meek-ui inside a Direct3D 12 frame the host owns. The
// host passes its device, its direct command queue and its swap chain;
// meek-ui records into command lists of its own and executes them on the
// host's queue right before the host presents. Direct3D 12 keeps pipeline
// state per command list, so nothing of the host's state is touched and
// nothing needs to be backed up.

#include "types.h"
#include "meek_ui_config.h"

typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12CommandQueue ID3D12CommandQueue;
typedef struct IDXGISwapChain IDXGISwapChain;

GUI_API boole embedded_d3d12__attach(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapchain);
GUI_API void  embedded_d3d12__detach(void);
GUI_API boole embedded_d3d12__is_attached(void);
GUI_API void  embedded_d3d12__release_render_targets(void);
GUI_API boole embedded_d3d12__begin_frame(int64 width, int64 height);
GUI_API void  embedded_d3d12__end_frame(void);

#endif
