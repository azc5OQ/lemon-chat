#ifndef EMBEDDED_D3D11_H
#define EMBEDDED_D3D11_H

// embedded_d3d11.h - meek-ui inside a Direct3D 11 frame the host owns.
// The host (a game, a plugin host, a managed application) keeps its
// device, its swap chain and its loop; meek-ui draws between the host's
// own drawing and its Present. The host's pipeline state is backed up
// before every meek-ui frame and restored afterwards, so the host never
// sees a state change. See backends/embedded/README.md for the model
// and examples/embedded_d3d11/main.c for a complete host.

#include "types.h"
#include "meek_ui_config.h"

typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct IDXGISwapChain IDXGISwapChain;

GUI_API boole embedded_d3d11__attach(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain);
GUI_API void  embedded_d3d11__detach(void);
GUI_API boole embedded_d3d11__is_attached(void);
GUI_API void  embedded_d3d11__release_render_target(void);
GUI_API boole embedded_d3d11__begin_frame(int64 width, int64 height);
GUI_API void  embedded_d3d11__end_frame(void);

#endif
