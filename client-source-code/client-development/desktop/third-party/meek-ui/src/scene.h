#ifndef SCENE_H
#define SCENE_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// scene.h - tree, styles, handlers, layout, hit test, input.
// paired with scene.c. all of this is portable c (no os calls).
// public functions in this file are prefixed scene__ so call sites
// document the owning translation unit. types stay in the project
// namespace (gui_*) because they're shared.

// ===== color constructors ===================================================

GUI_API gui_color_t scene__rgba(float r, float g, float b, float a);

GUI_API gui_color_t scene__rgb(float r, float g, float b);

GUI_API gui_color_t scene__hex(uint rrggbb);

// ===== handler registration =================================================

GUI_API void scene__register_handler(char* name, gui_handler_fn fn);

// convenience macro: SCENE_HANDLER(do_thing) expands to
// scene__register_handler("do_thing", do_thing).
#define SCENE_HANDLER(fn) scene__register_handler(#fn, fn)

GUI_API uint scene__hash_name(char* name);

// ===== tree construction ====================================================

GUI_API gui_node_t* scene__node_new(gui_node_type_e type);

GUI_API void      scene__add_child(gui_node_t* parent, gui_node_t* child);

GUI_API void*     scene__widget_state(gui_node_t* n);

GUI_API void      scene__set_on_click(gui_node_t* node, char* handler_name);

GUI_API void      scene__set_on_change(gui_node_t* node, char* handler_name);

// Replace any node caption or editable content with complete UTF-8 text.
// Length excludes the terminator. Failure leaves text and editing state intact.
// Inputs reject line breaks/tabs; textarea accepts them. No on_change is emitted.
// A successful input replacement collapses selection at the end, even for equal text.
GUI_API boole     scene__set_text(gui_node_t* node, const char* text, int64 length);

// Limits count bytes excluding NUL, from 0 through MEEK_UI_MAX_TEXT_BYTES.
// Reducing below the current text length fails without truncating the text.
GUI_API boole     scene__set_text_limit(gui_node_t* node, int64 limit);
// Reserve room for length bytes plus NUL without changing content or selection.
// Success may invalidate a saved text pointer. Capacity is node->text_capacity.
GUI_API boole     scene__reserve_text(gui_node_t* node, int64 length);

GUI_API void      scene__node_free(gui_node_t* node);

GUI_API void      scene__reconcile_tree(gui_node_t* old_tree, gui_node_t* new_tree);

// ===== root + layout + resolve ==============================================

GUI_API void      scene__set_root(gui_node_t* root);

GUI_API gui_node_t* scene__root(void);

GUI_API void      scene__layout(int64 viewport_w, int64 viewport_h);

GUI_API void      scene__layout_node(gui_node_t* n, float x, float y, float avail_w, float avail_h);

// Measure bounds without committing current or target scroll offsets, including
// nested layout calls. Custom container widgets should use this for probes.
GUI_API void      scene__measure_node(gui_node_t* n, float x, float y, float avail_w, float avail_h);
GUI_API boole     scene__layout_is_measuring(void);

GUI_API void      scene__layout_children_as_column(gui_node_t* parent);

GUI_API void      scene__layout_children_as_row(gui_node_t* parent);
GUI_API void      scene__layout_column_flow(gui_node_t* parent, float cx, float cy, float cw, float ch, float scroll_x, float scroll_y, float* out_content_w, float* out_content_h);
GUI_API void      scene__layout_row_flow(gui_node_t* parent, float cx, float cy, float cw, float ch);
GUI_API void      scene__apply_min_max(gui_node_t* n, float* w, float* h, float scale);

GUI_API void      scene__emit_draws(void);

GUI_API void      scene__set_scale(float scale);

GUI_API float     scene__scale(void);

GUI_API float     scene__px(float logical);

// ===== frame timing ========================================================
// The platform layer captures a millisecond timestamp at the START of
// each frame and hands it here via scene__begin_frame_time. Every
// time-sensitive subsystem (animator today; future hot-reload, etc.)
// then reads scene__frame_time_ms and scene__frame_delta_ms during
// the frame instead of taking its own clock samples. One source of
// truth, no clock-skew between subsystems within a frame.

GUI_API void  scene__begin_frame_time(int64 now_ms);

GUI_API int64 scene__frame_time_ms(void);

GUI_API int64 scene__frame_delta_ms(void);

GUI_API void  scene__set_deterministic_clock(int64 step_ms);

// ===== overlay (top-most popup layer) =====================================
// A single global "overlay" slot used for widgets that need to draw ABOVE
// the rest of the scene -- currently just <select>'s dropdown popup, but
// the same mechanism would suit modal dialogs, tooltips, autocomplete
// menus, context menus, etc. Only ONE overlay can be active at a time
// (matches how OS dropdowns work -- opening a second auto-closes the
// first).
// Set: widget calls scene__set_overlay(self, popup_bounds, draw_fn) when
// it opens. Bounds tell scene's hit-test which screen region is
// "on top" so clicks inside that region route to the overlay node
// first instead of falling through to whatever's underneath.
// Clear: scene__set_overlay(NULL, ..., NULL) closes the overlay.
// Draw: after the normal emit_draws walk completes, scene calls draw_fn
// with the overlay node + scale, so the popup paints on top of
// everything in the tree.

// Function pointer for the overlay draw callback. Receives the overlay
// node + the global UI scale; called by scene__emit_draws after the
// normal recursive walk is done.
typedef void (*gui_overlay_draw_fn)(gui_node_t* node, float scale);

GUI_API void  scene__set_overlay(gui_node_t* node, gui_rect_t bounds, gui_overlay_draw_fn draw_fn);

GUI_API void  scene__viewport(int64* out_w, int64* out_h);

GUI_API void  scene__shift_tree(gui_node_t* n, float dx, float dy);

GUI_API float scene__layout_width(gui_node_t* n, float avail_w, float scale);

GUI_API float scene__layout_height(gui_node_t* n, float avail_h, float scale);

GUI_API void  scene__set_background_color_override(gui_node_t* n, gui_color_t c);

GUI_API void  scene__clear_background_color_override(gui_node_t* n);

GUI_API void  scene__set_font_color_override(gui_node_t* n, gui_color_t c);
// Empty family and nonpositive size restore the stylesheet values.
GUI_API void  scene__set_font_override(gui_node_t* n, const char* family, float size);

GUI_API void  scene__set_background_image_override(gui_node_t* n, char* name);

GUI_API void  scene__clear_background_image_override(gui_node_t* n);

GUI_API void  scene__clear_font_color_override(gui_node_t* n);

GUI_API void  scene__emit_default_bg(gui_node_t* n, float scale);

GUI_API void  scene__layout_absolute_child(gui_node_t* parent, gui_node_t* c, float content_x, float content_y, float content_w, float content_h);

GUI_API void      scene__layout_positioned_children(gui_node_t* parent, float content_x, float content_y, float content_w, float content_h);

GUI_API void      scene__apply_position_offset(gui_node_t* parent, gui_node_t* c, float content_w, float content_h);

GUI_API void  scene__emit_border(gui_node_t* n, gui_rect_t bounds, float scale);

GUI_API float scene__border_width(gui_node_t* n, float scale);

GUI_API void  scene__submit_fitted_image(gui_rect_t bounds, void* tex, int natural_w, int natural_h, int fit, gui_color_t tint, float radius);

GUI_API void  scene__submit_fitted_image_uv(gui_rect_t bounds, void* tex, int natural_w, int natural_h, int fit, gui_color_t tint, float radius, float u0, float v0, float u1, float v1);

GUI_API void  scene__popup_ok(char* message, char* handler_name);

GUI_API void  scene__popup_confirm(char* message, char* handler_name);

GUI_API void  scene__show_keyboard(void);

GUI_API void  scene__hide_keyboard(void);

GUI_API boole scene__keyboard_visible(void);

GUI_API void  widget_keyboard__set_height_pct(int pct);

GUI_API void  widget_keyboard__set_font_size_pct(int pct);

GUI_API int   widget_keyboard__get_height_pct(void);
GUI_API int   widget_keyboard__get_font_size_pct(void);

GUI_API void  scene__set_virtual_keyboard_enabled(boole enabled);

GUI_API boole scene__virtual_keyboard_enabled(void);

// Platform hook for text-input focus events. When focus enters or
// leaves a GUI_NODE_TYPE__INPUT / GUI_NODE_TYPE__TEXTAREA, scene_input calls
// this if it's been installed. Its purpose is to let a host
// platform inform the compositor (e.g. via zwp_text_input_v3.enable
// /disable) that an editable widget has focus -- so the compositor
// can ask the system shell to show / hide its on-screen keyboard.
// If a hook is installed, scene_input will NOT auto-attach
// widget_keyboard to the scene on focus change -- the assumption is
// that the platform/compositor stack provides the keyboard out-of-
// process. Standalone targets (Windows, DRM kiosk) leave the hook
// unset and continue to use the in-tree widget_keyboard.
// Pass NULL to clear (reverts to the in-tree-widget_keyboard path).
typedef void (*scene_text_input_focus_fn)(boole active);
GUI_API void scene__set_text_input_focus_hook(scene_text_input_focus_fn fn);

GUI_API void scene__deliver_char(gui_node_t* node, uint codepoint);

GUI_API void scene__deliver_key(gui_node_t* node, int64 vk, boole down);

// Character-input redirect. When set to a non-NULL function, every
// call to scene__on_char / scene__deliver_char first passes the
// codepoint to the redirect. If the redirect returns TRUE, the
// normal widget vtable delivery is skipped; FALSE falls through.
// Use case: meek-shell's IME bridge. When a third-party Wayland
// client requests an IME via zwp_text_input_v3, meek-shell installs
// a redirect that forwards codepoints through the meek_shell_v1
// extension back to the compositor, which emits commit_string to
// the focused text_input. Clears by passing NULL.
// fn - NULL = disabled (default).
typedef boole (*scene_char_redirect_fn)(uint codepoint);
GUI_API void      scene__set_char_redirect(scene_char_redirect_fn fn);

GUI_API boole     scene__has_char_redirect(void);

GUI_API void      scene__set_active_modifiers(uint mask);
GUI_API uint      scene__get_active_modifiers(void);

GUI_API gui_node_t* scene__overlay_node(void);
// Move a widget's animation slot (gui_node_t.widget_anim) toward target
// with an exponential ease whose time constant is tau_ms (about 63 % of
// the distance per tau); the first call snaps. Returns the new value and
// keeps the render loop alive while it still moves.
GUI_API float scene__widget_ease(gui_node_t* node, float target, float tau_ms);

GUI_API void      scene__dispatch_click(gui_node_t* n, int64 x, int64 y, int64 button);

GUI_API void      scene__dispatch_gesture_by_name(const char* handler_name);

GUI_API void      scene__dispatch_change(gui_node_t* n, float new_value);

GUI_API void      scene__dispatch_event(gui_node_t* n, gui_event_t* ev);

GUI_API void      scene__resolve_styles(void);

GUI_API gui_node_t* scene__hit_test(gui_node_t* root, int64 x, int64 y);

GUI_API gui_node_t* scene__find_by_id(char* id);

// ===== input callbacks =====================================================
// called by the platform layer when raw input arrives. updates the
// hover/pressed state machine and dispatches click events.

GUI_API void      scene__on_mouse_move(int64 x, int64 y);
// Last pointer position in the same device-pixel space as node bounds.
// FALSE until the host delivers a pointer event.
GUI_API boole     scene__pointer_position(int64* x, int64* y);

GUI_API void      scene__on_mouse_button(int64 button, boole down, int64 x, int64 y);

GUI_API void      scene__on_mouse_wheel(int64 x, int64 y, float delta);

GUI_API void      scene__on_touch_scroll(int64 x, int64 y, float delta_pixels);

GUI_API gui_node_t* scene__pressed(void);

GUI_API void      scene__cancel_press(void);

GUI_API void      scene__on_resize(int64 w, int64 h);

GUI_API void      scene__on_char(uint codepoint);

GUI_API void      scene__on_key(int64 vk, boole down);

GUI_API void      scene__set_focus(gui_node_t* node);

GUI_API gui_node_t* scene__focus(void);

// ===== selector-based style registration ===================================
// stand-in for the .style file parser. supported selector forms:
// "Type"                  any node of that type
// ".class"                any node with this class token
// "#id"                   the unique node with this id
// "Type.class"            both must match
// "Type#id"               both must match
// "<base>:hover"          applies to GUI_NODE_STATE__HOVER
// "<base>:pressed"        applies to GUI_NODE_STATE__PRESSED
// "<base>:disabled"       applies to GUI_NODE_STATE__DISABLED
// "<base>:default"        applies to GUI_NODE_STATE__DEFAULT
// precedence within one state:
// id beats class beats type. within one tier, last registered wins.

GUI_API void scene__register_style(char* selector, const gui_style_t* style);

GUI_API void scene__register_style_important(char* selector, const gui_style_t* style);

// Add one rule; zero means invalid input/selector or capacity exhausted.
// Updating keeps its selector, priority and cascade order. Clear invalidates handles.
GUI_API uint64 scene__add_style(char* selector, const gui_style_t* style, boole important);
GUI_API boole scene__update_style(uint64 handle, const gui_style_t* style);
GUI_API int64 scene__style_count(void);
GUI_API int64 scene__style_capacity(void);

GUI_API void scene__clear_styles(void);

// ===== symbol resolver hook ================================================
// handler resolution falls back through this hook when the registration
// table misses, so that handlers exported by the host exe (UI_HANDLER)
// can be discovered by name without a per-handler scene__register_handler
// call. the resolver is platform-specific (Win32 wires
// GetProcAddress(GetModuleHandleW(NULL), name) during platform_win32__init);
// scene.c never sees windows.h.

typedef gui_handler_fn (*gui_symbol_resolver_fn)(char* name);

GUI_API void scene__set_symbol_resolver(gui_symbol_resolver_fn resolver);

#endif
