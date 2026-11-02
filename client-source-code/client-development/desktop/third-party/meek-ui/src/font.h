#ifndef FONT_H
#define FONT_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// font.h - TTF font loading, rasterization, and text drawing.
// built on stb_truetype (in gui/src/third_party/). cross-platform:
// the same code runs on windows today and will run on linux when we
// port. the only platform-specific piece is where TTF bytes come from
// (a path on disk via fs__read_entire_file, or a byte buffer already
// in memory), which we abstract with two registration entry points.
// USAGE:
// // once at startup:
// font__init();
// font__register_from_file("Inter", "fonts/Inter.ttf");
// // or, when fonts are embedded into the binary at build time:
// // font__register_from_memory("Inter", inter_bytes, inter_size);
// // whenever we need to draw text:
// gui_font_t* f = font__at("Inter", 14.0f);
// font__draw(f, x, y, color, "Hello");
// // at shutdown:
// font__shutdown();
// font__at lazily rasterizes + atlas-packs the (family, size) pair the
// first time it's requested, then caches. the atlas gets uploaded to
// the renderer via renderer__create_atlas_r8 and released via
// renderer__destroy_atlas on shutdown.
// QUALITY NOTES (imgui-style):
// we use stb_truetype's packer API with 3x horizontal oversampling.
// that means each glyph is rasterized 3x wider than final size and
// sampled with LINEAR filtering at draw time -- gives crisp subpixel
// positioning without having to pre-render multiple versions per
// glyph. stb_rect_pack does the atlas packing itself.

// opaque font handle returned by font__at. holds everything needed to
// measure and draw text at a specific (family, size) -- the rasterized
// atlas pixels, the per-codepoint glyph table, the renderer texture
// handle.
typedef struct gui_font_t gui_font_t;

// Scene traversal scopes inherited text effects without changing widget APIs.
gui_node_t* font__set_node(gui_node_t* node);

// per-codepoint glyph info returned by font__glyph. coordinates are
// in pixels at the font's rasterization size. UVs are 0..1 into the
// atlas texture.
typedef struct gui_glyph_t
{
    // how far to advance the pen after drawing this glyph.
    float advance;    // 0x0
    // bearing left: x offset from pen to the glyph's left edge.
    float offset_x;   // 0x4
    // bearing top: y offset from pen's baseline to the glyph's top edge (typically negative).
    float offset_y;   // 0x8
    // glyph quad width in pixels.
    float width;      // 0xC
    // glyph quad height in pixels.
    float height;     // 0x10
    // atlas UV of top-left corner.
    float uv_x0;      // 0x14
    float uv_y0;      // 0x18
    // atlas UV of bottom-right corner.
    float uv_x1;      // 0x1C
    float uv_y1;      // 0x20
} gui_glyph_t;

// ============================================================================
// lifecycle
// ============================================================================

GUI_API boole font__init(void);

GUI_API void  font__shutdown(void);

GUI_API void  font__drop_gpu_cache(void);

// ============================================================================
// family registration
// ============================================================================

// Re-registering a family releases its old rasterizations. Obtain fresh font
// handles with font__at afterwards; register between frames on the drawing thread.

GUI_API boole font__register_from_memory(char* family, const ubyte* ttf_bytes, int64 ttf_size);
// Copy temporary bytes into tracked storage owned by the font subsystem.
GUI_API boole font__register_from_memory_copy(char* family, const ubyte* ttf_bytes, int64 ttf_size);

GUI_API boole font__register_from_file(char* family, char* path);

GUI_API boole font__register_dir(char* dir_path);

// ============================================================================
// query
// ============================================================================

GUI_API gui_font_t* font__at(char* family, float size_px);

// Optional loading-time preparation for retained pages. Resolve styles first
// and call on the drawing thread with a live renderer. Includes inactive pages;
// creates their font atlases without laying out, animating or painting them.
// Atlases use the ordinary bounded font cache. Repeat after a DPI/font change
// if its first-use cost should be paid before the next page switch.
GUI_API void font__prewarm_tree(gui_node_t* root, float scale);

GUI_API boole     font__glyph(gui_font_t* f, uint codepoint, gui_glyph_t* out);

GUI_API float     font__measure(gui_font_t* f, char* utf8);

GUI_API float     font__measure_n(gui_font_t* f, char* utf8, int64 n);

GUI_API float     font__line_height(gui_font_t* f);

GUI_API float     font__ascent(gui_font_t* f);

GUI_API float     font__descent(gui_font_t* f);

GUI_API float     font__line_height_for(gui_font_t* f, const gui_style_t* s, float scale);

GUI_API float     font__baseline_for(gui_font_t* f, const gui_style_t* s, float scale);

GUI_API float     font__kern(gui_font_t* f, uint prev_cp, uint cp);

GUI_API void*     font__atlas_tex(gui_font_t* f);

// ============================================================================
// drawing
// ============================================================================

GUI_API void font__draw(gui_font_t* f, float x, float y, gui_color_t color, char* utf8);

GUI_API void font__draw_n(gui_font_t* f, float x, float y, gui_color_t color, char* utf8, int64 n);

// Draw with normal metrics, skipping glyph submissions outside the clip.
GUI_API void font__draw_n_clipped(gui_font_t* f, float x, float y, gui_color_t color, char* utf8, int64 n, gui_rect_t clip);
// Nearest UTF-8 byte boundary to a horizontal offset, in one forward walk.
GUI_API int64 font__text_position(gui_font_t* f, const char* utf8, int64 length, float target_x);

#endif
