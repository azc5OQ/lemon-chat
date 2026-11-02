#ifndef D3D11_RENDERER_H
#define D3D11_RENDERER_H

// d3d11_renderer.h - the one entry point of the Direct3D 11 backend that a
// host calls directly: adopting a device and swap chain the host already
// owns. Everything else goes through renderer.h, which is backend-neutral.
// The COM interface names are forward-declared so this header works before
// or after <d3d11.h>; C11 allows the repeated typedef.

#include "types.h"
#include "meek_ui_config.h"

typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct IDXGISwapChain IDXGISwapChain;

GUI_API boole renderer__init_adopt(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain);
GUI_API void  renderer__d3d11_release_render_target(void);

#endif
