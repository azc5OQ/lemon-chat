#ifndef EMBEDDED_OPENGL3_H
#define EMBEDDED_OPENGL3_H

// Draw on the caller's current desktop OpenGL 3.3+ context (Windows/WGL,
// Linux/GLX, or Linux/EGL with a supplied function loader). Strict OpenGL 2.x
// and OpenGL ES contexts are not supported. Use the same context/thread for
// attachment, all drawing and detach. The host owns presentation and input.

#include "types.h"
#include "meek_ui_config.h"
#include "opengl3_renderer.h"

GUI_API boole embedded_opengl3__attach(void);
// NULL selects WGL on Windows or GLX on Linux; EGL hosts pass eglGetProcAddress through a void* adapter.
GUI_API boole embedded_opengl3__attach_with_loader(meek_opengl_get_proc_fn resolver);
GUI_API void  embedded_opengl3__detach(void);
GUI_API boole embedded_opengl3__is_attached(void);
GUI_API boole embedded_opengl3__begin_frame(int64 width, int64 height);
GUI_API void  embedded_opengl3__end_frame(void);

#endif
