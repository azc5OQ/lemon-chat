#ifndef OPENGL2_RENDERER_H
#define OPENGL2_RENDERER_H
#include "types.h"
#include "meek_ui_config.h"

typedef void* (*meek_opengl2_get_proc_fn)(const char* name);
GUI_API boole renderer__init_adopt_opengl2(meek_opengl2_get_proc_fn loader);
// Internal boundary shared by standalone and embedded hosts.
GUI_API boole renderer__opengl2_save_state(void);
GUI_API void renderer__opengl2_restore_state(void);
GUI_API boole renderer__opengl2_set_display_gamma(const ushort* ramp);
#endif
