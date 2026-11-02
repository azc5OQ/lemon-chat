#ifndef MEEK_UI_ATLAS_H
#define MEEK_UI_ATLAS_H

// atlas.h - GPU texture-atlas allocator.
// Purpose: pack many small RGBA8 images (app icons, mostly) into a
// few large GL textures so the renderer can batch consecutive
// submit_image calls into one draw call per atlas, instead of one
// draw call per icon. On a tile-based mobile GPU each glBindTexture
// is hundreds of microseconds of state-change overhead, so cutting
// 150 binds/frame down to 1-2 binds/frame is a 100x speedup on the
// draws phase of the loop.
// Layout: each page is a 2048x2048 RGBA8 GL_TEXTURE_2D. Allocations
// use simple shelf packing -- maintain a list of horizontal strips
// ("shelves") with a height and a current x-cursor. A new
// allocation slots into the first shelf with enough remaining
// width and height-tolerance; if no shelf fits, open a new shelf
// at the next free Y. When a page can't fit a new shelf, mark it
// full and start a new page. Pages allocate on demand up to
// ATLAS_MAX_PAGES.
// Padding: every allocated region is internally bordered by 2px
// of transparent margin so bilinear filtering at the region's
// edge doesn't sample neighbor pixels. The returned uv_rect refers
// to the INSIDE of the padding -- callers don't see the border.
// Lifecycle: __init() before first __alloc() (renderer must have
// a current GL context). __shutdown() destroys all GL textures.
// __init() is idempotent.
// Failure: if all pages are full AND a new page can't be allocated
// (out of ATLAS_MAX_PAGES or glGenTextures fails), __alloc returns
// FALSE. Caller (widget_image) falls back to a per-image texture
// outside the atlas so rendering still works -- the only cost is
// a non-batched draw call for that one image. Graceful degrade,
// not a hard fail.

#include "types.h"
#include "gui.h"
#include "gui_api.h"

// 2048 was chosen as a sweet spot: holds ~144 160x160 icons per
// page (12 shelves x 12 icons), fits in 16 MB of GPU memory,
// well within max-texture-size limits on every shipping mobile
// GPU (the Adreno 630 in the Poco F1 supports 16384x16384). Bump
// to 4096 later if profiling shows we're hitting the per-page
// allocator overhead with many pages active.
#define ATLAS_PAGE_SIZE  2048

// Hard cap on simultaneous pages. 16 pages * 16 MB = 256 MB of
// GPU memory -- absurdly more than any realistic icon catalog
// will need (16 * 144 = 2304 icons). This is the "graceful
// degrade" boundary: beyond it, atlas__alloc returns FALSE and
// callers use the slow per-texture path.
#define ATLAS_MAX_PAGES  16

// One-time init. Clears internal page list. Safe to call multiple
// times (subsequent calls are no-ops). GL context must be current.
GUI_API void atlas__init(void);

// Destroy all atlas pages, free their GL textures, reset state.
// Call from renderer shutdown.
GUI_API void atlas__shutdown(void);

// Allocate a (w x h) region. On success writes the page index to
// *out_page and the INSIDE pixel rect (excluding padding) to
// *out_rect_px, then returns TRUE. On failure returns FALSE; the
// out parameters are undefined.
// Sizes larger than the page (minus padding) cannot fit -- those
// always return FALSE so the caller can pick a non-atlas path.
GUI_API boole atlas__alloc(int w, int h, int* out_page, gui_rect_t* out_rect_px);

// Upload RGBA8 pixels into a previously-allocated region. `pixels`
// is tightly-packed (pitch = w * 4 bytes). `rect_px` MUST be a
// rect previously returned by __alloc -- the function does not
// validate that, callers are expected to be honest.
GUI_API void atlas__upload(int page, gui_rect_t rect_px, const unsigned char* pixels);

// Get the opaque GL texture handle for a page. Cast to
// (uintptr_t) and then to GLuint at the call site to avoid
// dragging GL headers into every consumer.
GUI_API void* atlas__get_texture(int page);

// Convert a pixel rect (from __alloc) into normalized [0..1] UVs.
// Convenience for renderers building textured-quad verts.
GUI_API void atlas__rect_to_uv(gui_rect_t rect_px, float* u0, float* v0, float* u1, float* v1);

#endif
