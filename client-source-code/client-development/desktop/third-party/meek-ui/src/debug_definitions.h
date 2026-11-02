// debug_definitions.h - gated debug logging flags.
// Each DBG_* macro expands to `if (1)` when its corresponding
// MEEK_DBG_* define is set, or `if (0)` when not. Arguments still
// get type-checked in the inert case; the compiler dead-code
// eliminates the branch at -O2.
// Usage:
// DBG_TAP log_info("%s %u", "tile tapped handle=", handle);
// Turn on a specific flag by uncommenting its MEEK_DBG_* below, OR
// pass `-DMEEK_DBG_TAP=1` on the compile command line.  Nothing is
// active by default -- keep it that way so production builds stay
// clean. Include this header anywhere you want to emit a gated log.

#ifndef MEEK_DEBUG_DEFINITIONS_H
#define MEEK_DEBUG_DEFINITIONS_H

// =============================================================
// Flags -- uncomment to activate. Commented = silent.
// =============================================================

#define MEEK_DBG_TAP          1 // tile tap, route tap, dispatch_click
#define MEEK_DBG_FULLSCREEN   1 // show_fullscreen / show_switcher / focused_handle
// #define MEEK_DBG_TEX       1   // widget_process_window set_texture, egl image import/destroy
                                  // (dmabuf import / buffer_id cache hit/miss path is stable;
                                  // re-enable only when debugging texture lifecycle regressions)
// #define MEEK_DBG_LAYOUT       1   // per-node layout bounds (very noisy)
// #define MEEK_DBG_TICK         1   // per-tick timing + phase markers
// #define MEEK_DBG_ANIM      1 // per-node appear/disappear tracing; enable only for diagnosis
// #define MEEK_DBG_COMMIT       1   // wl_surface.commit, compositor scanout
// #define MEEK_DBG_STYLE        1   // style apply_rules / resolve walks
#define MEEK_DBG_INPUT        1 // mouse / touch event dispatch in scene_input

// =============================================================
// Macro expansion. Do not edit.
// =============================================================

#ifdef MEEK_DBG_TAP
#define DBG_TAP        if (1)
#else
#define DBG_TAP        if (0)
#endif

#ifdef MEEK_DBG_FULLSCREEN
#define DBG_FULLSCREEN if (1)
#else
#define DBG_FULLSCREEN if (0)
#endif

#ifdef MEEK_DBG_TEX
#define DBG_TEX        if (1)
#else
#define DBG_TEX        if (0)
#endif

#ifdef MEEK_DBG_LAYOUT
#define DBG_LAYOUT     if (1)
#else
#define DBG_LAYOUT     if (0)
#endif

#ifdef MEEK_DBG_TICK
#define DBG_TICK       if (1)
#else
#define DBG_TICK       if (0)
#endif

#ifdef MEEK_DBG_ANIM
#define DBG_ANIM       if (1)
#else
#define DBG_ANIM       if (0)
#endif

#ifdef MEEK_DBG_COMMIT
#define DBG_COMMIT     if (1)
#else
#define DBG_COMMIT     if (0)
#endif

#ifdef MEEK_DBG_STYLE
#define DBG_STYLE      if (1)
#else
#define DBG_STYLE      if (0)
#endif

#ifdef MEEK_DBG_INPUT
#define DBG_INPUT      if (1)
#else
#define DBG_INPUT      if (0)
#endif

#endif
