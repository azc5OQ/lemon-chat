// ============================================================================
// opengl3_renderer.c - opengl 3.3 core backend.
// ============================================================================
// WHAT THIS FILE DOES (the elevator pitch):
// scene.c walks the widget tree and tells us "draw this colored rectangle
// here" by calling renderer__submit_rect(). this file packages those calls
// into work the GPU understands and ships it across to the graphics card
// using the opengl 3.3 api. the GPU then turns that into pixels on screen.
// this backend also owns every piece of the opengl integration stack:
// picking a compatible pixel format for the window, building a 3.3 core
// context, loading the function pointers for every GL entry point we use,
// and calling SwapBuffers at the end of each frame. platform_win32 just
// hands us an HWND at init and stays out of our way.
// WHY OPENGL?
// the GPU doesn't speak C. it speaks a small set of binary commands like
// "draw triangles using these vertices", "run this little program for every
// pixel inside them", "blend the result with what's already on screen".
// opengl is one of several apis that gives us a portable way to issue those
// commands. d3d9, d3d11, vulkan, metal are alternatives -- our renderers/
// directory has stubs for d3d9 and d3d11 alongside this one. exactly one
// is compiled into the gui library at a time.
// WHY BATCHING?
// each call from CPU to GPU costs hundreds of microseconds of overhead --
// context switching, driver bookkeeping, command queueing. drawing 100 rects
// with 100 separate "draw" calls is dramatically slower than gathering them
// into a list of 600 vertices (100 rects * 6 verts per quad) and issuing
// one "draw 600 vertices" call. so:
// begin_frame:    clear screen, reset our cpu-side vertex list.
// submit_rect *N: append 6 vertices to the list per rectangle.
// end_frame:      copy the whole list to the GPU, issue ONE draw call,
// then SwapBuffers to make the frame visible.
// this gets us 60+ FPS even with thousands of rects per frame.
// ROUNDED CORNERS WITHOUT EXTRA TRIANGLES (SDF):
// the obvious way to draw a rounded rect is to tessellate the corners with
// many tiny triangles (like 8 per corner) approximating the curve. that
// works but it's wasteful and aliases badly when zoomed.
// instead, every rect is just two triangles forming a square (6 vertices).
// the fragment shader -- a tiny program the GPU runs FOR EVERY PIXEL inside
// those triangles -- asks for each pixel: "am i inside the rounded shape,
// outside, or right on the edge?" using a Signed Distance Function (SDF).
// pixels inside: full color. pixels outside: discarded (transparent).
// pixels on the edge: partial alpha for free anti-aliasing.
// result: pixel-perfect rounded corners at any size, with one shader, no
// per-corner geometry. when radius is zero the math collapses to a regular
// rectangle and you just get sharp corners.
// VISUAL CONTRACT:
// this backend must produce the same pixels as every other backend
// (d3d11, d3d9, ...) given the same input -- see renderer.h's "VISUAL
// CONTRACT" section. the SDF function, blend mode, smoothstep range,
// and radius clamping are all specified; don't diverge from them.

#if defined(_WIN32)
#include <windows.h>
#include <wingdi.h>
#else
#include <GL/glx.h>
#endif
#include <GL/gl.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h> // uintptr_t for the GLuint <-> void* atlas-handle round trip.

#include "types.h"
#include "gui.h"
#define MEEK_RENDERER_BACKEND
#include "renderer.h"
#include "renderer_backend.h"
#include "gradient.h"
#include "opengl3_renderer.h"
#include "_visual_contract.h"
#include "_blur_cpu.h"
#include "_mip_cpu.h"
#include "clib/stdlib.h"
// gl_funcs.h lives next to this file (inside renderers/) because its
// contents are opengl-specific. include path is relative to this file.
#include "gl_funcs.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// ============================================================================
// GL FUNCTION POINTER STORAGE
// ============================================================================
// gl_funcs.h declares these as extern. we define them (allocate the storage)
// here because this backend is the sole loader + user of them. on non-GL
// backend builds this file isn't compiled, so the globals don't exist and
// nothing else references them -- the linker is happy.

fncp_glCreateShader            p_glCreateShader;
fncp_glDeleteShader            p_glDeleteShader;
fncp_glShaderSource            p_glShaderSource;
fncp_glCompileShader           p_glCompileShader;
fncp_glGetShaderiv             p_glGetShaderiv;
fncp_glGetShaderInfoLog        p_glGetShaderInfoLog;

fncp_glCreateProgram           p_glCreateProgram;
fncp_glDeleteProgram           p_glDeleteProgram;
fncp_glAttachShader            p_glAttachShader;
fncp_glLinkProgram             p_glLinkProgram;
fncp_glGetProgramiv            p_glGetProgramiv;
fncp_glGetProgramInfoLog       p_glGetProgramInfoLog;
fncp_glUseProgram              p_glUseProgram;

fncp_glGetUniformLocation      p_glGetUniformLocation;
fncp_glUniform2f               p_glUniform2f;
fncp_glUniform1i               p_glUniform1i;
fncp_glUniform1f               p_glUniform1f;
fncp_glGenFramebuffers         p_glGenFramebuffers;
fncp_glDeleteFramebuffers      p_glDeleteFramebuffers;
fncp_glBindFramebuffer         p_glBindFramebuffer;
fncp_glBlitFramebuffer         p_glBlitFramebuffer;
fncp_glGetFramebufferAttachmentParameteriv p_glGetFramebufferAttachmentParameteriv;
fncp_glFramebufferTexture2D    p_glFramebufferTexture2D;
fncp_glCheckFramebufferStatus  p_glCheckFramebufferStatus;
fncp_glActiveTexture           p_glActiveTexture;

fncp_glGenBuffers              p_glGenBuffers;
fncp_glDeleteBuffers           p_glDeleteBuffers;
fncp_glBindBuffer              p_glBindBuffer;
fncp_glBufferData              p_glBufferData;
fncp_glBufferSubData           p_glBufferSubData;

fncp_glGenVertexArrays         p_glGenVertexArrays;
fncp_glBindVertexArray         p_glBindVertexArray;
fncp_glDeleteVertexArrays      p_glDeleteVertexArrays;
fncp_glVertexAttribPointer     p_glVertexAttribPointer;
fncp_glEnableVertexAttribArray p_glEnableVertexAttribArray;

fncp_glBlendFuncSeparate       p_glBlendFuncSeparate;
fncp_glGenerateMipmap          p_glGenerateMipmap;

// ============================================================================
// WGL EXTENSION BITS FOR 3.3 CORE CONTEXT CREATION
// ============================================================================
// core 3.3 contexts are created via wglCreateContextAttribsARB, which is an
// extension. to call it we first need a GL context (any context) to resolve
// the function pointer. the classic chicken-and-egg dance:
// 1. make a "legacy" (2.x) context so we can call wglGetProcAddress.
// 2. resolve wglCreateContextAttribsARB via that context.
// 3. call it to create the real 3.3 core context.
// 4. throw away the legacy context.

#if defined(_WIN32)
typedef HGLRC (WINAPI* fncp_wglCreateContextAttribsARB)(HDC hDC, HGLRC hShareContext, const int* attribList);
#endif

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

// ============================================================================
// VERTEX FORMAT
// ============================================================================
// a "vertex" is just a struct of data the GPU shader program receives one
// copy of, per corner of every triangle we draw. you can think of it as
// "the input row to the vertex shader".
// each rect = 6 vertices (2 triangles = 6 corners), and each vertex carries:
// - WHERE on screen this corner is        (x, y)         in pixels
// - WHAT color this corner should be      (r, g, b, a)
// - WHERE inside the rect this corner is  (lx, ly)       in 0..rect_w/h
// - HOW BIG the parent rect is            (rect_w, rect_h)
// - HOW ROUND the corners are             (radius)
// the last three are the same value for all 6 vertices of one rect. they're
// "varyings" the fragment shader needs to compute the SDF math. we duplicate
// them per-vertex (instead of using uniforms or instancing) because it keeps
// the batching trivial -- one pass over the buffer, no per-rect bookkeeping
// on the GPU side.

/**
 *one vertex in the cpu-side staging buffer. layout MUST match the shader
 *attribute setup in renderer__init below (same field order, same types).
 *if you change this struct, also change those attribute pointers.
 */
typedef struct _opengl3_renderer_internal__vertex_t
{
    // pixel x of this corner (top-left origin: 0 = left edge of window).
    float x;        // 0x0
    // pixel y of this corner (0 = top edge of window).
    float y;        // 0x4
    // red component of this corner, 0..1.
    float r;        // 0x8
    // green component, 0..1.
    float g;        // 0xC
    // blue component, 0..1.
    float b;        // 0x10
    // alpha (opacity), 0..1. 0 = fully transparent, 1 = opaque.
    float a;        // 0x14
    // local x INSIDE the rect this vertex belongs to: 0 at the left edge, rect_w at the right edge.
    float lx;       // 0x18
    // local y INSIDE the rect: 0 at the top, rect_h at the bottom.
    float ly;       // 0x1C
    // full width of the rect this vertex belongs to. fragment shader needs it for the SDF math.
    float rect_w;   // 0x20
    // full height of the rect.
    float rect_h;   // 0x24
    // corner radius in pixels. 0 = sharp rectangle. positive = rounded; clamped to half the smaller side.
    float radius;   // 0x28
} _opengl3_renderer_internal__vertex_t;

// how many rects we can hold in our cpu-side batch buffer in one frame.
// going past this drops draw calls silently. 2048 is plenty for the poc.
// once we add scrolling lists of thousands of items, we'll either grow this
// or flush mid-frame.
#define _OPENGL3_RENDERER_INTERNAL__MAX_QUADS       16384
#define _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD  6

// text batch capacity: one textured quad per glyph. 4096 glyphs per frame
// is plenty for a typical UI (a screen of 14px code at 40 lines * 80 cols
// is ~3200 glyphs); anything larger should be paged with scroll clipping
// anyway. kept conservative because this toolkit is intended to render on
// ARM embedded devices and phones where VBO memory matters -- 4096 * 6
// verts * 32 bytes = 768 KiB, well under mobile GPU working-set limits.
// each glyph adds 6 verts of the smaller text vertex format declared below.
#define _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096

// text run: a contiguous slice of the text vertex buffer that samples
// from ONE atlas. the frame can intermix multiple atlases (different
// fonts + sizes); we split the batch into runs so each run binds its
// own texture. set_text_atlas closes the current run and opens a new
// one when the atlas changes; each (font__draw call for a fresh font)
// typically produces one run.
// MAX_TEXT_RUNS caps how many distinct atlases can be used per frame.
// 64 covers 16 fonts at 4 sizes each, well beyond any realistic UI.
#define _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_RUNS 64

typedef struct _opengl3_renderer_internal__text_run_t
{
    // GL texture handle for this run.
    GLuint atlas;        // 0x0
    // offset into text_cpu_verts.
    int64  vert_start;   // 0x8
    // number of verts belonging to this run.
    int64  vert_count;   // 0x10
} _opengl3_renderer_internal__text_run_t;

// a text vertex is simpler than the solid-rect vertex: no SDF bookkeeping,
// just position + color + atlas uv. one per corner of each glyph quad, 6
// per glyph.
typedef struct _opengl3_renderer_internal__text_vertex_t
{
    // pixel x of this corner (top-left origin: 0 = left edge of window).
    float x;   // 0x0
    // pixel y.
    float y;   // 0x4
    // color channels, 0..1. per-vertex so each draw carries its own tint.
    float r;   // 0x8
    float g;   // 0xC
    float b;   // 0x10
    float a;   // 0x14
    // atlas UV in 0..1.
    float u;   // 0x18
    float v;   // 0x1C
} _opengl3_renderer_internal__text_vertex_t;

// ============================================================================
// RENDERER STATE
// ============================================================================
// all the GPU object handles, win32 handles, and CPU-side staging we hold
// onto across frames. exactly one instance of this struct exists for the
// process lifetime; it's allocated as a file-static global below.

/**
 *renderer state. file-local; one instance per process.
 */
typedef struct _opengl3_renderer_internal__state_t
{
    // host window. supplied by platform_win32 in renderer__init.
#if defined(_WIN32)
    HWND   hwnd;                                                                                 // 0x0
    // device context for the window. we derive this ourselves; we hold onto it for SwapBuffers.
    HDC    hdc;                                                                                  // 0x8
    // gl 3.3 core context.
    HGLRC  hglrc;                                                                                // 0x10
#endif

    // solid-rect (SDF rounded-box) pipeline.
    // handle to the linked vertex+fragment shader program. set by renderer__init.
    GLuint program;                                                                              // 0x18
    // cached "location" (slot) for the u_viewport uniform inside the program. -1 if not found.
    GLint  u_viewport_loc;                                                                       // 0x1C
    // Vertex Array Object: a recipe describing how the VBO's bytes map to shader attributes.
    GLuint vao;                                                                                  // 0x20
    // Vertex Buffer Object: a chunk of GPU memory holding our vertices.
    GLuint vbo;                                                                                  // 0x24
    // cpu-side scratch buffer; we fill this during the frame, then upload to the VBO once.
    _opengl3_renderer_internal__vertex_t* cpu_verts;                                               // 0x28
    // how many vertices we've written into cpu_verts this frame so far.
    int64                       vert_count;                                                      // 0x30
    // capacity of cpu_verts (and the GPU VBO) in vertices.
    int64                       vert_cap;                                                        // 0x38
    // window width in pixels, captured at begin_frame. used by the shader to convert pixel coords to GPU coords.
    int64 viewport_w;                                                                            // 0x40
    // window height in pixels.
    int64 viewport_h;                                                                            // 0x48

    // text pipeline. separate shader (no SDF; just textured alpha),
    // separate VAO/VBO, separate cpu staging buffer. drawn after the
    // solid pass in end_frame so glyphs land on top of button bg rects.
    // per-atlas "runs": the frame can mix text from multiple fonts
    // (different family/size pairs => different atlases). each run is
    // a contiguous slice of the vertex buffer that binds one atlas.
    // end_frame does one glDrawArrays per run.
    // shader program for the textured glyph quads.
    GLuint text_program;                                                                         // 0x50
    // u_viewport slot in text_program.
    GLint  text_u_viewport_loc;                                                                  // 0x54
    // u_atlas (sampler2D) slot in text_program.
    GLint  text_u_atlas_loc;                                                                     // 0x58
    // vao for the text vertex layout.
    GLuint text_vao;                                                                             // 0x5C
    // vbo for text vertices.
    GLuint text_vbo;                                                                             // 0x60
    // cpu staging for text verts.
    _opengl3_renderer_internal__text_vertex_t* text_cpu_verts;                                     // 0x68
    // text verts written this frame.
    int64  text_vert_count;                                                                      // 0x70
    // capacity of text_cpu_verts.
    int64  text_vert_cap;                                                                        // 0x78
    // GLuint of the atlas most recently set by set_text_atlas.
    GLuint current_text_atlas;                                                                   // 0x80

    _opengl3_renderer_internal__text_run_t text_runs[_OPENGL3_RENDERER_INTERNAL__MAX_TEXT_RUNS];   // 0x88
    int64                                text_run_count;                                         // 0x688

    // image pipeline. lazy-initialized on first renderer__submit_image
    // call; zero means "not built yet". Separate shader (samples RGBA
    // not just .r). Separate 6-vert VBO used immediate-mode (one quad
    // per draw call, no batching -- images are 0..N-few per frame).
    GLuint image_program;                                                                        // 0x690
    GLint  image_u_viewport_loc;                                                                 // 0x694
    GLint  image_u_tex_loc;                                                                      // 0x698
    GLuint image_vao;                                                                            // 0x69C
    GLuint image_vbo;                                                                            // 0x6A0

    // Layer pipeline (renderer_backend.h, layer mode): a homogeneous quad
    // (x * w, y * w, w) + uv and a premultiplied texel times the opacity.
    // Lazy-built like the image pipeline.
    GLuint layer_program;
    GLint  layer_u_viewport_loc;
    GLint  layer_u_tex_loc;
    GLint  layer_u_opacity_loc;
    GLuint layer_vao;
    GLuint layer_vbo;
    // render-to-layer pool (renderer_backend__begin_layer): framebuffers
    // with their color textures reused by size; the frame state to restore
    struct
    {
        GLuint fbo;
        GLuint texture;
        int    w;
        int    h;
        uint64 used;
    } layer_targets[4];
    int    layer_active; // slot + 1 while drawing a layer, else 0
    int64  layer_saved_viewport_w;
    int64  layer_saved_viewport_h;
    int64  layer_saved_scissor_depth;
    gui_rect_t layer_saved_scissors[32];
    boole  layer_saved_srgb;
    GLint  layer_saved_framebuffer; // an embedded host may draw into its own
    uint64 layer_counter;

    // scissor stack. push_scissor records the new clip rect at the top
    // and applies the intersection of all stacked rects via glScissor.
    // pop pops one. depth 0 means "no scissor; everything draws".
    // 32 deep is way more than any sane UI nesting needs.
    gui_rect_t scissor_stack[32];                                                                  // 0x6A4
    int64    scissor_depth;                                                                      // 0x8A8

    // sRGB framebuffer in play. Set by set_pixel_format when
    // wglChoosePixelFormatARB successfully returns a pixel format
    // with WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB = TRUE. When set:
    // - glEnable(GL_FRAMEBUFFER_SRGB) runs after context creation.
    // - Clear color is linearized before glClearColor (the default
    // framebuffer treats clear values as linear and gamma-encodes
    // on store).
    // - build_pipeline compiles the sRGB-aware fragment shaders
    // which convert vertex-color RGB from sRGB to linear, so the
    // hardware's linear->sRGB write produces the correct color.
    // When NOT set (driver lacks WGL_ARB_pixel_format, or it failed
    // to find a matching format), we stay on the legacy non-sRGB
    // framebuffer and compile the older shaders that keep colors in
    // sRGB space and use the pow(a, 1/2.2) coverage workaround for
    // text. Same visual output as before this refactor.
    boole srgb_fb;                                                                               // 0x8B0
    // adopted: the context belongs to the host (embedded / overlay use).
    // No clear, no SwapBuffers, no context or DC teardown.
    boole adopted;                                                                               // 0x8B4
} _opengl3_renderer_internal__state_t;

static _opengl3_renderer_internal__state_t _opengl3_renderer_internal__g;

// ============================================================================
// SHADER SOURCES
// ============================================================================
// "shader" = a tiny program that runs on the GPU. opengl 3.3 uses GLSL, a
// C-like language. we have two programs forming one pipeline:
// VERTEX SHADER:    runs once per vertex (so 6 times per rect).
// receives the vertex's attributes as inputs (in vec2/vec4).
// its job: write gl_Position (where on screen this vertex
// lands, in "Normalized Device Coordinates" -- see below).
// can also produce "varyings" (out variables) that get
// automatically interpolated across the triangle and
// delivered to the fragment shader.
// FRAGMENT SHADER:  runs once per pixel inside the triangle (could be
// millions of times per frame). receives the interpolated
// varyings from the vertex shader. writes the final pixel
// color (or "discards" the pixel to leave it transparent).
// between them: the GPU's rasterizer figures out which pixels are inside the
// triangle and runs the fragment shader for each one, interpolating the
// varyings linearly (or perspective-correct, but for 2D it's just linear).
// NORMALIZED DEVICE COORDINATES (NDC):
// the GPU's native coordinate system for gl_Position. it's a cube from
// (-1, -1, -1) to (+1, +1, +1) where (-1, -1) is bottom-left of the
// screen and (+1, +1) is top-right. we work in pixels (top-left = 0,0)
// because that's what humans understand. the vertex shader does the
// conversion: pixel_x / viewport_width * 2 - 1 -> NDC x, and so on.
// note the y-axis flip: pixel y grows downward, NDC y grows upward.

// VERTEX SHADER:
// inputs:  the 5 per-vertex attributes (matching our vertex struct layout).
// uniform: u_viewport (constant for all vertices in one draw call -- the
// current window size in pixels).
// outputs: gl_Position (built-in: where this vertex lives in NDC),
// plus varyings v_color/v_local/v_rect_size/v_radius forwarded
// to the fragment shader.
#include "shaders/generated/rect.vert.h"

// FRAGMENT SHADER: SDF rounded-box + 1px smoothstep AA. see renderer.h's
// VISUAL CONTRACT section -- the SDF formula and smoothstep range are
// binding on all backends, not just this one.
#include "shaders/generated/rect.frag.h"

// TEXT VERTEX SHADER:
// simpler than the solid-rect one -- just (position, color, uv). the
// same pixel-coords -> NDC conversion is used. varying outputs the uv
// so the fragment shader can sample the atlas.
#include "shaders/generated/text.vert.h"

// TEXT FRAGMENT SHADER:
// samples the atlas (R8 alpha-only glyph coverage) with LINEAR
// filtering and multiplies it by the vertex color's alpha to produce
// smooth anti-aliased text. discards when alpha is zero so we don't
// create needless blend work for transparent pixels (and so quad
// edges between glyphs don't accumulate "zero + zero" color that
// could be picked up by downstream effects).
// GAMMA-CORRECT EDGE COVERAGE. We don't render to an sRGB framebuffer
// (that would require WGL_ARB_pixel_format's sRGB-capable attribute,
// which needs a whole dummy-context dance to resolve before we can
// call it -- a bigger refactor than we wanted for the first sharpness
// pass). Instead we apply the gamma curve to the atlas coverage
// BEFORE multiplying by vertex alpha: pow(a, 1/2.2).
// Why this helps: standard SRC_ALPHA/INV_SRC_ALPHA blending treats
// 8-bit color values as if they were linear intensities, but monitors
// interpret them as sRGB. That means a pixel with coverage=0.5 blends
// 50%/50% between text color and background in code, but the human
// eye sees it as ~22% brightness relative to full text color
// (because sRGB curves the lower half hard). Edges of glyphs end up
// looking dim and soft -- the "washed-out grayscale AA" look that
// makes our text feel blurry next to DirectWrite's output in WSL
// Settings or similar native UIs.
// pow(coverage, 1/2.2) reshapes the coverage so that a linear ramp
// in input produces a visually-linear ramp out. At coverage=0.5 this
// gives ~0.73, i.e. 73% blend toward text color, which lands at the
// perceptual midpoint. Edges appear solid and crisp, not smeared.
// This is an approximation of proper sRGB-framebuffer blending --
// accurate to within a percent or two for the common case of light
// text on a dark background, which is essentially all of the UI.
// For a fully physically-correct pipeline we would eventually want
// an sRGB swap chain (or an intermediate sRGB FBO we blit from) on
// all four backends; until then this keeps the rest of the pipeline
// untouched.
#include "shaders/generated/text.frag.h"

// ===== sRGB-framebuffer shader variants ====================================
// Used when the pixel format is sRGB-capable (wglChoosePixelFormatARB +
// WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB) and GL_FRAMEBUFFER_SRGB is enabled.
// In that mode, the hardware converts shader output values from linear
// to sRGB on write, and alpha blending happens in linear space --
// exactly what we want for physically-correct edge compositing.
// Two changes from the legacy shaders above:
// 1. vec3 linear_rgb = pow(v_color.rgb, vec3(2.2));
// Vertex colors are sourced from sRGB hex values in .style
// (e.g. #6366f1). The hardware writes linear->sRGB, so we
// must feed it linear values; converting in the fragment
// shader is the simplest place.
// 2. Text shader drops the pow(a, 1/2.2) coverage workaround.
// That hack existed to approximate gamma-correct blending on
// a non-sRGB FB by brightening edge coverage; with a real
// sRGB FB the math is already correct. Applying it here
// would DOUBLE-gamma the alpha and produce thick/chunky text.
// pow(x, 2.2) is a fast approximation of the full piecewise sRGB
// transfer function. Error is < 0.01 across the 0..1 range, which is
// well below 8-bit quantization. Worth the two saved instructions.
#include "shaders/generated/rect_srgb.frag.h"

#include "shaders/generated/text_srgb.frag.h"

// forward declarations of file-local helpers. defined further down.
#if defined(_WIN32)
static boole  _opengl3_renderer_internal__set_pixel_format(HDC hdc);
#endif
#if defined(_WIN32)
static boole  _opengl3_renderer_internal__create_core_context(HDC hdc, HGLRC* out_ctx);
#endif
static boole  _opengl3_renderer_internal__load_gl_functions(void);
static boole  _opengl3_renderer_internal__build_pipeline(void);
static boole  _opengl3_renderer_internal__build_text_pipeline(void);
static void   _opengl3_renderer_internal__flush_batches(void);
// renderer_backend__submit_tiles statistics, logged at shutdown
static uint64 _opengl3_renderer_internal__tile_vertices = 0;
static uint64 _opengl3_renderer_internal__tile_flushes = 0;
static GLuint _opengl3_renderer_internal__compile_shader(GLenum kind, char* src);
static GLuint _opengl3_renderer_internal__link_program(GLuint vs, GLuint fs);
static void _opengl3_renderer_internal__release_blur(void);
static boole  _opengl3_renderer_internal__ensure_layer_pipeline(void);
static void   _opengl3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c);
static void   _opengl3_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color_t c);
#if defined(_WIN32)
static void   _opengl3_renderer_internal__log_win32_error(char* where);
#endif
static void   _opengl3_renderer_internal__capture_if_requested(void);

// ============================================================================
// PUBLIC: renderer__init
// ============================================================================
// bring up the GL context against the window the platform layer just
// created, then build our rendering pipeline on top.
// steps:
// 1. grab an HDC for the window.
// 2. pick + set a pixel format compatible with opengl.
// 3. create a dummy legacy GL context so we can resolve the 3.3 core
// context-creation extension.
// 4. create the real 3.3 core context.
// 5. make it current on this thread.
// 6. load every GL 3.x function pointer we need.
// 7. build shaders + VAO + VBO + cpu staging buffer (the "pipeline").
// 8. enable alpha blending.

/**
 * @brief bring up the graphics context and initialize the per-frame pipeline. Called once at startup AFTER the platform layer has created its window. The renderer owns whatever GPU objects it needs (GL context, D3D device+swapchain, shaders, vertex buffers) and stores them in its own file-static state.
 *
 * @param void* native_window -> Platform window handle. On Windows this is HWND (`struct HWND__*`). Backends cast as needed.
 *
 * @return boole -> TRUE on success; FALSE if context creation, shader compilation, or resource allocation failed.
 */

#if defined(_WIN32)
boole renderer__init(void* native_window)
{
    stdlib__memset(&_opengl3_renderer_internal__g, 0, sizeof(_opengl3_renderer_internal__g));

    _opengl3_renderer_internal__g.hwnd = (HWND)native_window;
    if (_opengl3_renderer_internal__g.hwnd == NULL_POINTER)
    {
        log_error("%s", "renderer__init got a NULL window handle");
        return FALSE;
    }

    // step 1: get an HDC. required for every subsequent GL+wgl call.
    // we hold onto it for the life of the renderer -- SwapBuffers uses
    // it at end_frame.
    _opengl3_renderer_internal__g.hdc = GetDC(_opengl3_renderer_internal__g.hwnd);
    if (_opengl3_renderer_internal__g.hdc == NULL_POINTER)
    {
        _opengl3_renderer_internal__log_win32_error("GetDC");
        return FALSE;
    }

    // step 2: pixel format. describes the back buffer's bit depth,
    // double-buffering, stencil/depth availability, etc. CAN ONLY BE
    // SET ONCE per window. if we ever destroy and recreate the GL
    // context for the same window we'd be stuck.
    if (_opengl3_renderer_internal__set_pixel_format(_opengl3_renderer_internal__g.hdc) == FALSE)
    {
        return FALSE;
    }

    // steps 3+4: create the real core context via the dummy-context dance.
    if (_opengl3_renderer_internal__create_core_context(_opengl3_renderer_internal__g.hdc, &_opengl3_renderer_internal__g.hglrc) == FALSE)
    {
        return FALSE;
    }

    // step 5: bind the context to this thread. subsequent gl* calls go
    // through it.
    if (wglMakeCurrent(_opengl3_renderer_internal__g.hdc, _opengl3_renderer_internal__g.hglrc) == FALSE)
    {
        _opengl3_renderer_internal__log_win32_error("wglMakeCurrent");
        return FALSE;
    }

    // step 6: load every GL 3.x function pointer we use. fails loudly
    // if any one is missing -- means the driver lied about supporting 3.3.
    if (_opengl3_renderer_internal__load_gl_functions() == FALSE)
    {
        log_error("%s", "failed to load gl 3.3 functions");
        return FALSE;
    }

    // step 7+8: build the draw pipeline (shaders, VAO, VBO, cpu buffer,
    // blend state).
    return _opengl3_renderer_internal__build_pipeline();
}
#endif


#if !defined(_WIN32)
boole renderer__init(void* native_window)
{
    (void)native_window;
    log_error("%s", "opengl3: Linux uses the embedded current-context entry point; standalone Linux uses GLES3");
    return FALSE;
}
#endif

static meek_opengl_get_proc_fn _opengl3_renderer_internal__resolver;

void* renderer__opengl_proc_address(const char* name)
{
    if (_opengl3_renderer_internal__resolver) return _opengl3_renderer_internal__resolver(name);
#if defined(_WIN32)
    void* address = (void*)wglGetProcAddress(name);
    if (address == NULL_POINTER || (uintptr_t)address <= 3 || (intptr_t)address == -1)
        address = (void*)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
    return address;
#else
    return (void*)glXGetProcAddressARB((const GLubyte*)name);
#endif
}

boole renderer__prepare_adopt_opengl(meek_opengl_get_proc_fn resolver)
{
    const char* version = (const char*)glGetString(GL_VERSION);
    char* end = NULL_POINTER;
    int major = version ? (int)stdlib__strtol(version, &end, 10) : 0;
    int minor = end && *end == '.' ? (int)stdlib__strtol(end + 1, NULL_POINTER, 10) : 0;
    if (major < 3 || (major == 3 && minor < 3))
    {
        log_error("%s", "embedded OpenGL needs a current desktop OpenGL 3.3+ context");
        return FALSE;
    }
    _opengl3_renderer_internal__resolver = resolver;
    return _opengl3_renderer_internal__load_gl_functions();
}

// ============================================================================
// PUBLIC: renderer__init_adopt_opengl
// ============================================================================

/**
 * @brief initializes the renderer on the OpenGL context that is current on the calling thread; the host owns that context and its window
 *
 * @attention the context must expose OpenGL 3.3 core functions through wglGetProcAddress, which every compatibility context on a current driver does and a strictly 2.x-only context does not; the renderer never clears, never swaps and never deletes the context
 *
 * @return boole -> TRUE on success
 */
boole renderer__init_adopt_opengl(void)
{
    const char* version = NULL_POINTER;
    char* end = NULL_POINTER;
    int major = 0;
    int minor = 0;

    stdlib__memset(&_opengl3_renderer_internal__g, 0, sizeof(_opengl3_renderer_internal__g));

    _opengl3_renderer_internal__g.adopted = TRUE;
    _opengl3_renderer_internal__g.srgb_fb = FALSE;

    // "major.minor ..." from the driver; the loader below fails on its own when a function is missing, this just gives a readable reason
    version = (const char*)glGetString(GL_VERSION);

    if (version != NULL_POINTER)
    {
        major = (int)stdlib__strtol(version, &end, 10);
        minor = (end != NULL_POINTER && *end == '.') ? (int)stdlib__strtol(end + 1, NULL_POINTER, 10) : 0;
        log_info("%s %s", "renderer__init_adopt_opengl: host context reports OpenGL", version);
    }

    if (version == NULL_POINTER || major < 3 || (major == 3 && minor < 3))
    {
        log_error("%s", "renderer__init_adopt_opengl: an OpenGL 3.3 capable context is required");
        stdlib__memset(&_opengl3_renderer_internal__g, 0, sizeof(_opengl3_renderer_internal__g));
        return FALSE;
    }

    if (_opengl3_renderer_internal__load_gl_functions() == FALSE)
    {
        log_error("%s", "renderer__init_adopt_opengl: failed to load the OpenGL 3.3 functions from the host context");
        stdlib__memset(&_opengl3_renderer_internal__g, 0, sizeof(_opengl3_renderer_internal__g));
        return FALSE;
    }

    if (_opengl3_renderer_internal__build_pipeline() == FALSE)
    {
        renderer__shutdown();
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// PUBLIC: renderer__shutdown
// ============================================================================
// symmetric with init. tear down the pipeline, then the context, then
// release the HDC. safe to call even if init bailed partway, because
// every handle starts at 0 / NULL and we null-check each one.

/**
 * @brief tear down the graphics context and free every GPU/CPU resource owned by the renderer. Symmetric with renderer__init; safe to call even if init failed partway.
 *
 * @return void
 */
void renderer__shutdown(void)
{
    if (_opengl3_renderer_internal__tile_vertices > 0)
    {
        log_info("%s %llu %s %llu", "tiles: vertices", (unsigned long long)_opengl3_renderer_internal__tile_vertices, "flushes", (unsigned long long)_opengl3_renderer_internal__tile_flushes);
    }
    // solid-rect pipeline objects.
    if (_opengl3_renderer_internal__g.vbo != 0)
    {
        glDeleteBuffers(1, &_opengl3_renderer_internal__g.vbo);
    }
    if (_opengl3_renderer_internal__g.vao != 0)
    {
        glDeleteVertexArrays(1, &_opengl3_renderer_internal__g.vao);
    }
    if (_opengl3_renderer_internal__g.program != 0)
    {
        glDeleteProgram(_opengl3_renderer_internal__g.program);
    }
    if (_opengl3_renderer_internal__g.cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_opengl3_renderer_internal__g.cpu_verts);
    }

    // text pipeline objects. atlas textures are owned by font.c; it
    // destroys them via renderer__destroy_atlas before we get here.
    if (_opengl3_renderer_internal__g.text_vbo != 0)
    {
        glDeleteBuffers(1, &_opengl3_renderer_internal__g.text_vbo);
    }
    if (_opengl3_renderer_internal__g.text_vao != 0)
    {
        glDeleteVertexArrays(1, &_opengl3_renderer_internal__g.text_vao);
    }
    if (_opengl3_renderer_internal__g.text_program != 0)
    {
        glDeleteProgram(_opengl3_renderer_internal__g.text_program);
    }
    if (_opengl3_renderer_internal__g.text_cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_opengl3_renderer_internal__g.text_cpu_verts);
    }

    // image pipeline (lazy-built; guards against "never used" case).
    // RGBA textures created via renderer__create_texture_rgba are owned
    // by whichever widget called it; widget_image::on_destroy calls
    // renderer__destroy_texture. No teardown loop here.
    if (_opengl3_renderer_internal__g.image_vbo != 0)
    {
        glDeleteBuffers(1, &_opengl3_renderer_internal__g.image_vbo);
    }
    // render-to-layer pool
    for (int slot = 0; slot < 4; slot++)
    {
        if (_opengl3_renderer_internal__g.layer_targets[slot].fbo != 0)
        {
            glDeleteFramebuffers(1, &_opengl3_renderer_internal__g.layer_targets[slot].fbo);
        }
        if (_opengl3_renderer_internal__g.layer_targets[slot].texture != 0)
        {
            glDeleteTextures(1, &_opengl3_renderer_internal__g.layer_targets[slot].texture);
        }
        stdlib__null_memory(&_opengl3_renderer_internal__g.layer_targets[slot], sizeof(_opengl3_renderer_internal__g.layer_targets[slot]));
    }
    _opengl3_renderer_internal__release_blur();
    // layer pipeline; layer textures are owned by the compositor's cache
    if (_opengl3_renderer_internal__g.layer_vbo != 0)
    {
        glDeleteBuffers(1, &_opengl3_renderer_internal__g.layer_vbo);
        _opengl3_renderer_internal__g.layer_vbo = 0;
    }
    if (_opengl3_renderer_internal__g.layer_vao != 0)
    {
        glDeleteVertexArrays(1, &_opengl3_renderer_internal__g.layer_vao);
        _opengl3_renderer_internal__g.layer_vao = 0;
    }
    if (_opengl3_renderer_internal__g.layer_program != 0)
    {
        glDeleteProgram(_opengl3_renderer_internal__g.layer_program);
        _opengl3_renderer_internal__g.layer_program = 0;
    }
    if (_opengl3_renderer_internal__g.image_vao != 0)
    {
        glDeleteVertexArrays(1, &_opengl3_renderer_internal__g.image_vao);
    }
    if (_opengl3_renderer_internal__g.image_program != 0)
    {
        glDeleteProgram(_opengl3_renderer_internal__g.image_program);
    }

    // tear down GL context. wglMakeCurrent(NULL, NULL) unbinds it from
    // this thread first -- required before deletion or the driver may
    // complain / leak.
    // an adopted context and DC belong to the host and stay current
#if defined(_WIN32)
    if (_opengl3_renderer_internal__g.adopted == FALSE && _opengl3_renderer_internal__g.hglrc != NULL_POINTER)
    {
        wglMakeCurrent(NULL_POINTER, NULL_POINTER);
        wglDeleteContext(_opengl3_renderer_internal__g.hglrc);
    }
    if (_opengl3_renderer_internal__g.adopted == FALSE && _opengl3_renderer_internal__g.hdc != NULL_POINTER)
    {
        ReleaseDC(_opengl3_renderer_internal__g.hwnd, _opengl3_renderer_internal__g.hdc);
    }

#endif
    stdlib__memset(&_opengl3_renderer_internal__g, 0, sizeof(_opengl3_renderer_internal__g));
}

// ============================================================================
// PUBLIC: renderer__begin_frame
// ============================================================================

/**
 * @brief start a frame. Set the viewport, clear the back buffer to `clear`, reset the per-frame vertex batch.
 *
 * @param int64 viewport_w -> Viewport width in pixels.
 * @param int64 viewport_h -> Viewport height in pixels.
 * @param gui_color_t clear -> Back-buffer clear color.
 *
 * @return void
 */
void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color_t clear)
{
    _opengl3_renderer_internal__g.viewport_w      = viewport_w;
    _opengl3_renderer_internal__g.viewport_h      = viewport_h;
    _opengl3_renderer_internal__g.vert_count      = 0;
    _opengl3_renderer_internal__g.text_vert_count = 0;
    _opengl3_renderer_internal__g.text_run_count  = 0;
    // current_text_atlas is intentionally preserved across frames -- the
    // first font__draw call each frame rebinds it anyway. runs, though,
    // reset every frame: a run belongs to the frame that produced it.

    // the viewport is the rectangle (in pixels, bottom-left origin here)
    // that gl_Position values in NDC get mapped into.
    glViewport(0, 0, (GLsizei)viewport_w, (GLsizei)viewport_h);

    // Set the color glClear should fill the back buffer with.
    // sRGB framebuffer subtlety: with GL_FRAMEBUFFER_SRGB enabled, the
    // hardware treats glClearColor values as LINEAR and encodes them
    // to sRGB on store. Our gui_color_t values come from scene__hex /
    // scene__rgba, which produce sRGB-space channel values (0.039 for
    // hex 0x0a, etc.). We linearize them first so the cleared FB
    // actually lands at the intended sRGB color rather than at a much
    // brighter value.
    // pow(x, 2.2) is a fast approximation of the sRGB transfer curve.
    // Error < 0.01 across 0..1, well below 8-bit quantization.
    // Alpha is unchanged; sRGB encoding only applies to color.
    // the host's frame is already in the back buffer when adopted; nothing to clear
    if (_opengl3_renderer_internal__g.adopted == FALSE)
    {
        if (_opengl3_renderer_internal__g.srgb_fb)
        {
            float lr = stdlib__powf(clear.r, 2.2f);
            float lg = stdlib__powf(clear.g, 2.2f);
            float lb = stdlib__powf(clear.b, 2.2f);
            glClearColor(lr, lg, lb, clear.a);
        }
        else
        {
            glClearColor(clear.r, clear.g, clear.b, clear.a);
        }

        // do the actual clear.
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

// ============================================================================
// PUBLIC: renderer__submit_rect
// ============================================================================
// appends 6 vertices (one quad, two triangles) to the cpu-side staging
// buffer. does NOT touch the GPU -- that happens once at end_frame.

// Shadow: cheap multi-pass approximation. Emit `layers` concentric
// rects, each larger than the last by `blur / layers` pixels and
// with alpha = shadow.a * (1 - t)^2 where t = layer/(layers). The
// SDF rounded-box in the fragment shader smooths each layer's own
// edge; stacking them with decreasing alpha produces a visually
// acceptable feather without needing a separate blur shader.
// Not mathematically accurate (a real Gaussian would be smoother),
// but good enough for UI shadows and doesn't require any shader
// changes. Layer count scales with blur radius; 1px blur = 2 layers,
// 20px blur = 8 layers. Capped at 8 for performance.
/**
 * @brief queue a shadow splat for the current frame. Same rect convention as submit_rect. `blur` is the feather width in pixels. Caller supplies the final offset rect (rect.x already includes dx, rect.y includes dy) so the renderer doesn't have to know about the originating shape.
 *
 * @param gui_rect_t rect -> Shadow bounding rect (already offset).
 * @param gui_color_t c -> Shadow color (alpha controls strength).
 * @param float radius -> Same corner radius as the decorated shape.
 * @param float blur -> Soft-edge width in pixels. 0 = hard edge.
 *
 * @return void
 */
void renderer__submit_shadow(gui_rect_t rect, gui_color_t c, float radius, float blur)
{
    if (c.a <= 0.0f)
    {
        return;
    }
    int layers = (int)(blur * 0.4f) + 2;
    if (layers < 2)
    {
        layers = 2;
    }
    if (layers > 8)
    {
        layers = 8;
    }
    float step = blur / (float)layers;
    for (int i = 0; i < layers; i++)
    {
        float grow = step * (float)i;
        float t = (float)i / (float)(layers - 1);
        // quadratic falloff: inner layer opaque, outer layer faded.
        // produces a softer tail than linear.
        float falloff = (1.0f - t) * (1.0f - t);
        gui_color_t layer_c = c;
        layer_c.a = c.a * falloff / (float)layers;
        gui_rect_t lr;
        lr.x = rect.x - grow;
        lr.y = rect.y - grow;
        lr.w = rect.w + 2.0f * grow;
        lr.h = rect.h + 2.0f * grow;
        renderer__submit_rect(lr, layer_c, radius + grow);
    }
}

// Gradients. The shared tessellator (src/gradient.c) turns a
// gui_gradient_t into triangles with exact per-vertex colors; they go
// through the rect pipeline, whose SDF mask rounds the corners and
// anti-aliases the edge. The two-color entry point is the old API kept
// for callers with a direction keyword.
/**
 * @brief two-color linear gradient rect. Drop-in replacement for submit_rect when a gradient is requested. `direction` selects the axis via gui_gradient_dir_e.
 *
 * @param gui_rect_t r -> Rect in top-left-origin pixel coords.
 * @param gui_color_t from -> Color at the start of the axis.
 * @param gui_color_t to -> Color at the end of the axis.
 * @param int direction -> gui_gradient_dir_e value. Cast to int for ABI stability across the DLL boundary.
 * @param float radius -> Corner radius in pixels.
 *
 * @return void
 */
void renderer__submit_rect_gradient(gui_rect_t r, gui_color_t from, gui_color_t to, int direction, float radius)
{
    gui_gradient_t g;

    stdlib__null_memory(&g, sizeof(g));
    gradient__two_color(&g, from, to, (gui_gradient_dir_e)direction);
    renderer__submit_gradient(r, &g, 1.0f, 1.0f, radius);
}

/**
 * @brief a CSS gradient over a rect: tessellated by gradient__tessellate (shared by every renderer, so the pixels agree) and pushed through the rect pipeline like a solid rect, which applies the rounded-corner mask and the anti-aliased edge to the triangles.
 *
 * @param gui_rect_t r -> Rect in top-left-origin pixel coords.
 * @param const gui_gradient_t* g -> The gradient (linear or radial, with its stops).
 * @param float scale -> The UI scale, for pixel lengths inside the gradient.
 * @param float opacity -> Multiplies every stop's alpha.
 * @param float radius -> Corner radius in pixels.
 *
 * @return void
 */
GUI_API boole renderer_backend__presents_with_vsync(void)
{
    return FALSE;
}

void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color)
{
    gui_vec2_t vertices[3] = {a, b, c};
    float left = stdlib__fminf(a.x, stdlib__fminf(b.x, c.x)) - 1;
    float top = stdlib__fminf(a.y, stdlib__fminf(b.y, c.y)) - 1;
    float width = stdlib__fmaxf(a.x, stdlib__fmaxf(b.x, c.x)) - left + 1;
    float height = stdlib__fmaxf(a.y, stdlib__fmaxf(b.y, c.y)) - top + 1;
    int i;
    if (_opengl3_renderer_internal__g.vert_count + 3 > _opengl3_renderer_internal__g.vert_cap) return;
    // Expand the rectangle mask past the geometry. Triangle rasterization
    // owns the edges; the existing colour pipeline needs no extra shader.
    for (i = 0; i < 3; i++)
        _opengl3_renderer_internal__push_vertex(vertices[i].x, vertices[i].y,
            vertices[i].x - left, vertices[i].y - top, width, height, 0, color);
}

void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    int64 count = 0;
    int64 i     = 0;
    float max_r = 0.0f;

    count = gradient__tessellate(g, r, scale, opacity, verts, GRADIENT_MAX_VERTS);

    if (count <= 0 || _opengl3_renderer_internal__g.vert_count + count > _opengl3_renderer_internal__g.vert_cap)
    {
        return;
    }

    max_r = (r.w < r.h ? r.w : r.h) * 0.5f;

    if (radius < 0.0f)
    {
        radius = 0.0f;
    }

    if (radius > max_r)
    {
        radius = max_r;
    }

    for (i = 0; i < count; i++)
    {
        _opengl3_renderer_internal__push_vertex(verts[i].x, verts[i].y, verts[i].x - r.x, verts[i].y - r.y, r.w, r.h, radius, verts[i].c);
    }
}

/**
 * @brief draw a background layer as a grid of tiles clipped to a rounded box (renderer_backend.h)
 * @param gui_rect_t box -> the node's box in device pixels: the rounded-box mask every tile is clipped to
 * @param float radius -> corner radius of the box
 * @param gui_rect_t tile -> the first tile; the grid steps by its size to the right and downwards
 * @param int columns -> tiles per row
 * @param int rows -> tile rows
 * @param const gui_gradient_t* gradient -> the tile's gradient, or NULL for a solid tile
 * @param gui_color_t color -> the solid tile color
 * @param float scale -> logical scale for the gradient's pixel stops
 * @param float opacity -> multiplies every vertex alpha
 * @return void
 */
GUI_API void renderer_backend__submit_tiles(gui_rect_t box, float radius, gui_rect_t tile, int columns, int rows, const gui_gradient_t* gradient, gui_color_t color, float scale, float opacity)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    int64 count = 0;
    int64 i = 0;
    int column = 0;
    int row = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    float max_r = (box.w < box.h ? box.w : box.h) * 0.5f;

    if (columns <= 0 || rows <= 0 || tile.w <= 0.0f || tile.h <= 0.0f)
    {
        return;
    }
    if (radius < 0.0f)
    {
        radius = 0.0f;
    }
    if (radius > max_r)
    {
        radius = max_r;
    }
    if (gradient != NULL_POINTER)
    {
        count = gradient__tessellate(gradient, tile, scale, opacity, verts, GRADIENT_MAX_VERTS);
    }
    else
    {
        // the two triangles of renderer__submit_rect
        color.a *= opacity;
        for (i = 0; i < 6; i++)
        {
            verts[i].x = tile.x + ((i == 1 || i == 3 || i == 4) ? tile.w : 0.0f);
            verts[i].y = tile.y + ((i == 2 || i == 4 || i == 5) ? tile.h : 0.0f);
            verts[i].c = color;
        }
        count = 6;
    }
    if (count <= 0)
    {
        return;
    }
    for (row = 0; row < rows; row++)
    {
        dy = (float)row * tile.h;
        for (column = 0; column < columns; column++)
        {
            dx = (float)column * tile.w;
            // a full batch is drawn before the next tile; the flush keeps
            // the submit order (every pending rect and glyph goes first)
            if (_opengl3_renderer_internal__g.vert_count + count > _opengl3_renderer_internal__g.vert_cap)
            {
                _opengl3_renderer_internal__flush_batches();
                _opengl3_renderer_internal__tile_flushes++;
            }
            if (_opengl3_renderer_internal__g.vert_count + count > _opengl3_renderer_internal__g.vert_cap)
            {
                return;
            }
            _opengl3_renderer_internal__tile_vertices += (uint64)count;
            for (i = 0; i < count; i++)
            {
                _opengl3_renderer_internal__push_vertex(verts[i].x + dx, verts[i].y + dy, verts[i].x + dx - box.x, verts[i].y + dy - box.y, box.w, box.h, radius, verts[i].c);
            }
        }
    }
}

/**
 * @brief queue one rectangle for the current frame. Radius == 0 produces square corners; positive values are clamped to half the smaller side.
 *
 * @param gui_rect_t r -> Rectangle in top-left-origin pixel coords.
 * @param gui_color_t c -> Fill color (linear RGBA, 0..1).
 * @param float radius -> Corner radius in pixels.
 *
 * @return void
 */
void renderer__submit_rect(gui_rect_t r, gui_color_t c, float radius)
{
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    if (_opengl3_renderer_internal__g.vert_count + _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD > _opengl3_renderer_internal__g.vert_cap)
    {
        return;
    }

    // clamp radius. the SDF math goes nuts if radius > min(w,h)/2 (the
    // corners would overlap and produce garbage). this clamp MUST match
    // every other backend (renderer.h VISUAL CONTRACT).
    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f)
    {
        radius = 0.0f;
    }
    if (radius > max_r)
    {
        radius = max_r;
    }

    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

    // Negative encoding identifies a solid axis-aligned quad to the shader.
    // It expands raster coverage and applies our top/left-inclusive pixel
    // ownership explicitly; OpenGL's edge convention otherwise loses/adds a
    // row when a control lands on a half pixel at fractional display scales.
    // Gradient meshes and raw triangles keep nonnegative radii and geometry.
    radius = -1.0f - radius;

    // triangle 1: (x0,y0) (x1,y0) (x0,y1)
    // triangle 2: (x1,y0) (x1,y1) (x0,y1)
    // x   y   lx    ly    rect_w  rect_h  radius  color
    _opengl3_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w,    r.h,    radius, c); // top-left
    _opengl3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w,    r.h,    radius, c); // top-right
    _opengl3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w,    r.h,    radius, c); // bottom-left
    _opengl3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w,    r.h,    radius, c); // top-right
    _opengl3_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w,    r.h,    radius, c); // bottom-right
    _opengl3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w,    r.h,    radius, c); // bottom-left
}

// ============================================================================
// INTERNAL: flush_batches
// ============================================================================
// Draw whatever is currently buffered (solid rects + text glyphs), then
// reset the buffer counts so subsequent submissions start a fresh batch.
// Used in two places: (1) end_frame, to draw the final batch before
// SwapBuffers; (2) push_scissor / pop_scissor, to make sure the rects
// submitted before the scissor change get drawn under the OLD scissor
// state, not the new one.
// Does NOT call SwapBuffers -- that's end_frame's job.

static void _opengl3_renderer_internal__flush_batches(void)
{
    // PASS 1: solid rects (SDF rounded boxes). drawn first so text lands
    // on top of backgrounds in the same flush.
    if (_opengl3_renderer_internal__g.vert_count > 0)
    {
        glUseProgram(_opengl3_renderer_internal__g.program);
        glUniform2f(_opengl3_renderer_internal__g.u_viewport_loc, (float)_opengl3_renderer_internal__g.viewport_w, (float)_opengl3_renderer_internal__g.viewport_h);

        glBindVertexArray(_opengl3_renderer_internal__g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.vbo);

        // Buffer ORPHANING (critical for mid-frame multi-flush correctness):
        // We call glBufferData with NULL data first, which tells the driver
        // "throw away the previous contents -- I don't need them anymore,
        // give me fresh GPU memory". Then glBufferSubData uploads our new
        // verts into that fresh memory.
        // Without this, when push_scissor / pop_scissor causes multiple
        // flushes per frame, the SECOND glBufferSubData would have to wait
        // for the FIRST glDrawArrays to finish reading from the buffer
        // before it could safely overwrite -- or worse, on some drivers
        // it would NOT wait properly and produce flicker as the second
        // upload races the first draw. (D3D11's MAP_WRITE_DISCARD and
        // D3D9's D3DLOCK_DISCARD are the equivalent "orphan" hints --
        // the d3d backends never had this bug.)
        // The "orphan" cost is negligible: drivers internally pool old
        // memory and recycle it once the GPU is done. A typical driver
        // handles thousands of orphans per second without breaking a sweat.
        GLsizeiptr bytes = (GLsizeiptr)_opengl3_renderer_internal__g.vert_count * (GLsizeiptr)sizeof(_opengl3_renderer_internal__vertex_t);
        GLsizeiptr cap_bytes = (GLsizeiptr)_opengl3_renderer_internal__g.vert_cap * (GLsizeiptr)sizeof(_opengl3_renderer_internal__vertex_t);
        glBufferData(GL_ARRAY_BUFFER, cap_bytes, NULL_POINTER, GL_DYNAMIC_DRAW); // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, _opengl3_renderer_internal__g.cpu_verts);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)_opengl3_renderer_internal__g.vert_count);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);

        _opengl3_renderer_internal__g.vert_count = 0;
    }

    // PASS 2: text. one draw per atlas-run so a flush can mix fonts of
    // different sizes (each (family, size) rasterizes into its own
    // atlas texture). all runs share the same VAO/VBO/program; only
    // the bound texture changes between runs.
    if (_opengl3_renderer_internal__g.text_vert_count > 0 && _opengl3_renderer_internal__g.text_run_count > 0)
    {
        glUseProgram(_opengl3_renderer_internal__g.text_program);
        glUniform2f(_opengl3_renderer_internal__g.text_u_viewport_loc, (float)_opengl3_renderer_internal__g.viewport_w, (float)_opengl3_renderer_internal__g.viewport_h);

        glActiveTexture(GL_TEXTURE0);
        if (_opengl3_renderer_internal__g.text_u_atlas_loc >= 0)
        {
            glUniform1i(_opengl3_renderer_internal__g.text_u_atlas_loc, 0);
        }

        glBindVertexArray(_opengl3_renderer_internal__g.text_vao);
        glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.text_vbo);

        // Same orphan trick as the solid-rect pass above.
        GLsizeiptr tbytes = (GLsizeiptr)_opengl3_renderer_internal__g.text_vert_count * (GLsizeiptr)sizeof(_opengl3_renderer_internal__text_vertex_t);
        GLsizeiptr tcap_bytes = (GLsizeiptr)_opengl3_renderer_internal__g.text_vert_cap * (GLsizeiptr)sizeof(_opengl3_renderer_internal__text_vertex_t);
        glBufferData(GL_ARRAY_BUFFER, tcap_bytes, NULL_POINTER, GL_DYNAMIC_DRAW); // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, tbytes, _opengl3_renderer_internal__g.text_cpu_verts);

        for (int64 i = 0; i < _opengl3_renderer_internal__g.text_run_count; i++)
        {
            _opengl3_renderer_internal__text_run_t* run = &_opengl3_renderer_internal__g.text_runs[i];
            if (run->vert_count <= 0)
            {
                continue;
            }
            glBindTexture(GL_TEXTURE_2D, run->atlas);
            glDrawArrays(GL_TRIANGLES, (GLint)run->vert_start, (GLsizei)run->vert_count);
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        _opengl3_renderer_internal__g.text_vert_count = 0;
        _opengl3_renderer_internal__g.text_run_count  = 0;
        _opengl3_renderer_internal__g.current_text_atlas = 0;
    }
}

// ============================================================================
// PUBLIC: renderer__push_scissor / renderer__pop_scissor
// ============================================================================
// Bracket a region of submissions whose draws are clipped to the given
// rect. Each push/pop forces a flush of whatever is currently batched,
// then changes GL scissor state. Subsequent submissions accumulate into
// a fresh batch that will be drawn under the new state.
// OpenGL's glScissor uses BOTTOM-LEFT-origin coordinates, so we flip Y
// from our top-left convention: gl_y = viewport_h - top_y - height.
// Nesting takes the INTERSECTION of all stacked rects. A scrollable
// child div inside a scrollable parent div correctly clips to the
// smaller of the two regions.

static gui_rect_t _opengl3_renderer_internal__intersect(gui_rect_t a, gui_rect_t b)
{
    float x0 = (a.x > b.x) ? a.x : b.x;
    float y0 = (a.y > b.y) ? a.y : b.y;
    float x1a = a.x + a.w;
    float y1a = a.y + a.h;
    float x1b = b.x + b.w;
    float y1b = b.y + b.h;
    float x1 = (x1a < x1b) ? x1a : x1b;
    float y1 = (y1a < y1b) ? y1a : y1b;
    gui_rect_t r;
    r.x = x0;
    r.y = y0;
    r.w = (x1 > x0) ? (x1 - x0) : 0.0f;
    r.h = (y1 > y0) ? (y1 - y0) : 0.0f;
    return r;
}

static void _opengl3_renderer_internal__apply_scissor_top(void)
{
    // Compute the intersection of every rect on the stack. The result
    // is what the GPU's scissor test will use until the next change.
    if (_opengl3_renderer_internal__g.scissor_depth <= 0)
    {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    gui_rect_t r = _opengl3_renderer_internal__g.scissor_stack[0];
    for (int64 i = 1; i < _opengl3_renderer_internal__g.scissor_depth; i++)
    {
        r = _opengl3_renderer_internal__intersect(r, _opengl3_renderer_internal__g.scissor_stack[i]);
    }
    GLint   gl_x = (GLint)r.x;
    // Quantize top-left edges before flipping Y, matching the Direct3D scissor bounds.
    GLint   top = (GLint)r.y;
    GLint   right = (GLint)(r.x + r.w);
    GLint   bottom = (GLint)(r.y + r.h);
    GLint   gl_y = (GLint)_opengl3_renderer_internal__g.viewport_h - bottom;
    GLsizei gl_w = right - gl_x;
    GLsizei gl_h = bottom - top;
    if (gl_w < 0)
    {
        gl_w = 0;
    }
    if (gl_h < 0)
    {
        gl_h = 0;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(gl_x, gl_y, gl_w, gl_h);
}

/**
 * @brief push a clip rect onto the scissor stack. Subsequent submissions are clipped to the intersection of every rect currently on the stack.
 *
 * @param gui_rect_t rect -> Clip region in pixel coords (top-left origin).
 *
 * @return void
 */
void renderer__push_scissor(gui_rect_t rect)
{
    _opengl3_renderer_internal__flush_batches();
    if (_opengl3_renderer_internal__g.scissor_depth >= (int64)(sizeof(_opengl3_renderer_internal__g.scissor_stack) / sizeof(_opengl3_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("%s", "renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    _opengl3_renderer_internal__g.scissor_stack[_opengl3_renderer_internal__g.scissor_depth++] = rect;
    _opengl3_renderer_internal__apply_scissor_top();
}

/**
 * @brief pop the most recent scissor rect off the stack. If the stack is now empty, scissor testing is disabled entirely (everything draws).
 *
 * @return void
 */
void renderer__pop_scissor(void)
{
    _opengl3_renderer_internal__flush_batches();
    if (_opengl3_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("%s", "renderer__pop_scissor: stack already empty");
        return;
    }
    _opengl3_renderer_internal__g.scissor_depth--;
    _opengl3_renderer_internal__apply_scissor_top();
}

// Real backdrop blur for opengl3. glReadPixels the backbuffer region,
// run a CPU-side separable Gaussian over the captured pixels, upload
// the result as a temp RGBA8 texture, draw it back at the same rect.
// Not the fastest possible implementation (GPU-side FBO ping-pong
// would beat it), but this path has no new shader code and drops in
// on any GL context that can glReadPixels -- which is every desktop
// GL 3.x profile. Rect sizes typical for UI panels (200-600 px wide,
// 200-400 px tall) keep the per-frame cost low enough to not register
// on a frame timer at sigma <= 16.
// The other backends keep the translucent-darken placeholder until
// they gain a comparable path (each has its own readback + temp-
// texture machinery to wire up).
/**
 * @brief capture the framebuffer region under `rect`, apply a separable Gaussian blur with standard deviation `sigma_px`, and paint the blurred result back over `rect`. Subsequent submissions draw on top of the blurred backdrop, as the caller intends (e.g. text rendered legibly above a frosted panel background).
 *
 * @param gui_rect_t rect -> Region to blur, in pixel coords.
 * @param float sigma_px -> Gaussian standard deviation in pixels. Values under 1.0 produce negligible blur; 4-16 is the "frosted glass" range.
 *
 * @return void
 */
void renderer__blur_region(gui_rect_t rect, float sigma_px)
{
    if (sigma_px <= 0.0f)
    {
        return;
    }
    if (rect.w <= 0.0f || rect.h <= 0.0f)
    {
        return;
    }

    // Flush anything batched so the backbuffer reflects every
    // submission that happened BEFORE this blur call. Otherwise the
    // pixels we read back would be stale ("before the bg of the
    // div that the blur is inside" instead of "including it").
    _opengl3_renderer_internal__flush_batches();

    int vw = (int)_opengl3_renderer_internal__g.viewport_w;
    int vh = (int)_opengl3_renderer_internal__g.viewport_h;

    // Clamp the blur rect to the viewport. Going past the edges
    // would feed glReadPixels coordinates outside the framebuffer
    // (undefined pixel values, often black) and would waste work on
    // regions that won't be composited.
    int x = (int)rect.x;
    int y = (int)rect.y;
    int w = (int)rect.w;
    int h = (int)rect.h;
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > vw)
    {
        w = vw - x;
    }
    if (y + h > vh)
    {
        h = vh - y;
    }
    if (w <= 0 || h <= 0)
    {
        return;
    }

    // GL's framebuffer origin is bottom-left, ours is top-left, so the
    // readback Y is mirrored. glReadPixels returns rows starting at the
    // requested y and going UP; that means the FIRST row corresponds
    // to the BOTTOM of our screen rect. We'll flip the pixel buffer
    // back to top-first before uploading so the regular image UV
    // convention (V=0 at top) lines up.
    int gl_y = vh - (y + h);

    size_t pxbytes = (size_t)w * (size_t)h * 4;
    ubyte* pixels = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE__RENDERER);
    ubyte* tmp    = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE__RENDERER);
    if (pixels == NULL_POINTER || tmp == NULL_POINTER)
    {
        if (pixels)
        {
            GUI_FREE(pixels);
        }
        if (tmp)
        {
            GUI_FREE(tmp);
        }
        // OOM for the blur buffers: fall back to the darken placeholder
        // so at least something renders.
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x, gl_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Run the separable Gaussian on the readback buffer, then flip
    // rows so the top of the buffer matches our top-left origin
    // (glReadPixels returns bottom-left origin). The flip uses a
    // single-row scratch that we carve out of `tmp`.
    blur_cpu__apply_gaussian(pixels, tmp, w, h, sigma_px);
    blur_cpu__flip_rows_v(pixels, tmp, w, h);

    // Upload as a throw-away RGBA8 texture and draw it at the blur
    // rect using the existing image pipeline. Force no mipmapping
    // (single level) since we're rendering 1:1 and mipmap generation
    // would be wasted cost. GL refcount-defers the deletion until
    // the draw finishes, so destroying immediately after the draw
    // call is safe per the GL spec.
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Draw at the (possibly clamped) rect. Tint = white so the image
    // displays at full opacity. submit_image flushes any pending
    // batches itself, then draws the textured quad.
    gui_rect_t draw_rect;
    draw_rect.x = (float)x;
    draw_rect.y = (float)y;
    draw_rect.w = (float)w;
    draw_rect.h = (float)h;
    gui_color_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
    renderer__submit_image(draw_rect, (void*)(uintptr_t)tex, white);

    glDeleteTextures(1, &tex);
    GUI_FREE(tmp);
    GUI_FREE(pixels);
}

// ============================================================================
// PUBLIC: renderer__end_frame
// ============================================================================
// Flushes any remaining batched submissions, then SwapBuffers to promote
// the back buffer to front (= to screen). Also defensively resets the
// scissor stack -- a missing pop_scissor in widget code would otherwise
// leak into the next frame.

/**
 * @brief drain the pending rect + text batches mid-frame in the correct order (rects first, then text-on-top), WITHOUT changing scissor state or presenting. After this call the GPU has painted everything submitted so far, and subsequent submissions start a fresh batch that will draw ON TOP of what's already on the back buffer.
 *
 * @return void
 */
void renderer__flush_pending_draws(void)
{
    // Public thin wrapper over the internal flush so scene_render can
    // break the per-frame batch at z-index sibling boundaries. See
    // renderer.h for why this matters for text+z interleaving.
    _opengl3_renderer_internal__flush_batches();
}

/**
 * @brief finalize the frame: upload the batched verts, issue the draw, then present the back buffer to the window (swap buffers / swapchain Present). After this call returns, the frame is on screen.
 *
 * @return void
 */
void renderer__end_frame(void)
{
    _opengl3_renderer_internal__flush_batches();

    // defensive scissor cleanup: if a widget pushed but failed to pop,
    // the next frame would inherit the stale scissor. clear and disable.
    if (_opengl3_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("%s %lld %s", "renderer__end_frame: scissor stack non-empty (", (long long)_opengl3_renderer_internal__g.scissor_depth, "); resetting");
        _opengl3_renderer_internal__g.scissor_depth = 0;
    }
    glDisable(GL_SCISSOR_TEST);

    _opengl3_renderer_internal__capture_if_requested();

    // present. SwapBuffers takes the just-rendered back buffer and makes
    // it the visible front buffer. this is what actually puts pixels on
    // screen; everything above this line only wrote to an offscreen
    // buffer. even when both passes were skipped (empty frame), we still
    // present so the clear color reaches the display.
    if (_opengl3_renderer_internal__g.adopted == FALSE)
    {
#if defined(_WIN32)
        SwapBuffers(_opengl3_renderer_internal__g.hdc);
#endif
    }
}

// ===== frame capture state (see renderer.h) ================================
static ubyte* _opengl3_renderer_internal__capture_out  = NULL_POINTER;
static int64  _opengl3_renderer_internal__capture_cap  = 0;
static boole  _opengl3_renderer_internal__capture_done = FALSE;

// The armed frame capture: glReadPixels of the whole back buffer, rows
// flipped from GL's bottom-up order to top-down while copying out.
static void _opengl3_renderer_internal__capture_if_requested(void)
{
    int64  w   = (int64)_opengl3_renderer_internal__g.viewport_w;
    int64  h   = (int64)_opengl3_renderer_internal__g.viewport_h;
    ubyte* tmp = NULL_POINTER;
    int64  y   = 0;

    if (_opengl3_renderer_internal__capture_out == NULL_POINTER || w <= 0 || h <= 0 || _opengl3_renderer_internal__capture_cap < w * h * 4)
    {
        return;
    }

    tmp = (ubyte*)GUI_MALLOC_T((size_t)(w * h * 4), MM_TYPE__RENDERER);

    if (tmp == NULL_POINTER)
    {
        _opengl3_renderer_internal__capture_out = NULL_POINTER;
        return;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);

    for (y = 0; y < h; y++)
    {
        stdlib__memcpy(_opengl3_renderer_internal__capture_out + (size_t)y * (size_t)w * 4, tmp + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
    }

    GUI_FREE(tmp);
    _opengl3_renderer_internal__capture_out  = NULL_POINTER;
    _opengl3_renderer_internal__capture_done = TRUE;
}

// ============================================================================
// INTERNAL: context creation
// ============================================================================

// pick a pixel format and set it on the HDC. this establishes the back
// buffer's properties (RGBA8, 24-bit depth + 8-bit stencil, double-buffered).
// must happen before any context creation. irreversible -- the window is
// locked into this format after SetPixelFormat.
// Build the legacy PIXELFORMATDESCRIPTOR we use both for the sRGB
// path's DescribePixelFormat fill-in and for the non-sRGB fallback's
// ChoosePixelFormat call. Single source of truth for the fields.

#if defined(_WIN32)
static void _opengl3_renderer_internal__fill_legacy_pfd(PIXELFORMATDESCRIPTOR* pfd)
{
    ZeroMemory(pfd, sizeof(*pfd));
    pfd->nSize        = sizeof(*pfd);
    pfd->nVersion     = 1;
    pfd->dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->iPixelType   = PFD_TYPE_RGBA;
    pfd->cColorBits   = 32;
    // cAlphaBits = 0 (NOT 8). The Windows DWM compositor treats a
    // non-zero alpha channel in the framebuffer as window transparency
    // and samples it during composition. Even though our blend func
    // (glBlendFuncSeparate with GL_ONE / GL_ONE_MINUS_SRC_ALPHA on the
    // alpha channel) keeps dst.a pinned to the cleared value of 1.0
    // in steady state, intermediate draws within a frame can briefly
    // produce dst.a values that DWM samples mid-composition, producing
    // visible flicker as our :appear animation modulates per-frame
    // alpha. Setting cAlphaBits = 0 makes DWM completely ignore the
    // framebuffer alpha and treat the surface as opaque -- which is
    // what we want, since the toolkit doesn't render to a transparent
    // window. The d3d11 / d3d9 backends use DXGI_FORMAT_R8G8B8A8_UNORM /
    // D3DFMT_A8R8G8B8 (both have alpha) but their swap chains don't
    // hand the alpha to DWM the same way -- which is why those
    // backends don't show this bug.
    pfd->cAlphaBits   = 0;
    pfd->cDepthBits   = 24;
    pfd->cStencilBits = 8;
    pfd->iLayerType   = PFD_MAIN_PLANE;
}
#endif


// Resolve wglChoosePixelFormatARB. The extension is only queryable
// while a GL context is current, and a GL context requires a pixel
// format to already be set on its HDC -- chicken and egg. The
// idiomatic workaround: stand up a COMPLETELY SEPARATE throwaway
// window with a legacy pixel format + legacy 2.x context just long
// enough to call wglGetProcAddress, then tear the whole thing down.
// The main window's HDC is untouched, so SetPixelFormat can be called
// on it with whatever format wglChoosePixelFormatARB returned.
// Returns NULL if the extension isn't available (very old drivers).
// Caller falls back to the plain ChoosePixelFormat path in that case.

#if defined(_WIN32)
static fncp_wglChoosePixelFormatARB _opengl3_renderer_internal__resolve_wgl_pixel_format_arb(void)
{
    // Register the dummy class exactly once per process. Two reasons:
    // 1. If renderer__init is called twice (re-init after shutdown),
    // a fresh RegisterClassW on a previously-registered class
    // fails with ERROR_CLASS_ALREADY_EXISTS and we'd bail early.
    // 2. UnregisterClassW can fail if ANY window of that class is
    // still alive, which isn't a guarantee we can make cleanly
    // while the GL context is being stood up. Keeping the class
    // alive for the process lifetime avoids the race.
    // The leak is ~one WNDCLASS entry per process -- negligible.
    static ATOM          s_dummy_class_atom   = 0;
    static const wchar_t s_dummy_class_name[] = L"GuiOpenGL3DummyClass";

    if (s_dummy_class_atom == 0)
    {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(NULL_POINTER);
        wc.lpszClassName = s_dummy_class_name;
        s_dummy_class_atom = RegisterClassW(&wc);
        if (s_dummy_class_atom == 0)
        {
            // Non-fatal; just means we can't resolve the ARB extension.
            // Caller falls back to the legacy ChoosePixelFormat path.
            return NULL_POINTER;
        }
    }

    fncp_wglChoosePixelFormatARB result = NULL_POINTER;

    HWND dummy_hwnd = CreateWindowW( s_dummy_class_name, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, NULL_POINTER, NULL_POINTER, GetModuleHandleW(NULL_POINTER), NULL_POINTER);
    if (dummy_hwnd == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    HDC dummy_hdc = GetDC(dummy_hwnd);
    if (dummy_hdc != NULL_POINTER)
    {
        PIXELFORMATDESCRIPTOR pfd;
        _opengl3_renderer_internal__fill_legacy_pfd(&pfd);
        int pf = ChoosePixelFormat(dummy_hdc, &pfd);
        if (pf != 0 && SetPixelFormat(dummy_hdc, pf, &pfd))
        {
            HGLRC dummy_ctx = wglCreateContext(dummy_hdc);
            if (dummy_ctx != NULL_POINTER)
            {
                if (wglMakeCurrent(dummy_hdc, dummy_ctx))
                {
                    result = (fncp_wglChoosePixelFormatARB)wglGetProcAddress("wglChoosePixelFormatARB");
                    wglMakeCurrent(NULL_POINTER, NULL_POINTER);
                }
                wglDeleteContext(dummy_ctx);
            }
        }
        ReleaseDC(dummy_hwnd, dummy_hdc);
    }

    DestroyWindow(dummy_hwnd);
    return result;
}
#endif


// Select a pixel format for the main HDC. Preferred path is
// wglChoosePixelFormatARB with WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB = TRUE,
// which gives us an sRGB default framebuffer -- the hardware then
// does linear->sRGB on every pixel write and blending happens in
// physically-correct linear space. Fallback path is the legacy
// ChoosePixelFormat with the same PFD as before this refactor; the
// renderer then stays on non-sRGB blending (same pixel output as
// pre-refactor builds).
// _opengl3_renderer_internal__g.srgb_fb is set to TRUE only when the
// ARB path succeeded AND DescribePixelFormat + SetPixelFormat also
// succeeded. Everywhere else in the renderer reads that flag to
// decide whether to linearize clear colors, enable GL_FRAMEBUFFER_SRGB,
// and which shader variant to compile.

#if defined(_WIN32)
static boole _opengl3_renderer_internal__set_pixel_format(HDC hdc)
{
    fncp_wglChoosePixelFormatARB wglChoosePixelFormatARB_p = NULL_POINTER;

#if defined(MEEK_UI_OPENGL_SRGB_FRAMEBUFFER)
    wglChoosePixelFormatARB_p = _opengl3_renderer_internal__resolve_wgl_pixel_format_arb();
#endif

    // An sRGB framebuffer blends in linear light, which is not what the
    // other backends or a browser do: translucent colors and the backdrop
    // blur came out lighter on opengl3 than on d3d9 / d3d11 / d3d12 and
    // than in the HTML reference. The renderer contract (renderer.h) is
    // blending in sRGB space on a plain back buffer, so the sRGB path is
    // opt-in through MEEK_UI_OPENGL_SRGB_FRAMEBUFFER now.
    if (wglChoosePixelFormatARB_p != NULL_POINTER)
    {
        // Attribute list mirrors the legacy PFD's fields:
        // accelerated + double-buffered + RGBA + 24 color / 0 alpha /
        // 24 depth / 8 stencil + sRGB-capable.
        // Ending in 0 (not two 0s; wgl terminator is a single int
        // entry) stops the driver from reading past our array.
        int attribs[] = {
            WGL_DRAW_TO_WINDOW_ARB,           1,
            WGL_SUPPORT_OPENGL_ARB,           1,
            WGL_DOUBLE_BUFFER_ARB,            1,
            WGL_ACCELERATION_ARB,             WGL_FULL_ACCELERATION_ARB,
            WGL_PIXEL_TYPE_ARB,               WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB,               24,
            WGL_ALPHA_BITS_ARB,               0,
            WGL_DEPTH_BITS_ARB,               24,
            WGL_STENCIL_BITS_ARB,             8,
            WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB, 1,
            0
        };

        int  pf  = 0;
        UINT num = 0;
        if (wglChoosePixelFormatARB_p(hdc, attribs, NULL_POINTER, 1, &pf, &num) && num > 0)
        {
            // DescribePixelFormat fills the PFD fields SetPixelFormat
            // actually reads (most are advisory; the pixel-format index
            // is what binds). Required by the SetPixelFormat API even
            // when the format was picked by the ARB extension.
            PIXELFORMATDESCRIPTOR pfd;
            ZeroMemory(&pfd, sizeof(pfd));
            pfd.nSize    = sizeof(pfd);
            pfd.nVersion = 1;
            DescribePixelFormat(hdc, pf, sizeof(pfd), &pfd);

            if (SetPixelFormat(hdc, pf, &pfd))
            {
                _opengl3_renderer_internal__g.srgb_fb = TRUE;
                log_info("%s %d %s", "opengl3: sRGB-capable pixel format selected (index", pf, ")");
                return TRUE;
            }
            _opengl3_renderer_internal__log_win32_error("SetPixelFormat (sRGB)");
            // fall through to legacy path; don't bail -- maybe the
            // legacy ChoosePixelFormat can still give us something.
        }
        else
        {
            log_warn("%s", "opengl3: wglChoosePixelFormatARB returned no sRGB format; falling back to legacy");
        }
    }

    // Legacy fallback: no sRGB. Same PFD + ChoosePixelFormat path as
    // before this refactor. srgb_fb stays FALSE.
    PIXELFORMATDESCRIPTOR pfd;
    _opengl3_renderer_internal__fill_legacy_pfd(&pfd);
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (pf == 0)
    {
        _opengl3_renderer_internal__log_win32_error("ChoosePixelFormat");
        return FALSE;
    }
    if (SetPixelFormat(hdc, pf, &pfd) == FALSE)
    {
        _opengl3_renderer_internal__log_win32_error("SetPixelFormat");
        return FALSE;
    }
    log_info("%s %d %s", "opengl3: legacy (non-sRGB) pixel format selected (index", pf, "); text will use pow-coverage gamma approximation");
    return TRUE;
}
#endif


// the dummy-context dance. a "dummy" 2.x context is created first,
// made current, used to resolve wglCreateContextAttribsARB, then
// thrown away once we have the real 3.3 core context. gross but
// that's how wgl works.

#if defined(_WIN32)
static boole _opengl3_renderer_internal__create_core_context(HDC hdc, HGLRC* out_ctx)
{
    HGLRC dummy = wglCreateContext(hdc);
    if (dummy == NULL_POINTER)
    {
        _opengl3_renderer_internal__log_win32_error("wglCreateContext (dummy)");
        return FALSE;
    }
    if (wglMakeCurrent(hdc, dummy) == FALSE)
    {
        _opengl3_renderer_internal__log_win32_error("wglMakeCurrent (dummy)");
        wglDeleteContext(dummy);
        return FALSE;
    }

    fncp_wglCreateContextAttribsARB wglCreateContextAttribsARB = (fncp_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");

    if (wglCreateContextAttribsARB == NULL_POINTER)
    {
        // driver doesn't expose the arb extension. fall back to the
        // legacy context -- still usable, though some 3.3-only calls
        // may not work.
        log_warn("%s", "wglCreateContextAttribsARB not available; using legacy context");
        *out_ctx = dummy;
        wglMakeCurrent(NULL_POINTER, NULL_POINTER);
        return TRUE;
    }

    const int attribs[] =
    {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    HGLRC real = wglCreateContextAttribsARB(hdc, NULL_POINTER, attribs);
    wglMakeCurrent(NULL_POINTER, NULL_POINTER);
    wglDeleteContext(dummy);

    if (real == NULL_POINTER)
    {
        _opengl3_renderer_internal__log_win32_error("wglCreateContextAttribsARB");
        return FALSE;
    }
    *out_ctx = real;
    return TRUE;
}
#endif


// resolve every GL 3.x function pointer via wglGetProcAddress. gl 1.1
// functions (glClear, glViewport etc.) are linked statically against
// opengl32.lib and don't need this. returns FALSE on any missing entry
// point -- that indicates a broken driver.
static boole _opengl3_renderer_internal__load_gl_functions(void)
{
    boole ok = TRUE;

    // helper macro: given an identifier `name`, stringify it for
    // wglGetProcAddress, and build the matching extern pointer /
    // typedef names via ## token-paste.
    #define LOAD(name)                                                           \
        do                                                                       \
        {                                                                        \
            p_##name = (fncp_##name)renderer__opengl_proc_address(#name);                    \
            if (p_##name == NULL_POINTER)                                                \
            {                                                                    \
                log_error("%s %s", "OpenGL function lookup failed for", #name);                     \
                ok = FALSE;                                                      \
            }                                                                    \
        } while (0)

    LOAD(glCreateShader);
    LOAD(glDeleteShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);

    LOAD(glCreateProgram);
    LOAD(glDeleteProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glUseProgram);

    LOAD(glGetUniformLocation);
    LOAD(glUniform2f);
    LOAD(glUniform1i);
    LOAD(glUniform1f);
    LOAD(glGenFramebuffers);
    LOAD(glDeleteFramebuffers);
    LOAD(glBindFramebuffer);
    LOAD(glBlitFramebuffer);
    LOAD(glGetFramebufferAttachmentParameteriv);
    LOAD(glFramebufferTexture2D);
    LOAD(glCheckFramebufferStatus);
    LOAD(glActiveTexture);

    LOAD(glGenBuffers);
    LOAD(glDeleteBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glBufferSubData);

    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glDeleteVertexArrays);
    LOAD(glVertexAttribPointer);
    LOAD(glEnableVertexAttribArray);

    LOAD(glBlendFuncSeparate);
    LOAD(glGenerateMipmap);

    #undef LOAD

    return ok;
}

// ============================================================================
// INTERNAL: pipeline (shader + VAO + VBO + cpu buffer)
// ============================================================================
// once the GL context exists and function pointers are loaded, build the
// draw pipeline: compile + link shaders, create + configure the VAO/VBO,
// allocate the cpu staging buffer, enable alpha blending.

static boole _opengl3_renderer_internal__build_pipeline(void)
{
    // compile each shader stage independently. Fragment shader source
    // depends on whether we negotiated an sRGB-capable framebuffer --
    // see shader source comments above for the two variants.
    char* fs_src = _opengl3_renderer_internal__g.srgb_fb
                 ? _opengl3_renderer_internal__fs_src_srgb
                 : _opengl3_renderer_internal__fs_src;
    GLuint vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER, _opengl3_renderer_internal__vs_src);
    GLuint fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    // link the two stages into a program. delete the shader objects
    // afterwards; the program has copied what it needs from them.
    _opengl3_renderer_internal__g.program = _opengl3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_opengl3_renderer_internal__g.program == 0)
    {
        return FALSE;
    }

    // cache the u_viewport uniform's slot so we don't re-query each frame.
    _opengl3_renderer_internal__g.u_viewport_loc =
        glGetUniformLocation(_opengl3_renderer_internal__g.program, "u_viewport");

    // create + bind VAO and VBO. gl 3.3 core requires a VAO bound any
    // time you draw.
    glGenVertexArrays(1, &_opengl3_renderer_internal__g.vao);
    glGenBuffers(1, &_opengl3_renderer_internal__g.vbo);

    glBindVertexArray(_opengl3_renderer_internal__g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.vbo);

    // reserve the max buffer size up front. GL_DYNAMIC_DRAW tells the
    // driver we'll update this buffer often.
    GLsizeiptr max_bytes = (GLsizeiptr)(_OPENGL3_RENDERER_INTERNAL__MAX_QUADS* _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_opengl3_renderer_internal__vertex_t);
    glBufferData(GL_ARRAY_BUFFER, max_bytes, NULL_POINTER, GL_DYNAMIC_DRAW);

    // describe the vertex layout to the GPU. one glVertexAttribPointer
    // per attribute in our vertex struct; must match the shader's
    // layout(location=N) declarations.
    GLsizei stride = (GLsizei)sizeof(_opengl3_renderer_internal__vertex_t);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex_t, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex_t, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex_t, lx));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex_t, rect_w));

    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__vertex_t, radius));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // allocate the cpu-side staging buffer that mirrors the VBO.
    _opengl3_renderer_internal__g.vert_cap = _OPENGL3_RENDERER_INTERNAL__MAX_QUADS * _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _opengl3_renderer_internal__g.cpu_verts = (_opengl3_renderer_internal__vertex_t*)GUI_MALLOC_T((size_t)_opengl3_renderer_internal__g.vert_cap * sizeof(_opengl3_renderer_internal__vertex_t), MM_TYPE__RENDERER);
    if (_opengl3_renderer_internal__g.cpu_verts == NULL_POINTER)
    {
        log_error("%s", "out of memory for cpu vertex buffer");
        return FALSE;
    }

    // alpha blending. required for our anti-aliased rounded corners
    // to fade cleanly into the background. this blend mode is the
    // VISUAL CONTRACT for all backends -- see renderer.h.
    // Alpha blending. We use glBlendFuncSeparate (not the simpler
    // glBlendFunc) so that the framebuffer's ALPHA channel stays at
    // 1.0 across overlapping draws -- if it dropped below 1.0, the
    // Windows DWM compositor would treat the OpenGL window as
    // semi-transparent and let the desktop bleed through, producing
    // visible flicker as our own alpha animations modulate dst.a.
    // RGB:    src.a   * src.rgb +  (1 - src.a) * dst.rgb    (classic alpha blend)
    // Alpha:  1       * src.a   +  (1 - src.a) * dst.a       (preserves dst.a = 1)
    // Matches d3d11's separate-alpha blend state. Part of the VISUAL
    // CONTRACT in renderer.h.
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Turn on hardware sRGB encoding for the default framebuffer when
    // we got an sRGB-capable pixel format. With this enabled, shader
    // fragment outputs are interpreted as LINEAR color values and the
    // GPU does the linear->sRGB conversion on write -- which means
    // alpha blending runs in linear space (physically correct) and
    // partial-coverage edges land at the perceptual midpoint instead
    // of the sRGB-space midpoint.
    // Has no effect if the default FB isn't sRGB-capable (spec:
    // silently becomes a no-op), so the glEnable call alone wouldn't
    // hurt even in the fallback path -- we still guard it behind
    // srgb_fb to make the pairing with the shader variant explicit.
    if (_opengl3_renderer_internal__g.srgb_fb)
    {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    // the second pipeline: textured glyph quads. same blend state,
    // separate shader + VAO + VBO + cpu buffer.
    if (_opengl3_renderer_internal__build_text_pipeline() == FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

// build_text_pipeline: compile the text shaders, link them, create the
// text VAO/VBO, and allocate the cpu-side staging buffer. mirrors the
// solid-rect pipeline structure exactly; only the shader sources and
// vertex layout differ.
static boole _opengl3_renderer_internal__build_text_pipeline(void)
{
    // Text fragment shader variant depends on sRGB FB availability
    // just like the rect shader. Non-sRGB path keeps the legacy
    // pow(a, 1/2.2) coverage hack; sRGB path does sRGB->linear on
    // the vertex color and relies on the hardware for gamma.
    char* fs_src = _opengl3_renderer_internal__g.srgb_fb
                 ? _opengl3_renderer_internal__text_fs_src_srgb
                 : _opengl3_renderer_internal__text_fs_src;
    GLuint vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   _opengl3_renderer_internal__text_vs_src);
    GLuint fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    _opengl3_renderer_internal__g.text_program = _opengl3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_opengl3_renderer_internal__g.text_program == 0)
    {
        return FALSE;
    }

    _opengl3_renderer_internal__g.text_u_viewport_loc =
        glGetUniformLocation(_opengl3_renderer_internal__g.text_program, "u_viewport");
    _opengl3_renderer_internal__g.text_u_atlas_loc =
        glGetUniformLocation(_opengl3_renderer_internal__g.text_program, "u_atlas");

    glGenVertexArrays(1, &_opengl3_renderer_internal__g.text_vao);
    glGenBuffers(1,      &_opengl3_renderer_internal__g.text_vbo);

    glBindVertexArray(_opengl3_renderer_internal__g.text_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.text_vbo);

    GLsizeiptr max_bytes = (GLsizeiptr)(_OPENGL3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS* _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_opengl3_renderer_internal__text_vertex_t);
    glBufferData(GL_ARRAY_BUFFER, max_bytes, NULL_POINTER, GL_DYNAMIC_DRAW);

    // attribute layout: matches the text vertex shader's location=N
    // declarations and the _opengl3_renderer_internal__text_vertex_t
    // struct. one attribute per field group (position, color, uv).
    GLsizei stride = (GLsizei)sizeof(_opengl3_renderer_internal__text_vertex_t);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__text_vertex_t, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__text_vertex_t, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_opengl3_renderer_internal__text_vertex_t, u));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    _opengl3_renderer_internal__g.text_vert_cap =
        _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS* _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _opengl3_renderer_internal__g.text_cpu_verts =
        (_opengl3_renderer_internal__text_vertex_t*)GUI_MALLOC_T((size_t)_opengl3_renderer_internal__g.text_vert_cap * sizeof(_opengl3_renderer_internal__text_vertex_t), MM_TYPE__RENDERER);
    if (_opengl3_renderer_internal__g.text_cpu_verts == NULL_POINTER)
    {
        log_error("%s", "out of memory for cpu text vertex buffer");
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// INTERNAL: shader compile / link helpers
// ============================================================================

static GLuint _opengl3_renderer_internal__compile_shader(GLenum kind, char* src)
{
    GLuint sh = glCreateShader(kind);
    if (sh == 0)
    {
        log_error("%s", "glCreateShader returned 0");
        return 0;
    }

    const GLchar* srcs[1] = { (const GLchar*)src };
    glShaderSource(sh, 1, srcs, NULL_POINTER);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == FALSE)
    {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        if (len > 0)
        {
            char* log = (char*)GUI_MALLOC(len);
            if (log != NULL_POINTER)
            {
                glGetShaderInfoLog(sh, len, NULL_POINTER, log);
                log_error("%s %s", "shader compile error:\n", log);
                GUI_FREE(log);
            }
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint _opengl3_renderer_internal__link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    if (p == 0)
    {
        log_error("%s", "glCreateProgram returned 0");
        return 0;
    }
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok == FALSE)
    {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        if (len > 0)
        {
            char* log = (char*)GUI_MALLOC(len);
            if (log != NULL_POINTER)
            {
                glGetProgramInfoLog(p, len, NULL_POINTER, log);
                log_error("%s %s", "program link error:\n", log);
                GUI_FREE(log);
            }
        }
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ============================================================================
// PUBLIC: text pipeline
// ============================================================================
// the font module rasterizes glyphs into a 1024x1024 R8 bitmap and hands
// it to us here for uploading. we keep the GLuint as the opaque atlas
// handle; void* round-trips through font.c until it comes back in
// set_text_atlas/destroy_atlas.

/**
 * @brief upload an alpha-only bitmap to the GPU as a texture. Called by font.c when rasterizing a new (family, size) pair.
 *
 * @param const ubyte* pixels -> Row-major R8 bitmap (one byte per pixel, interpreted as alpha).
 * @param int width -> Bitmap width in pixels.
 * @param int height -> Bitmap height in pixels.
 *
 * @return void* -> Opaque backend-specific texture handle (GLuint / ID3D11ShaderResourceView* / IDirect3DTexture9*), or NULL on failure.
 */
void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    if (pixels == NULL_POINTER || width <= 0 || height <= 0)
    {
        return NULL_POINTER;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0)
    {
        return NULL_POINTER;
    }

    // row alignment defaults to 4 bytes. our R8 atlas rows are width*1
    // bytes; widths that aren't multiples of 4 would land on the wrong
    // byte offset per row without this. atlases are normally 1024-wide
    // so we're already aligned, but setting this makes arbitrary widths
    // safe -- zero cost at texture upload time.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glBindTexture(GL_TEXTURE_2D, tex);

    // GL_R8 internal format (single-channel 8-bit unsigned, reads as
    // 0..1 in the shader's .r channel). GL_RED is the format of the
    // data we're uploading. core in 3.0; widely supported.
    glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels );

    // LINEAR filtering so the 3x-oversampled glyphs sample smoothly
    // between texels. CLAMP_TO_EDGE so we never bleed neighbor glyphs
    // (stbtt_pack leaves 1px padding, but bilinear reaches ~0.5px
    // either way from the quad edge).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    // round-trip the GLuint through a void* (via uintptr_t to dodge
    // sign-extension warnings on 64-bit compilers).
    return (void*)(uintptr_t)tex;
}

/**
 * @brief destroy an atlas created by renderer__create_atlas_r8.
 *
 * @param void* atlas -> Handle returned by create_atlas_r8. NULL is a no-op.
 *
 * @return void
 */
void renderer__destroy_atlas(void* atlas)
{
    if (atlas == NULL_POINTER)
    {
        return;
    }
    GLuint tex = (GLuint)(uintptr_t)atlas;
    // if we're about to delete the currently-bound atlas, clear the
    // cached handle so end_frame won't try to sample a dangling name.
    if (_opengl3_renderer_internal__g.current_text_atlas == tex)
    {
        _opengl3_renderer_internal__g.current_text_atlas = 0;
    }
    glDeleteTextures(1, &tex);
}

/**
 * @brief bind the atlas to be sampled by subsequent renderer__submit_text_glyph calls (until either another atlas is bound or the frame ends). The text draw pass uses the last-bound atlas -- so all text in one frame that uses different fonts needs to be submitted in font-coherent runs.
 *
 * @param void* atlas -> Handle from create_atlas_r8.
 *
 * @return void
 */
void renderer__set_text_atlas(void* atlas)
{
    _opengl3_renderer_internal__g.current_text_atlas = (GLuint)(uintptr_t)atlas;
}

/**
 * @brief queue one textured quad into the frame's text batch. Drawn after all solid rects when end_frame fires.
 *
 * @param gui_rect_t rect -> Quad position + size in pixels (top-left origin).
 * @param gui_rect_t uv -> UV rect into the atlas (0..1 coordinates; uv.w/h are sizes, not endpoints).
 * @param gui_color_t color -> Text color; per-pixel alpha = color.a * atlas_sample.r.
 *
 * @return void
 */
void renderer__submit_text_glyph(gui_rect_t rect, gui_rect_t uv, gui_color_t color)
{
    GLuint atlas = _opengl3_renderer_internal__g.current_text_atlas;
    if (atlas == 0)
    {
        // no atlas bound -- font.c didn't call set_text_atlas before
        // submitting, or the current font's atlas was destroyed. drop.
        return;
    }
    if (_opengl3_renderer_internal__g.text_vert_cap < _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }
    boole new_run = (boole)(_opengl3_renderer_internal__g.text_run_count == 0 ||
        _opengl3_renderer_internal__g.text_runs[_opengl3_renderer_internal__g.text_run_count - 1].atlas != atlas);
    if (_opengl3_renderer_internal__g.text_vert_count + _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD > _opengl3_renderer_internal__g.text_vert_cap ||
        (new_run && _opengl3_renderer_internal__g.text_run_count >= _OPENGL3_RENDERER_INTERNAL__MAX_TEXT_RUNS))
    {
        // Keep the active scissor and draw order, then continue this string.
        // The flush resets the atlas, so restore it for subsequent glyphs.
        _opengl3_renderer_internal__flush_batches();
        _opengl3_renderer_internal__g.current_text_atlas = atlas;
    }

    // resolve the current run. if the last run targets this atlas, we
    // keep appending to it. otherwise we open a new run pointing at the
    // next vertex slot. this is the heart of multi-font support: the
    // frame's text VBO is laid out as [run_0_verts | run_1_verts | ...],
    // and end_frame issues one draw per run with that run's atlas bound.
    _opengl3_renderer_internal__text_run_t* run = NULL_POINTER;
    if (_opengl3_renderer_internal__g.text_run_count > 0)
    {
        run = &_opengl3_renderer_internal__g.text_runs[_opengl3_renderer_internal__g.text_run_count - 1];
        if (run->atlas != atlas)
        {
            run = NULL_POINTER; // different atlas -- need a fresh run.
        }
    }
    if (run == NULL_POINTER)
    {
        run = &_opengl3_renderer_internal__g.text_runs[_opengl3_renderer_internal__g.text_run_count++];
        run->atlas      = atlas;
        run->vert_start = _opengl3_renderer_internal__g.text_vert_count;
        run->vert_count = 0;
    }

    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    float u0 = uv.x;
    float v0 = uv.y;
    float u1 = uv.x + uv.w;
    float v1 = uv.y + uv.h;

    // same vertex ordering as solid rects (VISUAL CONTRACT):
    // tri 1: TL, TR, BL
    // tri 2: TR, BR, BL
    _opengl3_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _opengl3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _opengl3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _opengl3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _opengl3_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _opengl3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    run->vert_count += _OPENGL3_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

// ============================================================================
// INTERNAL: misc helpers
// ============================================================================

static void _opengl3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c)
{
    _opengl3_renderer_internal__vertex_t* v = &_opengl3_renderer_internal__g.cpu_verts[_opengl3_renderer_internal__g.vert_count++];
    v->x      = x;
    v->y      = y;
    v->r      = c.r;
    v->g      = c.g;
    v->b      = c.b;
    v->a      = c.a;
    v->lx     = lx;
    v->ly     = ly;
    v->rect_w = rw;
    v->rect_h = rh;
    v->radius = radius;
}

static void _opengl3_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color_t c)
{
    _opengl3_renderer_internal__text_vertex_t* tv = &_opengl3_renderer_internal__g.text_cpu_verts[_opengl3_renderer_internal__g.text_vert_count++];
    tv->x = x;
    tv->y = y;
    tv->r = c.r;
    tv->g = c.g;
    tv->b = c.b;
    tv->a = c.a;
    tv->u = u;
    tv->v = v;
}


#if defined(_WIN32)
static void _opengl3_renderer_internal__log_win32_error(char* where)
{
    DWORD err = GetLastError();
    log_error("%s %s %lu %s", where, "failed (error", (unsigned long)err, ")");
}
#endif


// ============================================================================
// IMAGE PIPELINE (RGBA textures, one-quad-per-submit, lazy-initialized)
// ============================================================================
// Kept self-contained down here so the rest of the file doesn't have to
// thread image-specific state through init. First call to submit_image
// lazily builds the program / VAO / VBO; shutdown above releases them.

#include "shaders/generated/image.vert.h"

#include "shaders/generated/image.frag.h"

static boole _opengl3_renderer_internal__ensure_image_pipeline(void)
{
    if (_opengl3_renderer_internal__g.image_program != 0)
    {
        return TRUE;
    }

    GLuint vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   (char*)_OPENGL3_RENDERER_INTERNAL__IMAGE_VS);
    GLuint fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, (char*)_OPENGL3_RENDERER_INTERNAL__IMAGE_FS);
    if (vs == 0 || fs == 0)
    {
        if (vs != 0)
        {
            glDeleteShader(vs);
        }
        if (fs != 0)
        {
            glDeleteShader(fs);
        }
        return FALSE;
    }
    GLuint prog = _opengl3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (prog == 0)
    {
        return FALSE;
    }
    _opengl3_renderer_internal__g.image_program         = prog;
    _opengl3_renderer_internal__g.image_u_viewport_loc  = glGetUniformLocation(prog, "u_viewport");
    _opengl3_renderer_internal__g.image_u_tex_loc       = glGetUniformLocation(prog, "u_tex");

    // one-quad scratch VBO. reused across every submit_image call;
    // glBufferData with NULL first (orphan) before each upload so the
    // driver doesn't stall the GPU waiting for the previous draw to
    // finish reading the buffer.
    glGenVertexArrays(1, &_opengl3_renderer_internal__g.image_vao);
    glGenBuffers(1, &_opengl3_renderer_internal__g.image_vbo);
    glBindVertexArray(_opengl3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.image_vbo);

    // Same layout as text_vertex: x,y, r,g,b,a, u,v. Reusing the struct
    // would be viable but keeping it inline here (as a 32-byte stride)
    // avoids a forward dependency on that struct's memory layout.
    GLsizei stride = (GLsizei)(8 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return TRUE;
}

/**
 * @brief upload an RGBA8 image to the GPU. Bytes are consumed top-to-bottom, left-to-right, 4 bytes per pixel, no row padding (tightly packed). Caller retains ownership of the `rgba` buffer; the renderer copies the data into a texture and no longer references the source pointer after returning.
 *
 * @param const ubyte* rgba -> Tightly-packed RGBA8 pixel buffer, length = width*height*4.
 * @param int width -> Image width in pixels.
 * @param int height -> Image height in pixels.
 *
 * @return void* -> Opaque texture handle, or NULL if upload failed or the backend doesn't yet implement this pipeline.
 */
static void* _opengl3_renderer_internal__create_texture_rgba(const ubyte* rgba, int width, int height, boole generate_mips)
{
    if (rgba == NULL_POINTER || width <= 0 || height <= 0)
    {
        return NULL_POINTER;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0)
    {
        return NULL_POINTER;
    }

    // the mip chain is built on the CPU by the shared helper so every
    // backend samples the same texels (see _mip_cpu.h); the driver's
    // glGenerateMipmap filtered differently from the other APIs. the
    // chain matters: photo-sized sources drawn as 48 px tiles sample one
    // 2x2 neighborhood of a far bigger source per output pixel without
    // it, and twinkle while they move.
    mip_cpu__chain_t chain;
    ubyte*           storage       = NULL_POINTER;
    size_t           storage_bytes = generate_mips == TRUE ? mip_cpu__storage_bytes(width, height) : 0;
    int              level         = 0;

    if (storage_bytes > 0)
    {
        storage = (ubyte*)GUI_MALLOC_T(storage_bytes, MM_TYPE__RENDERER);
    }

    mip_cpu__build(&chain, rgba, width, height, storage);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, tex);

    for (level = 0; level < chain.count; level++)
    {
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, chain.levels[level].w, chain.levels[level].h, 0, GL_RGBA, GL_UNSIGNED_BYTE, chain.levels[level].pixels);
    }

    if (storage != NULL_POINTER)
    {
        GUI_FREE(storage);
    }

    // trilinear over the complete chain; bilinear on the single level
    // used for frame surfaces or when chain storage could not be allocated
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, chain.count > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return (void*)(uintptr_t)tex;
}

// Public image uploads retain mipmaps for images that can be drawn smaller.
void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _opengl3_renderer_internal__create_texture_rgba(rgba, width, height, TRUE);
}

// The compositor's final surface is displayed at one texel per viewport pixel.
// Its lower mip levels would be rebuilt and uploaded every frame but never used.
GUI_API void* renderer_backend__create_frame_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _opengl3_renderer_internal__create_texture_rgba(rgba, width, height, FALSE);
}

/**
 * @brief release a texture returned by renderer__create_texture_rgba. NULL is a no-op so widgets that never successfully uploaded can still call this from their destroy hook without a guard.
 *
 * @param void* tex -> Handle from create_texture_rgba.
 *
 * @return void
 */
void renderer__destroy_texture(void* tex)
{
    if (tex == NULL_POINTER)
    {
        return;
    }
    GLuint name = (GLuint)(uintptr_t)tex;
    glDeleteTextures(1, &name);
}

/**
 * @brief draw one image quad immediately. Flushes any pending solid-rect or text batch first so previously-submitted content stays underneath and subsequent submissions stay on top. Full-texture draw (UV 0..1 on both axes); no subregion support in this pass (crop via node bounds + scissor instead).
 *
 * @param gui_rect_t rect -> Destination rect in top-left-origin pixel coords.
 * @param void* tex -> Texture handle from create_texture_rgba. NULL = skip draw.
 * @param gui_color_t tint -> Multiplied against the sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 * @param float u0 -> left texture coordinate of the sub-rect to sample
 * @param float v0 -> top texture coordinate of the sub-rect to sample
 * @param float u1 -> right texture coordinate of the sub-rect to sample
 * @param float v1 -> bottom texture coordinate of the sub-rect to sample
 *
 * @return void
 */
void renderer__submit_image_uv(gui_rect_t rect, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    if (tex == NULL_POINTER)
    {
        return;
    }
    if (_opengl3_renderer_internal__ensure_image_pipeline() == FALSE)
    {
        return;
    }

    // flush any pending rects + text so their draws land UNDERNEATH the
    // image (correct order relative to submit_rect / submit_text_glyph
    // calls made before this one).
    _opengl3_renderer_internal__flush_batches();

    float x0 = (float)rect.x;
    float y0 = (float)rect.y;
    float x1 = x0 + (float)rect.w;
    float y1 = y0 + (float)rect.h;

    float verts[6 * 8] = {
        // x,  y,    r,g,b,a,                   u, v
        x0, y0,  tint.r, tint.g, tint.b, tint.a,  u0, v0,
        x1, y0,  tint.r, tint.g, tint.b, tint.a,  u1, v0,
        x0, y1,  tint.r, tint.g, tint.b, tint.a,  u0, v1,
        x1, y0,  tint.r, tint.g, tint.b, tint.a,  u1, v0,
        x1, y1,  tint.r, tint.g, tint.b, tint.a,  u1, v1,
        x0, y1,  tint.r, tint.g, tint.b, tint.a,  u0, v1,
    };

    glUseProgram(_opengl3_renderer_internal__g.image_program);
    if (_opengl3_renderer_internal__g.image_u_viewport_loc >= 0)
    {
        glUniform2f(_opengl3_renderer_internal__g.image_u_viewport_loc, (float)_opengl3_renderer_internal__g.viewport_w, (float)_opengl3_renderer_internal__g.viewport_h);
    }
    if (_opengl3_renderer_internal__g.image_u_tex_loc >= 0)
    {
        glUniform1i(_opengl3_renderer_internal__g.image_u_tex_loc, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)tex);

    glBindVertexArray(_opengl3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.image_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), NULL_POINTER,  GL_DYNAMIC_DRAW);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

/**
 * @brief queue a textured quad covering rect with the whole texture; convenience wrapper over renderer__submit_image_uv with the full [0,1] range
 *
 * @param gui_rect_t rect -> destination rectangle in pixels
 * @param void* tex -> texture handle from renderer__create_texture_rgba
 * @param gui_color_t tint -> multiplied color
 *
 * @return void
 */
void renderer__submit_image(gui_rect_t rect, void* tex, gui_color_t tint)
{
    renderer__submit_image_uv(rect, tex, tint, 0.0f, 0.0f, 1.0f, 1.0f);
}


/**
 * @brief size of the buffer renderer__request_capture needs: the back buffer's pixel size
 *
 * @param int64* out_w -> receives the width in pixels
 * @param int64* out_h -> receives the height in pixels
 *
 * @return boole -> FALSE before the renderer has a viewport
 */
boole renderer__capture_size(int64* out_w, int64* out_h)
{
    int64 w = (int64)_opengl3_renderer_internal__g.viewport_w;
    int64 h = (int64)_opengl3_renderer_internal__g.viewport_h;

    if (out_w != NULL_POINTER)
    {
        *out_w = w;
    }

    if (out_h != NULL_POINTER)
    {
        *out_h = h;
    }

    return (boole)(w > 0 && h > 0);
}

/**
 * @brief arms a copy of the next finished frame into out_rgba (top-down RGBA8, width * height * 4 bytes), taken at the next renderer__end_frame before the frame is presented
 *
 * @param ubyte* out_rgba -> destination, owned by the caller
 * @param int64 cap -> destination capacity in bytes
 *
 * @return boole -> FALSE when the buffer is too small or there is no viewport
 */
boole renderer__request_capture(ubyte* out_rgba, int64 cap)
{
    int64 w = 0;
    int64 h = 0;

    if (out_rgba == NULL_POINTER || renderer__capture_size(&w, &h) == FALSE || cap < w * h * 4)
    {
        return FALSE;
    }

    _opengl3_renderer_internal__capture_out  = out_rgba;
    _opengl3_renderer_internal__capture_cap  = cap;
    _opengl3_renderer_internal__capture_done = FALSE;

    return TRUE;
}

/**
 * @brief tells whether the armed capture has been copied out (it is, once renderer__end_frame ran after the request)
 *
 * @return boole -> TRUE after the copy
 */
boole renderer__capture_done(void)
{
    return _opengl3_renderer_internal__capture_done;
}

// Ordinary standalone scenes can use this renderer directly.
GUI_API boole renderer_backend__needs_compositor(void) { return FALSE; }

// ============================================================================
// LAYER MODE (renderer_backend.h, src/compositor.c, session/LAYERS-DESIGN.md)
// ============================================================================

#include "shaders/generated/layer.vert.h"
#include "shaders/generated/layer.frag.h"

/**
 * @brief build the layer pipeline on first use
 * @return boole -> TRUE when the pipeline is ready
 */
static boole _opengl3_renderer_internal__ensure_layer_pipeline(void)
{
    GLuint vs = 0;
    GLuint fs = 0;
    GLuint prog = 0;
    GLsizei stride = (GLsizei)(5 * sizeof(float));

    if (_opengl3_renderer_internal__g.layer_program != 0)
    {
        return TRUE;
    }
    vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER, (char*)_OPENGL3_RENDERER_INTERNAL__LAYER_VS);
    fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, (char*)_OPENGL3_RENDERER_INTERNAL__LAYER_FS);
    if (vs == 0 || fs == 0)
    {
        if (vs != 0)
        {
            glDeleteShader(vs);
        }
        if (fs != 0)
        {
            glDeleteShader(fs);
        }
        return FALSE;
    }
    prog = _opengl3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (prog == 0)
    {
        return FALSE;
    }
    _opengl3_renderer_internal__g.layer_program        = prog;
    _opengl3_renderer_internal__g.layer_u_viewport_loc = glGetUniformLocation(prog, "u_viewport");
    _opengl3_renderer_internal__g.layer_u_tex_loc      = glGetUniformLocation(prog, "u_tex");
    _opengl3_renderer_internal__g.layer_u_opacity_loc  = glGetUniformLocation(prog, "u_opacity");
    glGenVertexArrays(1, &_opengl3_renderer_internal__g.layer_vao);
    glGenBuffers(1, &_opengl3_renderer_internal__g.layer_vbo);
    glBindVertexArray(_opengl3_renderer_internal__g.layer_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.layer_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return TRUE;
}

GUI_API boole renderer_backend__supports_layers(void)
{
    return TRUE;
}

/**
 * @brief a single-level premultiplied RGBA8 texture, linear, clamped
 * @param const ubyte* premultiplied_rgba -> tightly packed pixels, or NULL
 * @param int width -> width
 * @param int height -> height
 * @return void* -> texture name, released with renderer__destroy_texture
 */
GUI_API void* renderer_backend__create_layer_texture(const ubyte* premultiplied_rgba, int width, int height)
{
    GLuint tex = 0;

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
    {
        return NULL_POINTER;
    }
    glGenTextures(1, &tex);
    if (tex == 0)
    {
        return NULL_POINTER;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, premultiplied_rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return (void*)(uintptr_t)tex;
}

/**
 * @brief refresh a layer texture of the same size in place
 * @param void* texture -> from renderer_backend__create_layer_texture
 * @param const ubyte* premultiplied_rgba -> tightly packed pixels
 * @param int width -> must equal the texture width
 * @param int height -> must equal the texture height
 * @return boole -> FALSE when the size differs
 */
GUI_API boole renderer_backend__update_layer_texture(void* texture, const ubyte* premultiplied_rgba, int width, int height)
{
    GLuint tex = (GLuint)(uintptr_t)texture;
    GLint  tw = 0;
    GLint  th = 0;

    if (tex == 0 || premultiplied_rgba == NULL_POINTER || width <= 0 || height <= 0)
    {
        return FALSE;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw != width || th != height)
    {
        glBindTexture(GL_TEXTURE_2D, 0);
        return FALSE;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, premultiplied_rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return TRUE;
}

/**
 * @brief draw a premultiplied layer texture over a homogeneous quad (renderer_backend.h)
 * @param void* texture -> layer texture
 * @param const float* corners -> 4 x (x * w, y * w, w): top-left, top-right, bottom-right, bottom-left
 * @param float opacity -> multiplies all four channels
 * @param int blend_mode -> 0 normal, 1 screen, 2 replace
 * @return void
 */
GUI_API void renderer_backend__submit_layer(void* texture, const float* corners, float opacity, int blend_mode)
{
    static const int order[6] = {0, 1, 3, 1, 2, 3};
    static const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    float verts[6 * 5];
    GLuint tex = (GLuint)(uintptr_t)texture;
    GLint tw = 0;
    GLint th = 0;
    GLint filter = GL_LINEAR;
    boole aligned = FALSE;
    boole rendered = FALSE;
    int i = 0;

    stdlib__memset(verts, 0, sizeof(verts));
    if (tex == 0 || corners == NULL_POINTER || _opengl3_renderer_internal__ensure_layer_pipeline() == FALSE)
    {
        return;
    }
    // a pool render target was drawn with the frame's top-down mapping, so
    // its rows hold the layer bottom-up: sample it with v flipped
    for (i = 0; i < 4; i++)
    {
        if (_opengl3_renderer_internal__g.layer_targets[i].texture == tex)
        {
            rendered = TRUE;
        }
    }
    // draws submitted before this layer land underneath it
    _opengl3_renderer_internal__flush_batches();
    for (i = 0; i < 6; i++)
    {
        verts[i * 5 + 0] = corners[order[i] * 3 + 0];
        verts[i * 5 + 1] = corners[order[i] * 3 + 1];
        verts[i * 5 + 2] = corners[order[i] * 3 + 2];
        verts[i * 5 + 3] = uvs[order[i]][0];
        verts[i * 5 + 4] = (rendered == TRUE) ? 1.0f - uvs[order[i]][1] : uvs[order[i]][1];
    }
    glUseProgram(_opengl3_renderer_internal__g.layer_program);
    if (_opengl3_renderer_internal__g.layer_u_viewport_loc >= 0)
    {
        glUniform2f(_opengl3_renderer_internal__g.layer_u_viewport_loc, (float)_opengl3_renderer_internal__g.viewport_w, (float)_opengl3_renderer_internal__g.viewport_h);
    }
    if (_opengl3_renderer_internal__g.layer_u_tex_loc >= 0)
    {
        glUniform1i(_opengl3_renderer_internal__g.layer_u_tex_loc, 0);
    }
    if (_opengl3_renderer_internal__g.layer_u_opacity_loc >= 0)
    {
        glUniform1f(_opengl3_renderer_internal__g.layer_u_opacity_loc, opacity);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    // an integer-aligned identity quad of the texture's size samples texel
    // centers exactly, so nearest filtering reproduces the CPU's aligned fetch
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    aligned = (boole)(corners[2] == 1.0f && corners[5] == 1.0f && corners[8] == 1.0f && corners[11] == 1.0f && corners[0] == (float)(int)corners[0] && corners[1] == (float)(int)corners[1] && corners[3] - corners[0] == (float)tw && corners[7] - corners[1] == (float)th && corners[4] == corners[1] && corners[6] == corners[3] && corners[9] == corners[0] && corners[10] == corners[7]);
    filter = (aligned == TRUE) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    // premultiplied source over: d = s + d * (1 - a); screen: d = s + d * (1 - s); replace: d = s
    if (blend_mode == 1)
    {
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    else if (blend_mode == 2)
    {
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    }
    else if (blend_mode == 3)
    {
        // multiply over an opaque backdrop: d = s * d + d * (1 - a)
        glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    else
    {
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    // the layer bytes were premultiplied in sRGB space; store and blend them as bytes
    if (_opengl3_renderer_internal__g.srgb_fb)
    {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }
    glBindVertexArray(_opengl3_renderer_internal__g.layer_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _opengl3_renderer_internal__g.layer_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), NULL_POINTER, GL_DYNAMIC_DRAW);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (_opengl3_renderer_internal__g.srgb_fb)
    {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    // the rect and text batches rely on the blend function set at init
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

/**
 * @brief draw a layer's content into a pooled framebuffer (renderer_backend.h)
 * @param int width -> layer width in device pixels
 * @param int height -> layer height in device pixels
 * @return boole -> FALSE when no target is available (the caller rasterizes on the CPU)
 */
GUI_API boole renderer_backend__begin_layer(int width, int height)
{
    int slot = -1;
    int i = 0;

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || _opengl3_renderer_internal__g.layer_active != 0 || p_glGenFramebuffers == NULL_POINTER)
    {
        return FALSE;
    }
    for (i = 0; i < 4; i++)
    {
        if (_opengl3_renderer_internal__g.layer_targets[i].fbo != 0 && _opengl3_renderer_internal__g.layer_targets[i].w == width && _opengl3_renderer_internal__g.layer_targets[i].h == height)
        {
            slot = i;
            break;
        }
        if (slot < 0 || _opengl3_renderer_internal__g.layer_targets[i].used < _opengl3_renderer_internal__g.layer_targets[slot].used)
        {
            slot = i;
        }
    }
    // draws submitted before the layer belong to the frame
    _opengl3_renderer_internal__flush_batches();
    // the frame's framebuffer is whatever is bound now: the window's, or an
    // embedded host's own
    _opengl3_renderer_internal__g.layer_saved_framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &_opengl3_renderer_internal__g.layer_saved_framebuffer);
    if (_opengl3_renderer_internal__g.layer_targets[slot].fbo == 0 || _opengl3_renderer_internal__g.layer_targets[slot].w != width || _opengl3_renderer_internal__g.layer_targets[slot].h != height)
    {
        if (_opengl3_renderer_internal__g.layer_targets[slot].fbo == 0)
        {
            glGenFramebuffers(1, &_opengl3_renderer_internal__g.layer_targets[slot].fbo);
        }
        if (_opengl3_renderer_internal__g.layer_targets[slot].texture == 0)
        {
            glGenTextures(1, &_opengl3_renderer_internal__g.layer_targets[slot].texture);
        }
        glBindTexture(GL_TEXTURE_2D, _opengl3_renderer_internal__g.layer_targets[slot].texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL_POINTER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, _opengl3_renderer_internal__g.layer_targets[slot].fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _opengl3_renderer_internal__g.layer_targets[slot].texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)_opengl3_renderer_internal__g.layer_saved_framebuffer);
            glDeleteFramebuffers(1, &_opengl3_renderer_internal__g.layer_targets[slot].fbo);
            glDeleteTextures(1, &_opengl3_renderer_internal__g.layer_targets[slot].texture);
            stdlib__null_memory(&_opengl3_renderer_internal__g.layer_targets[slot], sizeof(_opengl3_renderer_internal__g.layer_targets[slot]));
            log_error("%s", "layer framebuffer incomplete");
            return FALSE;
        }
        _opengl3_renderer_internal__g.layer_targets[slot].w = width;
        _opengl3_renderer_internal__g.layer_targets[slot].h = height;
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, _opengl3_renderer_internal__g.layer_targets[slot].fbo);
    }
    _opengl3_renderer_internal__g.layer_targets[slot].used = ++_opengl3_renderer_internal__g.layer_counter;
    _opengl3_renderer_internal__g.layer_active = slot + 1;
    _opengl3_renderer_internal__g.layer_saved_viewport_w = _opengl3_renderer_internal__g.viewport_w;
    _opengl3_renderer_internal__g.layer_saved_viewport_h = _opengl3_renderer_internal__g.viewport_h;
    _opengl3_renderer_internal__g.layer_saved_scissor_depth = _opengl3_renderer_internal__g.scissor_depth;
    stdlib__memcpy(_opengl3_renderer_internal__g.layer_saved_scissors, _opengl3_renderer_internal__g.scissor_stack, sizeof(_opengl3_renderer_internal__g.scissor_stack));
    _opengl3_renderer_internal__g.layer_saved_srgb = _opengl3_renderer_internal__g.srgb_fb;
    // the shaders map pixels through u_viewport: inside the layer that is
    // the layer's size; the target is plain RGBA8, so the plain shaders
    // draw it (the layer's bytes are written raw at present time)
    _opengl3_renderer_internal__g.viewport_w = width;
    _opengl3_renderer_internal__g.viewport_h = height;
    _opengl3_renderer_internal__g.scissor_depth = 0;
    _opengl3_renderer_internal__g.srgb_fb = FALSE;
    if (_opengl3_renderer_internal__g.layer_saved_srgb)
    {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    _opengl3_renderer_internal__apply_scissor_top();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return TRUE;
}

/**
 * @brief finish the layer: back to the frame's framebuffer, viewport and scissor
 * @return void* -> the layer's texture name (owned by the pool)
 */
GUI_API void* renderer_backend__end_layer(void)
{
    int slot = _opengl3_renderer_internal__g.layer_active - 1;

    if (slot < 0)
    {
        return NULL_POINTER;
    }
    _opengl3_renderer_internal__flush_batches();
    _opengl3_renderer_internal__g.layer_active = 0;
    _opengl3_renderer_internal__g.viewport_w = _opengl3_renderer_internal__g.layer_saved_viewport_w;
    _opengl3_renderer_internal__g.viewport_h = _opengl3_renderer_internal__g.layer_saved_viewport_h;
    _opengl3_renderer_internal__g.scissor_depth = _opengl3_renderer_internal__g.layer_saved_scissor_depth;
    stdlib__memcpy(_opengl3_renderer_internal__g.scissor_stack, _opengl3_renderer_internal__g.layer_saved_scissors, sizeof(_opengl3_renderer_internal__g.scissor_stack));
    _opengl3_renderer_internal__g.srgb_fb = _opengl3_renderer_internal__g.layer_saved_srgb;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)_opengl3_renderer_internal__g.layer_saved_framebuffer);
    if (_opengl3_renderer_internal__g.srgb_fb)
    {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    glViewport(0, 0, (GLsizei)_opengl3_renderer_internal__g.viewport_w, (GLsizei)_opengl3_renderer_internal__g.viewport_h);
    _opengl3_renderer_internal__apply_scissor_top();
    return (void*)(uintptr_t)_opengl3_renderer_internal__g.layer_targets[slot].texture;
}

/**
 * @brief read a back-buffer region (premultiplied by construction of the blend function), top-left origin
 * @param int x -> left, device pixels
 * @param int y -> top
 * @param int w -> width
 * @param int h -> height
 * @param ubyte* out_rgba -> w * h * 4 bytes
 * @return boole -> FALSE when the region leaves the viewport
 */
GUI_API boole renderer_backend__read_region(int x, int y, int w, int h, ubyte* out_rgba)
{
    ubyte* tmp = NULL_POINTER;
    int vh = (int)_opengl3_renderer_internal__g.viewport_h;
    int vw = (int)_opengl3_renderer_internal__g.viewport_w;
    int row = 0;

    if (out_rgba == NULL_POINTER || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > vw || y + h > vh)
    {
        return FALSE;
    }
    tmp = (ubyte*)GUI_MALLOC_T((size_t)w * (size_t)h * 4, MM_TYPE__RENDERER);
    if (tmp == NULL_POINTER)
    {
        return FALSE;
    }
    _opengl3_renderer_internal__flush_batches();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // GL rows come bottom-up; the caller wants the top row first
    glReadPixels(x, vh - (y + h), w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (row = 0; row < h; row++)
    {
        stdlib__memcpy(out_rgba + (size_t)row * (size_t)w * 4, tmp + (size_t)(h - 1 - row) * (size_t)w * 4, (size_t)w * 4);
    }
    GUI_FREE(tmp);
    return TRUE;
}

#include "opengl3_blur.h"
