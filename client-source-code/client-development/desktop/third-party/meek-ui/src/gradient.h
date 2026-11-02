// gradient.h - CSS gradients. A style carries the description
// (gui_gradient_t in gui.h: linear with an angle or a corner, radial
// with the CSS shape and size rules, color stops with positions); this
// module turns it into triangles with exact per-vertex colors that every
// renderer pushes through its rounded-rect pipeline, so the six backends
// share one gradient implementation and stay pixel-identical. The
// rounded-corner mask and the anti-aliased edge come from the rect
// shader as for a solid rect: the triangles may extend past the rect,
// the SDF mask clips them.
//
// Color stops interpolate in premultiplied sRGB, as browsers do for
// hex / rgb() colors, so a fade to `transparent` keeps its hue instead
// of darkening. The GPU only interpolates between neighbouring vertices;
// bands and rings are subdivided so that its straight interpolation
// stays within a channel value of the premultiplied curve.
#ifndef GRADIENT_H
#define GRADIENT_H

#include "meek_ui_config.h"
#include "types.h"
#include "gui.h"

// one vertex of a tessellated gradient: a device-pixel position and the
// gradient color there
typedef struct gradient_vertex_t
{
    float       x;   // 0x0
    float       y;   // 0x4
    gui_color_t c;   // 0x8
} gradient_vertex_t;

// the most vertices gradient__tessellate writes for one rect (a radial
// gradient at full resolution: 40 rings of 64 segments); a renderer
// sizes its scratch buffer with it
#define GRADIENT_MAX_VERTS 16384

/**
 * @brief fills a two-stop linear gradient from the old direction keywords: vertical is `to bottom`, horizontal `to right`, diagonal-tl `to bottom right`, diagonal-tr `to bottom left`. Hosts that set gradients from C use it; the renderers' renderer__submit_rect_gradient goes through it too.
 *
 * @param gui_gradient_t* out -> the gradient to fill
 * @param gui_color_t from -> the color at the start
 * @param gui_color_t to -> the color at the end
 * @param gui_gradient_dir_e dir -> one of the four directions
 *
 * @return void
 */
GUI_API void gradient__two_color(gui_gradient_t* out, gui_color_t from, gui_color_t to, gui_gradient_dir_e dir);

/**
 * @brief interpolates between two gradients of the same shape (kind, stop count, corner and size keywords): every stop color and position, the angle, the center and the radii; gradients of different shapes switch over at the halfway point. The animator uses it for transitions.
 *
 * @param gui_gradient_t* out -> the result
 * @param const gui_gradient_t* a -> the gradient at t = 0
 * @param const gui_gradient_t* b -> the gradient at t = 1
 * @param float t -> 0..1
 *
 * @return void
 */
GUI_API void gradient__lerp(gui_gradient_t* out, const gui_gradient_t* a, const gui_gradient_t* b, float t);

/**
 * @brief tessellates a gradient over a rect into triangles with exact colors. Linear gradients become bands perpendicular to the gradient line, radial ones rings around the center; both extend past the rect and rely on the rect shader's mask. Pixel lengths in the gradient (stop positions, radii, the center) are logical and get multiplied by `scale`.
 *
 * @param const gui_gradient_t* g -> the gradient
 * @param gui_rect_t r -> the rect in device pixels
 * @param float scale -> the UI scale (logical to device pixels)
 * @param float opacity -> multiplies every stop's alpha (the node's effective opacity)
 * @param gradient_vertex_t* out -> receives the vertices, three per triangle
 * @param int64 cap -> the buffer's capacity in vertices
 *
 * @return int64 -> the number of vertices written; 0 for an empty rect or a gradient without stops
 */
GUI_API int64 gradient__tessellate(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap);

// Prepared once per layer; sampling does not repeat stop fixup or geometry.
typedef struct gradient_sampler_t
{
    gui_gradient_kind_e kind;                  // 0x0
    boole repeating;                           // 0x4
    int count;                                 // 0x8
    float cx;                                  // 0xC
    float cy;                                  // 0x10
    float dx;                                  // 0x14
    float dy;                                  // 0x18
    float line;                                // 0x1C
    float rx;                                  // 0x20
    float ry;                                  // 0x24
    float angle;                               // 0x28
    float pos[GUI_GRADIENT_MAX_STOPS];          // 0x2C
    gui_color_t color[GUI_GRADIENT_MAX_STOPS];   // 0x6C
    boole premultiplied; // 0x16C: prepared CPU ramps may retain premultiplied stop colors
} gradient_sampler_t;

GUI_API void gradient__prepare(const gui_gradient_t* g, float width, float height, float scale, gradient_sampler_t* out);
GUI_API gui_color_t gradient__sample(const gradient_sampler_t* sampler, float x, float y);
// Shade at an already evaluated geometric parameter (wrapped turns for conics).
GUI_API gui_color_t gradient__sample_parameter(const gradient_sampler_t* sampler, float t);

#endif
