#ifndef SCROLL_H
#define SCROLL_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// scroll.h - shared scrollbar mechanics for any scrollable container.
// Used by widget_div.c (a styled scrollable block) and widget_window.c
// (the whole-page scrollbar). Both widgets:
// - Read overflow_x / overflow_y from gui_style_t.
// - Lay out their children with a -scroll_y offset.
// - Need a draggable scrollbar drawn on the right edge.
// - Need to clip children to their bounds (renderer scissor).
// All the logic lives here so adding a third scrollable container kind
// is a copy-paste of the call sequence, not a copy-paste of the math.
// USAGE PATTERN (widget side):
// In layout(): after computing the node's bounds and content extent,
// parent->content_h = ...measured...;
// scroll__clamp(parent);
// In emit_draws(): bracket children with scissor + bg + push:
// if (scroll__vbar_visible(node))
// renderer__push_scissor(node->bounds);
// ...submit bg...
// In emit_draws_post(): scrollbar on top of children, then pop:
// if (scroll__vbar_visible(node)) {
// scroll__draw_vbar(node, scale);
// renderer__pop_scissor();
// }
// In on_mouse_down/drag/up:
// scroll__on_mouse_down(node, x, y, scale);
// scroll__on_mouse_drag(node, y, scale);  // returns TRUE if consumed
// scroll__on_mouse_up(node);

GUI_API boole scroll__vbar_visible(gui_node_t* node);
// Opacity for one direct child subtree of a vertical scroll-fade container.
GUI_API float scroll__child_fade_alpha(gui_node_t* node, float scale);

GUI_API boole scroll__hbar_visible(gui_node_t* node);

GUI_API float scroll__bar_size(gui_node_t* node, float scale);

GUI_API void  scroll__clamp(gui_node_t* node);

GUI_API void  scroll__draw_vbar(gui_node_t* node, float scale);

GUI_API void  scroll__draw_hbar(gui_node_t* node, float scale);

GUI_API void  scroll__on_mouse_down(gui_node_t* node, int64 x, int64 y, float scale);

GUI_API boole scroll__on_mouse_drag(gui_node_t* node, int64 y, float scale);

GUI_API void  scroll__on_mouse_up(gui_node_t* node);
// Rubber-band overscroll (overscroll-behavior: auto on the container):
// enabled_y tells whether the vertical rubber band is on; limit_y is the
// farthest the content can be pulled past an edge in device pixels;
// push_y pulls it by a movement of excess device pixels (negative past
// the start, positive past the end) with the diminishing resistance;
// release_y lets go: the spring back to the edge starts.
GUI_API boole scroll__overscroll_enabled_y(gui_node_t* node);
GUI_API float scroll__overscroll_limit_y(gui_node_t* node);
GUI_API void  scroll__overscroll_push_y(gui_node_t* node, float excess);
GUI_API void  scroll__overscroll_release_y(gui_node_t* node);

// A scrollbar for a view that is not the node's own box (the open list of
// a select). view is the visible rectangle, content_h the height of all
// the content, scroll_y how far it is scrolled, all in device pixels. The
// bar takes the node's scrollbar-size, scrollbar-radius, scrollbar-track
// and scrollbar-thumb. thumb_in returns the thumb's rectangle (the hit
// test and the drag use it), draw_vbar_in draws the track and the thumb
// with their alpha multiplied by alpha, scroll_per_pixel_in is how far the
// content moves for one pixel of thumb travel.
GUI_API gui_rect_t scroll__vbar_thumb_in(gui_node_t* node, gui_rect_t view, float content_h, float scroll_y, float scale);
GUI_API void       scroll__draw_vbar_in(gui_node_t* node, gui_rect_t view, float content_h, float scroll_y, float alpha, float scale);
GUI_API float      scroll__scroll_per_pixel_in(gui_node_t* node, gui_rect_t view, float content_h, float scale);

// scroll-end-effect: wobble. wobble_enabled: the style asks for it with an
// amount above zero. animated: the position moves to its target over
// time (scroll-smooth, or the wobble), so input writes the target only.
// wobble_step advances the spring by ms_delta: position and velocity
// (device pixels, device pixels per millisecond) move toward target;
// TRUE while the spring has not come to rest.
GUI_API boole scroll__wobble_enabled(const gui_style_t* style);
GUI_API boole scroll__animated(const gui_style_t* style);
/**
 * Advances smooth scrolling with frame-rate-independent exponential decay.
 * Long pauses advance at most 100 ms; a subpixel remainder settles exactly.
 *
 * @function scroll__smooth_step
 * @param float* position -> Current displayed offset, updated in place.
 * @param float target -> Destination offset in pixels.
 * @param float response_ms -> Scroll response time; zero disables smoothing.
 * @param float ms_delta -> Elapsed frame time in milliseconds.
 * @return boole -> TRUE while the offset is still approaching its target.
 */
GUI_API boole scroll__smooth_step(float* position, float target, float response_ms, float ms_delta);
GUI_API boole scroll__wobble_step(const gui_style_t* style, float* position, float* velocity, float target, float ms_delta);

#endif
