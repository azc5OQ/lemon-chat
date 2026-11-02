#ifndef LEMON_DESKTOP_UI_HEADERS_H
#define LEMON_DESKTOP_UI_HEADERS_H

#include "clib.h"

// Meek's pointer-sized aliases use long long on LP64; Lemon's use nint.
// Keep each library's exact declarations without changing either upstream copy.
#define nuint meek_ui_nuint
#define nint meek_ui_nint
#include "gui_api.h"
#include "gui.h"
#include "scene.h"
#include "platform.h"
#include "font.h"
#include "hot_reload.h"
#include "parser_style.h"
#undef nint
#undef nuint

#endif
