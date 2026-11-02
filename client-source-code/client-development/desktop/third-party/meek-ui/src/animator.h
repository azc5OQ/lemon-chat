#ifndef ANIMATOR_H
#define ANIMATOR_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// animator.h - per-frame animation engine.
// Walks the scene tree once per frame, advancing per-node animation
// timers (scroll-momentum-style integration on top of resolved.* values
// that scene__resolve_styles wrote first). Currently supports one
// animation kind:
// :appear opacity fade
// If a node's resolved.appear_ms > 0, the animator multiplies
// the node's color alphas (bg.a, fg.a, color.a) by an eased
// fade factor that ramps 0 -> 1 over appear_ms milliseconds.
// The factor is computed from the node's own appear_age_ms,
// which the animator increments by scene__frame_delta_ms each
// tick.
// Future additions plug in here:
// - :disappear (needs deferred destruction so the animation can play
// before the node is freed).
// - transition: <prop> <ms> <easing> for state changes (hover/press).
// Would track a "from-value" per animatable property + a target,
// interpolating each frame.
// CALL ORDER (each frame):
// 1. platform__tick stamps scene__begin_frame_time(now_ms).
// 2. scene__resolve_styles() writes resolved.* from the per-state
// style slot.
// 3. animator__tick() walks the tree, advances appear_age_ms by
// scene__frame_delta_ms, multiplies resolved color alphas by
// the appear-fade factor.
// 4. scene__layout() computes bounds.
// 5. renderer__begin_frame() / scene__emit_draws() / __end_frame().
// Step 3 must come AFTER step 2 (resolve overwrites resolved.*) and
// BEFORE step 5 (emit reads resolved.*).

GUI_API void  animator__tick(void);

GUI_API int   animator__has_active(void);

GUI_API void  animator__set_size_transitions_enabled(boole on);

GUI_API boole animator__size_transitions_enabled(void);

// Evaluate the same easing curve for style transitions and widget motion.
GUI_API float animator__ease(gui_easing_e kind, const float* params, float progress);

#endif
