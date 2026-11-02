#ifndef EMBEDDED_OPENGL2_H
#define EMBEDDED_OPENGL2_H
#include "opengl2_renderer.h"

// Desktop OpenGL 2.0 or newer compatibility context. No OpenGL 3 functions
// or shaders are required. Core-only and OpenGL ES contexts are rejected.
// The host keeps its context current on the same thread for all calls, owns
// presentation and input, and releases meek-ui before destroying the context.
// Drawing targets the window's default framebuffer without clearing it.
// Keep resource creation, hot reload, layout and drawing inside begin/end so
// texture uploads cannot inherit the host's pixel-transfer state.
// NULL loader selects WGL on Windows or GLX on Linux. EGL hosts supply a
// void* wrapper around eglGetProcAddress. Only one attachment is active.
GUI_API boole embedded_opengl2__attach(void);
GUI_API boole embedded_opengl2__attach_with_loader(meek_opengl2_get_proc_fn loader);
GUI_API void embedded_opengl2__detach(void);
GUI_API boole embedded_opengl2__is_attached(void);
GUI_API boole embedded_opengl2__begin_frame(int64 width, int64 height);
GUI_API void embedded_opengl2__end_frame(void);
/**
 * Compensates only the finished UI texture for a host's display gamma ramp.
 * The host's image and operating-system ramp remain untouched.
 * @function embedded_opengl2__set_display_gamma
 * @param const ushort* ramp -> Three monotonic 256-entry RGB ramps, or NULL to reset.
 * @return boole -> FALSE for a nonmonotonic ramp; otherwise TRUE.
 */
GUI_API boole embedded_opengl2__set_display_gamma(const ushort* ramp);
#endif
