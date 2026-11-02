#ifndef RENDERER_H
#define RENDERER_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// Backend implementations are wrapped by the shared effect compositor.
#ifdef MEEK_RENDERER_BACKEND
#define renderer__shutdown renderer_backend__shutdown
#define renderer__begin_frame renderer_backend__begin_frame
#define renderer__end_frame renderer_backend__end_frame
#define renderer__flush_pending_draws renderer_backend__flush_pending_draws
#define renderer__submit_rect renderer_backend__submit_rect
#define renderer__submit_triangle renderer_backend__submit_triangle
#define renderer__submit_shadow renderer_backend__submit_shadow
#define renderer__submit_rect_gradient renderer_backend__submit_rect_gradient
#define renderer__submit_gradient renderer_backend__submit_gradient
#define renderer__create_atlas_r8 renderer_backend__create_atlas_r8
#define renderer__destroy_atlas renderer_backend__destroy_atlas
#define renderer__set_text_atlas renderer_backend__set_text_atlas
#define renderer__submit_text_glyph renderer_backend__submit_text_glyph
#define renderer__create_texture_rgba renderer_backend__create_texture_rgba
#define renderer__destroy_texture renderer_backend__destroy_texture
#define renderer__submit_image renderer_backend__submit_image
#define renderer__submit_image_uv renderer_backend__submit_image_uv
#define renderer__push_scissor renderer_backend__push_scissor
#define renderer__pop_scissor renderer_backend__pop_scissor
#define renderer__blur_region renderer_backend__blur_region
#endif

// ============================================================================
// renderer.h - abstract renderer backend interface.
// ============================================================================
// the toolkit targets multiple GPU APIs via interchangeable backends, each
// implementing this contract:
// renderers/opengl3_renderer.c   - OpenGL 3.3 core (working default).
// renderers/d3d11_renderer.c     - Direct3D 11 (scaffold).
// renderers/d3d9_renderer.c      - Direct3D 9 (scaffold).
// exactly one backend is compiled into gui.dll at a time, selected at
// configure time via the GUI_RENDERER CMake cache variable. platform_win32
// creates the window, then hands the native HWND to the chosen renderer's
// renderer__init() which creates whatever graphics context it needs.
// ============================================================================
// VISUAL CONTRACT -- binding on every backend.
// ============================================================================
// all backends MUST produce visually indistinguishable output (modulo at most
// one subpixel of anti-aliasing rounding) given the same sequence of
// renderer__submit_rect calls. swapping the backend must be invisible to the
// host app and the end user. the rules that enforce this:
// COORDINATE SPACE
// - input rects are in TOP-LEFT-ORIGIN pixel coordinates: (0,0) is the
// top-left pixel of the window, x grows right, y grows down.
// - backends convert to their native NDC inside their vertex shader.
// OpenGL's NDC is y-up with bottom-left origin -> the GL vertex
// shader does a y-flip. D3D10+'s NDC is y-up with top-left origin ->
// different conversion, same visual result. D3D9 needs an additional
// half-pixel adjustment for its integer-pixel-center rasterization
// rules.
// COLOR SPACE
// - gui_color_t components are in LINEAR 0..1. no sRGB conversion done
// by the library; we write linear values into a non-sRGB back buffer
// so "what the .style file says" is "what lands on screen" byte-wise.
// if we ever enable sRGB framebuffers, every backend enables it the
// same way and we document the switch once.
// ALPHA BLENDING
// - every backend configures SEPARATE blend factors for color and alpha:
// RGB:    final.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)
// ALPHA:  final.a   = src.a   * 1     + dst.a   * (1 - src.a)
// The RGB equation is classic alpha blending. The ALPHA equation
// is what KEEPS the framebuffer's alpha channel at 1.0 across
// overlapping draws -- a critical detail because:
// * The Windows DWM compositor (opengl3) treats partial
// framebuffer alpha as window transparency, letting the
// desktop bleed through. Without separate-alpha blending,
// our own alpha animations make the WHOLE WINDOW flicker.
// * Some Android compositors multiply the swap chain by
// framebuffer alpha at present time. Same flicker.
// * D3D11/D3D9 swap chains default to ignoring framebuffer
// alpha but the DXGI desktop window manager doesn't, so
// we keep the same separate-alpha rule on those backends
// for the VISUAL CONTRACT to hold byte-for-byte.
// Per backend:
// OpenGL 3.3 / GLES 3.0:
// glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
// GL_ONE,       GL_ONE_MINUS_SRC_ALPHA)
// D3D11: SrcBlend=SRC_ALPHA,  DestBlend=INV_SRC_ALPHA,
// SrcBlendAlpha=ONE,   DestBlendAlpha=INV_SRC_ALPHA
// D3D9:  D3DRS_SRCBLEND=SRCALPHA,        D3DRS_DESTBLEND=INVSRCALPHA,
// D3DRS_SEPARATEALPHABLENDENABLE=TRUE,
// D3DRS_SRCBLENDALPHA=ONE,        D3DRS_DESTBLENDALPHA=INVSRCALPHA
// No color/alpha-channel write masking (RenderTargetWriteMask = ALL).
// ROUNDED RECTANGLE SHAPE
// - every backend uses the same signed-distance-function:
// float sd_round_box(vec2 p, vec2 b, float r) {
// vec2 q = abs(p) - b + vec2(r);
// return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
// }
// with p = local_pixel - rect_size/2, b = rect_size/2, r = radius.
// - edge antialiasing: alpha = 1 - smoothstep(-0.5, 0.5, d). that
// specific range gives a 1-pixel-wide AA band centered on the
// mathematical edge.
// - pixels with alpha <= 0 MUST be discarded so the rect's rectangular
// bounding box doesn't occlude what's behind the rounded corners.
// RADIUS CLAMPING
// - radius is clamped to [0, min(rect_w, rect_h) * 0.5] before being
// passed to the SDF. the CPU-side clamp in each backend's
// renderer__submit_rect must be identical.
// VERTEX TOPOLOGY
// - each rect is two triangles forming a quad: 6 vertices per rect,
// no index buffer. ordering:
// tri 1: (x0,y0) (x1,y0) (x0,y1)      -- top-left, top-right, bottom-left
// tri 2: (x1,y0) (x1,y1) (x0,y1)      -- top-right, bottom-right, bottom-left
// - backends with different winding-order defaults MUST disable face
// culling to render both sides (or arrange winding themselves).
// a new backend passes validation when, given the same .ui + .style + scale
// and the same window size, its framebuffer matches the OpenGL reference to
// within one subpixel of anti-aliasing rounding. color readback + diff is
// the practical way to verify during development.

GUI_API boole renderer__init(void* native_window);

// Shared diagnostics: yellow top-left frames-per-second counter, off by default.
// Measures completed render frames against real monotonic time, never animation
// time or event-loop iterations. Works without a scene or an initialized font.
enum { GUI_RENDERER_DEBUG__FPS = 1u << 0 };
GUI_API void renderer__set_debug_flags(uint flags);
GUI_API uint renderer__debug_flags(void);
// Removes --fps / --no-fps from argc/argv, preserving other arguments. Standard
// main trampolines call this automatically. Hosts owning main can call it before
// their own argument parser, or use the C flag above. MEEK_UI_FPS=1 is also opt-in.
GUI_API void renderer__parse_launch_flags(int* argc, char** argv);

// Optional effect computation, independent of the presentation backend.
// Select before platform__init or an embedded adapter's attach call, on the
// drawing thread.
//   AUTO (default): effect surfaces are rasterized on the CPU. A backdrop
//     blur drawn on the frame itself (the select popup's popup-blur, the
//     blur: property, backdrop-filter at depth 0) runs on the renderer's own
//     device where the renderer implements renderer_backend__blur_layer
//     (Direct3D 9, Direct3D 11, OpenGL 3), and on the CPU elsewhere or when
//     the renderer refuses the draw target.
//   CPU: everything on the CPU, that backdrop blur included. The checks
//     compare against this mode.
//   GPU: AUTO, plus the supported native renderer effects and, on Windows,
//     the separate D3D11 compute device for CPU surfaces.
// Unsupported effects fall back to the CPU compositor. Embedded adapters
// restore the host state changed by native effects when the UI frame ends.
typedef enum gui_effect_acceleration_e
{
    GUI_EFFECT_ACCELERATION__CPU = 0,
    GUI_EFFECT_ACCELERATION__GPU = 1,
    GUI_EFFECT_ACCELERATION__D3D11_COMPUTE = GUI_EFFECT_ACCELERATION__GPU, // Legacy spelling.
    GUI_EFFECT_ACCELERATION__AUTO = 2
} gui_effect_acceleration_e;

GUI_API void renderer__set_effect_acceleration(gui_effect_acceleration_e mode);
// Diagnostic: the backdrop blurs since the start, by what filtered them. gpu
// counts the ones the renderer's own filter did on its device, cpu the ones
// that were read back, filtered on the CPU and uploaded (or filtered inside
// the frame compositor's surface). Either pointer may be NULL.
GUI_API void renderer__backdrop_blur_counts(uint64* gpu, uint64* cpu);

// How an extended stylesheet (CSS effects) is drawn. LAYERS: the backend
// draws the frame and only effect groups are rasterized on the CPU, each
// drawn as one textured quad. FRAME: the whole frame is rasterized on the
// CPU and drawn as one quad (the portable reference; the same bytes on
// every backend). AUTO (default) is LAYERS on a backend that implements
// it and FRAME elsewhere. Select before the first frame; the launch
// flags --frame-compositor / --layer-compositor and MEEK_UI_FRAME_COMPOSITOR=1
// do the same.
typedef enum gui_compositor_mode_e
{
    GUI_COMPOSITOR_MODE__AUTO = 0,
    GUI_COMPOSITOR_MODE__FRAME = 1,
    GUI_COMPOSITOR_MODE__LAYERS = 2
} gui_compositor_mode_e;

GUI_API void renderer__set_compositor_mode(gui_compositor_mode_e mode);
GUI_API gui_compositor_mode_e renderer__compositor_mode(void);

GUI_API void  renderer__shutdown(void);

GUI_API void  renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color_t clear);

GUI_API void  renderer__submit_rect(gui_rect_t r, gui_color_t c, float radius);

// Device-pixel coordinates, straight RGBA, current scissor and paint order.
// Triangles are filled (either winding); lines are thick, butt-capped quads.
// Triangle/line edges use pixel-center coverage. Circles use the rect SDF AA.
// Nonpositive radius/width and degenerate triangles/lines draw nothing.
GUI_API void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color);
GUI_API void renderer__submit_line(gui_vec2_t from, gui_vec2_t to, float width, gui_color_t color);
GUI_API void renderer__submit_circle(gui_vec2_t center, float radius, gui_color_t color);

GUI_API void  renderer__end_frame(void);

GUI_API void  renderer__flush_pending_draws(void);

// ============================================================================
// TEXT PIPELINE
// ============================================================================
// the font module (font.c) owns TTF parsing + rasterization + atlas
// layout. the renderer just provides GPU storage for the atlas bitmap
// and a second draw pass for textured glyph quads.
// each backend implements four text entry points below + a shader that
// samples the bound atlas with LINEAR filtering (imgui-quality antialiased
// glyphs). the atlas is R8 (alpha-only); the shader multiplies the
// sampled alpha by the per-vertex color.
// each glyph's "color" is the text color (from the style) -- the atlas
// provides only alpha/coverage. the rect is in pixel coords (top-left
// origin, same as solid rects). uv is 0..1 into the atlas.

GUI_API void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height);

GUI_API void  renderer__destroy_atlas(void* atlas);

GUI_API void  renderer__set_text_atlas(void* atlas);

GUI_API void  renderer__submit_text_glyph(gui_rect_t rect, gui_rect_t uv, gui_color_t color);

// ============================================================================
// SHADOW + GRADIENT PIPELINE (extensions of the solid-rect pass)
// ============================================================================
// Shadow: one SDF rounded-box splat, offset by (dx, dy) from the shape
// it's decorating, with a `blur`-wide soft edge. Submit BEFORE the bg
// rect so z-order is shadow-under-bg.
// Gradient: same rect + radius + SDF AA as submit_rect. The shared
// tessellator (src/gradient.c) turns a gui_gradient_t (linear with an
// angle or a corner, radial with the CSS shape rules, color stops) into
// triangles with exact per-vertex colors; the renderer pushes them
// through its rect pipeline, so all six backends paint the same pixels.
// renderer__submit_rect_gradient is the two-color entry point kept for
// callers with a direction keyword (the color picker, image placeholders).
// Every backend implements both.

GUI_API void renderer__submit_shadow(gui_rect_t rect, gui_color_t c, float radius, float blur);

GUI_API void renderer__submit_rect_gradient(gui_rect_t r, gui_color_t from, gui_color_t to, int direction, float radius);

GUI_API void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius);

// ============================================================================
// IMAGE PIPELINE (RGBA textures)
// ============================================================================
// Separate from the text pipeline because images sample ALL FOUR channels
// of their texture while text atlases are R8 (coverage only). Same quad-
// based draw shape, different shader.
// Usage pattern (widget_image):
// 1. decode file bytes (stb_image) to RGBA8.
// 2. void* tex = renderer__create_texture_rgba(rgba, w, h); -- once.
// 3. renderer__submit_image(rect, tex, tint); -- per frame.
// 4. renderer__destroy_texture(tex); -- on widget destroy.
// Images flush any pending rect/text batch on submit so draw order is
// preserved (image on top of a previously-submitted rect, underneath a
// subsequently-submitted text glyph, etc.). One draw call per image is
// fine -- UI scenes typically have <10 images, not hundreds of glyphs.
// Every backend uploads the same CPU-built mip chain (backends/common/_mip_cpu.h)
// and samples it trilinearly, so a downscaled image looks the same on
// each of them.
// Stubs on backends without an image implementation yet return NULL
// from create_texture_rgba; widget_image treats NULL as "skip draw",
// so a scene using <image> still runs on those backends (it just shows
// the node's background rect instead of the picture).

GUI_API void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height);

GUI_API void  renderer__destroy_texture(void* tex);

GUI_API void  renderer__submit_image(gui_rect_t rect, void* tex, gui_color_t tint);

GUI_API void  renderer__submit_image_uv(gui_rect_t rect, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1);

// ============================================================================
// SCISSOR (CLIPPING)
// ============================================================================
// push_scissor / pop_scissor bracket a region of submissions whose draws
// are clipped to the given rect. Used by scrollable containers (widget_div,
// widget_window) to hide content that overflows their bounds.
// Calls nest. The current effective scissor is the INTERSECTION of every
// rect on the stack -- so a scrollable child div inside a scrollable
// parent div correctly clips to the smaller of the two regions.
// Implementation note: backends flush their batched submissions on every
// push/pop because state changes mid-frame can't be merged into a single
// draw call. Scrollable regions therefore add a few extra draw calls per
// frame -- fine for the dozens of regions a real UI has.

GUI_API void renderer__push_scissor(gui_rect_t rect);

GUI_API void renderer__pop_scissor(void);

GUI_API void renderer__blur_region(gui_rect_t rect, float sigma_px);

// ===== frame capture ========================================================
// renderer__request_capture arms a copy of the next finished frame: at the
// next renderer__end_frame, after the last draw and before the frame is
// presented, the backend reads the whole back buffer into `out_rgba` as
// top-down RGBA8 rows (renderer__capture_size tells how big that is) and
// clears the request; renderer__capture_done is TRUE once the copy
// happened. The visual test harness captures through this instead of
// copying the window with GDI: a flip-model swap chain has nothing for
// GDI to copy, and a desktop window in front would end up in the
// picture. A backend without a readback path returns FALSE from request.
GUI_API boole renderer__capture_size(int64* out_w, int64* out_h);
GUI_API boole renderer__request_capture(ubyte* out_rgba, int64 cap);
GUI_API boole renderer__capture_done(void);


#endif
