#ifndef RENDERER_BACKEND_H
#define RENDERER_BACKEND_H

#include "renderer.h"

GUI_API void  renderer_backend__shutdown(void);
GUI_API void  renderer_backend__begin_frame(int64 viewport_w, int64 viewport_h, gui_color_t clear);
GUI_API void  renderer_backend__submit_rect(gui_rect_t r, gui_color_t c, float radius);
GUI_API void renderer_backend__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color);
GUI_API void  renderer_backend__end_frame(void);
// Standalone presentation already limits the frame rate; platform gating must not wait twice.
GUI_API boole renderer_backend__presents_with_vsync(void);
// Backends may use the portable painter for their complete drawing contract.
GUI_API boole renderer_backend__needs_compositor(void);
GUI_API void  renderer_backend__flush_pending_draws(void);
GUI_API void* renderer_backend__create_atlas_r8(const ubyte* pixels, int width, int height);
GUI_API void  renderer_backend__destroy_atlas(void* atlas);
GUI_API void  renderer_backend__set_text_atlas(void* atlas);
GUI_API void  renderer_backend__submit_text_glyph(gui_rect_t rect, gui_rect_t uv, gui_color_t color);
GUI_API void renderer_backend__submit_shadow(gui_rect_t rect, gui_color_t c, float radius, float blur);
GUI_API void renderer_backend__submit_rect_gradient(gui_rect_t r, gui_color_t from, gui_color_t to, int direction, float radius);
GUI_API void renderer_backend__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius);
GUI_API void* renderer_backend__create_texture_rgba(const ubyte* rgba, int width, int height);
// Private upload for the final compositor surface, drawn 1:1 in device pixels.
// One mip level; ownership and destruction match create_texture_rgba.
GUI_API void* renderer_backend__create_frame_texture_rgba(const ubyte* rgba, int width, int height);
GUI_API void  renderer_backend__destroy_texture(void* tex);
GUI_API void  renderer_backend__submit_image(gui_rect_t rect, void* tex, gui_color_t tint);
GUI_API void  renderer_backend__submit_image_uv(gui_rect_t rect, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1);
GUI_API void renderer_backend__push_scissor(gui_rect_t rect);
GUI_API void renderer_backend__pop_scissor(void);
GUI_API void renderer_backend__blur_region(gui_rect_t rect, float sigma_px);

// Layer mode (src/compositor.c, session/LAYERS-DESIGN.md). A backend that
// implements these three draws the frame itself while effect groups are
// rasterized on the CPU; one that returns FALSE from supports_layers keeps
// the whole-frame compositor.
GUI_API boole renderer_backend__supports_layers(void);
// One level, premultiplied RGBA8, linear filtering, clamp to edge. Destroyed
// with renderer_backend__destroy_texture. The pixels may be NULL (uploaded
// later with the update call).
GUI_API void* renderer_backend__create_layer_texture(const ubyte* premultiplied_rgba, int width, int height);
// Refreshes a layer texture of the same size in place (a changing layer keeps
// its texture across frames); FALSE when the size differs.
GUI_API boole renderer_backend__update_layer_texture(void* texture, const ubyte* premultiplied_rgba, int width, int height);
// Draws the whole texture over a quad. corners: 4 x (x, y, w) in device
// pixels, top-left, top-right, bottom-right, bottom-left, x and y already
// multiplied by w (the vertex position is (x, y, 0, w), so a perspective
// transform interpolates correctly; w = 1 for affine). opacity scales all
// four channels. blend_mode 0 = normal (ONE, INV_SRC_ALPHA), 1 = screen
// (ONE, INV_SRC_COLOR; alpha ONE, INV_SRC_ALPHA), 2 = replace (ONE, ZERO:
// writes the texel, alpha included; used to write a composed read-back
// region back), 3 = multiply over an opaque backdrop (DEST_COLOR,
// INV_SRC_ALPHA; alpha ONE, INV_SRC_ALPHA). An integer-aligned identity quad of the texture's size is
// point-sampled so it fetches exact texels. Flushes pending batches first
// (submit order is draw order) and honours the scissor.
GUI_API void  renderer_backend__submit_layer(void* texture, const float* corners, float opacity, int blend_mode);
// Flushes pending draws and reads the back buffer region as RGBA8, top-left
// origin, tightly packed (w * h * 4 bytes). The bytes are premultiplied by
// construction of the blend states (rgb = sum of alpha * color); on an
// opaque window alpha is 255. FALSE when unsupported or the region leaves
// the viewport.
GUI_API boole renderer_backend__read_region(int x, int y, int w, int h, ubyte* out_rgba);
// Optional GPU backdrop filter: three horizontal/vertical box pairs, clamped
// to this rectangle, with (sum + radius) / (2 * radius + 1) byte rounding after
// EACH axis. Coordinates are already clipped device pixels, radius is 1..256.
// Preserve the draw target, viewport and scissor. FALSE leaves its pixels
// untouched so the compositor can use the CPU reference. No CPU readback.
GUI_API boole renderer_backend__blur_layer(int x, int y, int w, int h, int radius);
// Render-to-layer: between begin_layer and end_layer every submit (rects,
// gradients, text, images, scissors, in layer coordinates with the origin
// at the layer's top-left) draws into a pooled, transparent-cleared
// premultiplied render target of the given size instead of the frame;
// end_layer returns its texture for renderer_backend__submit_layer. The
// texture belongs to the backend's pool (never destroyed by the caller)
// and stays valid until the next begin_layer of the same size slot. Layers
// do not nest. FALSE from begin_layer means the caller rasterizes on the CPU.
GUI_API boole renderer_backend__begin_layer(int width, int height);
GUI_API void* renderer_backend__end_layer(void);
// A background layer drawn as a grid of tiles (layer mode: the tiled and
// repeating gradients of a plain node cost no CPU raster). tile is the
// first tile in device pixels; columns x rows copies follow to the right
// and downwards, stepping by the tile size. Every tile is clipped to box
// with its corner radius (the rounded-box mask of the rect pipeline, with
// the local coordinate taken relative to box), so a tile crossing the box
// edge is cut there. gradient NULL draws solid tiles of color. opacity
// multiplies every vertex alpha. A full vertex batch is flushed and the
// draw continues, in submit order.
GUI_API void  renderer_backend__submit_tiles(gui_rect_t box, float radius, gui_rect_t tile, int columns, int rows, const gui_gradient_t* gradient, gui_color_t color, float scale, float opacity);

#endif
