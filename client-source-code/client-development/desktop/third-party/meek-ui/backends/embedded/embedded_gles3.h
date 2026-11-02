#ifndef EMBEDDED_GLES3_H
#define EMBEDDED_GLES3_H
#include "types.h"
#include "meek_ui_config.h"

// Adopt the caller's current OpenGL ES 3.0+ context. The host owns the
// context, window, device, presentation and input. Call on one thread with
// the same context current until detach. No desktop GL function loader.
// Draws over the default framebuffer without clearing or swapping it.
// Keep resource creation, hot reload, layout and drawing inside begin/end.
// End transform feedback before attachment/drawing; an active, unpaused
// transform-feedback operation makes attach/begin fail without changing state.
GUI_API boole embedded_gles3__attach(void);
GUI_API void embedded_gles3__detach(void);
GUI_API boole embedded_gles3__is_attached(void);
GUI_API boole embedded_gles3__begin_frame(int64 width, int64 height);
GUI_API void embedded_gles3__end_frame(void);
#endif
