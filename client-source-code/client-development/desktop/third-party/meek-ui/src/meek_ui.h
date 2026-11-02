#ifndef MEEK_UI_H
#define MEEK_UI_H

// meek_ui.h - the single include for host applications.
// meek-ui renders HTML/CSS-shaped .ui + .style files (or a scene
// tree built in C) through one of several graphics backends. A host
// needs this header, the src/ directory on its include path, and
// one renderer from backends/renderers/ + OS integration from backends/platform/.

// Typical host, library owns the window:
// #include "meek_ui.h"
// int main(int argc, char** argv)
// {
// gui_app_config_t cfg = { L"my app", 800, 600, scene__hex(0x101010) };
// platform__init(&cfg);
// font__register_dir("fonts");
// hot_reload__watch_style("app.style");
// hot_reload__watch_ui("app.ui");
// while (platform__tick()) { hot_reload__tick(); }
// platform__shutdown();
// return 0;
// }

// Embedded host, the host owns the window / device / loop: define
// MEEK_UI_HOST_OWNS_MAIN, skip platform__init, include the matching
// backends/embedded/embedded_<api>.h, call embedded_<api>__attach with
// your device, then per frame after your own drawing:
// scene__begin_frame_time, scene__resolve_styles, animator__tick,
// scene__layout, embedded_<api>__begin_frame, scene__emit_draws,
// embedded_<api>__end_frame; feed input through
// embedded_win32__on_window_event. backends/embedded/README.md has
// the full sequence.
// cfg.hidden = TRUE creates the window without showing it; the test
// runner uses it so a sweep leaves the desktop alone.

#include "meek_ui_config.h"
#include "types.h"
#include "gui.h"
#include "scene.h"
#include "font.h"
#include "renderer.h"
#include "gradient.h"
#include "platform.h"
#include "hot_reload.h"
#include "canvas.h"
#include "scroll.h"
#include "animator.h"
#include "motion.h"
#include "widget.h"
#include "widget_registry.h"
#include "widgets/widget_datepicker.h"
#include "widgets/widget_hexview.h"
#include "widgets/widget_filepicker.h"
#include "atlas.h"
#include "fs.h"
#include "third_party/log.h"
#include "clib/stdlib.h"
#include "clib/memory_manager.h"

#endif
