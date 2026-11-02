#ifndef EMBEDDED_D3D9_H
#define EMBEDDED_D3D9_H

// embedded_d3d9.h - meek-ui inside a Direct3D 9 frame the host owns. The
// overlay draws between the host's BeginScene and EndScene; the host's
// pipeline state is captured in a state block (plus the three transforms
// the state block does not reliably cover) and put back after the frame.

#include "types.h"
#include "meek_ui_config.h"

typedef struct IDirect3DDevice9 IDirect3DDevice9;

GUI_API boole embedded_d3d9__attach(IDirect3DDevice9* device);
GUI_API void  embedded_d3d9__detach(void);
GUI_API boole embedded_d3d9__is_attached(void);
GUI_API void  embedded_d3d9__on_device_lost(void);
GUI_API boole embedded_d3d9__on_device_reset(void);
GUI_API boole embedded_d3d9__begin_frame(int64 width, int64 height);
GUI_API void  embedded_d3d9__end_frame(void);

#endif
