#ifndef D3D9_RENDERER_H
#define D3D9_RENDERER_H

// d3d9_renderer.h - the entry points of the Direct3D 9 backend that a host
// calls directly: adopting a device the host owns, and the two calls around
// the host's device reset. Everything else goes through renderer.h.

#include "types.h"
#include "meek_ui_config.h"

typedef struct IDirect3DDevice9 IDirect3DDevice9;

GUI_API boole renderer__init_adopt_d3d9(IDirect3DDevice9* device);
GUI_API void  renderer__d3d9_on_device_lost(void);
GUI_API boole renderer__d3d9_on_device_reset(void);

#endif
