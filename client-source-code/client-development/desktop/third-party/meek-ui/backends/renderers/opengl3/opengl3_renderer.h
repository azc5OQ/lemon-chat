#ifndef OPENGL3_RENDERER_H
#define OPENGL3_RENDERER_H

// opengl3_renderer.h - the one entry point of the OpenGL backend that a host
// calls directly: adopting the context that is current on the calling
// thread. Everything else goes through renderer.h.

#include "types.h"
#include "meek_ui_config.h"

typedef void* (*meek_opengl_get_proc_fn)(const char* name);
GUI_API boole renderer__prepare_adopt_opengl(meek_opengl_get_proc_fn resolver);
GUI_API void* renderer__opengl_proc_address(const char* name);
GUI_API boole renderer__init_adopt_opengl(void);

#endif
