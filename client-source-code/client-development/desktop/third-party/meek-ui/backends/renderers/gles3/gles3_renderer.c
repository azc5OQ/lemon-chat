// ============================================================================
// gles3_renderer.c - OpenGL ES 3.0 backend.
// ============================================================================
// WHAT THIS FILE DOES (the elevator pitch):
// scene.c walks the widget tree and tells us "draw this colored rectangle
// here" by calling renderer__submit_rect(). this file packages those calls
// into work the GPU understands and ships it across using OpenGL ES 3.0 --
// the embedded variant of OpenGL that ships on essentially every Android
// device (and iOS, and most embedded Linux SoCs).
// this backend is interchangeable with opengl3_renderer.c (desktop OpenGL),
// d3d11_renderer.c, and d3d9_renderer.c -- exactly one is compiled into
// the gui library at a time. on Android the build script picks gles3.
// PLATFORM / RENDERER SPLIT (different from the Windows backends):
// UNLIKE opengl3_renderer.c, this file does NOT create the GL context.
// EGL setup lives in platform_android.c (EGLDisplay + EGLConfig +
// EGLSurface + EGLContext tied to the ANativeWindow from
// android_native_app_glue). the renderer expects the platform to have
// made an EGL context current BEFORE renderer__init runs and to keep
// it current during every renderer__* call.
// this is a cleaner split than the Windows backends, which each own
// their context creation. eventually platform_win32 should pull the
// wgl* dance out of opengl3_renderer.c and own the WGL context the
// same way platform_android owns the EGL context.
// THE PIPELINE:
// 1. INPUT-ASSEMBLY: glBindVertexArray + glBindBuffer set the source
// of vertex data. the VAO recipe was built once at init.
// 2. VERTEX SHADER: written in GLSL ES 3.00. converts pixel coords
// to NDC (-1..1, y-up) so we can work in pixel coords on the CPU
// side. passes color and SDF varyings through to the PS.
// 3. RASTERIZER: hardware fixed function. ScissorEnable lives here
// (glEnable(GL_SCISSOR_TEST) + glScissor(rect) -- BL-origin Y
// coords).
// 4. PIXEL SHADER: GLSL ES 3.00. computes SDF for a rounded box,
// antialiases the edge with a 1-pixel smoothstep, discards
// pixels outside the rounded shape so corners actually round.
// 5. OUTPUT-MERGER: alpha blending (glBlendFuncSeparate so the
// framebuffer's alpha stays at 1.0 across overlapping draws --
// see renderer.h VISUAL CONTRACT).
// 6. PRESENT: NOT done here. platform_android calls eglSwapBuffers
// on its EGLSurface. our end_frame just flushes the batches.
// SCOPE FOR THE FIRST PASS:
// SOLID RECT PASS: full SDF rounded-box renderer, identical math to
// opengl3_renderer.c. this gives us window + widgets + buttons.
// SCISSOR: implemented (mirrors opengl3). scrollable divs and Window
// get proper clipping on Android.
// TEXT PASS: stub entry points (create_atlas_r8 returns NULL, submit
// is a no-op). font__at will fail to create atlases on Android for
// now; widgets gracefully skip text rendering. adding the textured
// glyph pipeline is a small follow-up once the rect path is verified
// on-device.
// VISUAL CONTRACT: identical SDF formula, smoothstep range, scissor
// semantics, separate-alpha blend mode, and vertex ordering as the
// opengl3 / d3d11 / d3d9 backends.

// GL header: Android + Linux + DRM use <GLES3/gl3.h>. macOS has no
// GLES headers and instead ships desktop GL 3.3+ core under
// <OpenGL/gl3.h>. The token set the backend uses is the intersection
// of GLES 3.0 and desktop GL 3.3 core, so picking either header at
// compile time yields the same callable surface.
#if defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION 1
  #include <OpenGL/gl3.h>
#else
  #include <GLES3/gl3.h>
#endif

#include <stdint.h> // uintptr_t for GLuint <-> void* round-trip through font.c's opaque atlas_tex handle.
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#define MEEK_RENDERER_BACKEND
#include "renderer.h"
#include "renderer_backend.h"
#include "gradient.h"
#include "atlas.h"
#include "_visual_contract.h"
#include "_blur_cpu.h"
#include "_mip_cpu.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// Shader version preamble. GLES 3.0 uses "#version 300 es" +
// requires precision qualifiers. Desktop GL on macOS (we ask
// NSOpenGLPixelFormat for a 4.1 Core Profile context, which gives
// GLSL 410) uses "#version 410"; precision qualifiers are allowed
// but ignored. Everything else in the shader bodies (layout(location
// =N), in/out, texture(), discard) works identically in both
// versions, so the only per-platform difference is this preamble
// string. Kept as a macro rather than a const char* so string
// concatenation at the call sites stays compile-time.
#if defined(__APPLE__)
  #define _GLES3_RENDERER_INTERNAL__GLSL_VERSION "#version 410\n"
#else
  #define _GLES3_RENDERER_INTERNAL__GLSL_VERSION "#version 300 es\n"
#endif

// ============================================================================
// VERTEX FORMAT -- identical to opengl3_renderer.c
// ============================================================================

typedef struct _gles3_renderer_internal__vertex_t
{
    float x;        // 0x0
    float y;        // 0x4
    float r;        // 0x8
    float g;        // 0xC
    float b;        // 0x10
    float a;        // 0x14
    float lx;       // 0x18
    float ly;       // 0x1C
    float rect_w;   // 0x20
    float rect_h;   // 0x24
    float radius;   // 0x28
} _gles3_renderer_internal__vertex_t;

#define _GLES3_RENDERER_INTERNAL__MAX_QUADS       16384
#define _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD  6

// TEXT VERTEX: simpler than the solid-rect vertex -- no SDF bookkeeping,
// just pixel position + per-vertex color + atlas UV. six per glyph quad.
// mirrors opengl3_renderer.c's layout exactly so the font.c path works
// unchanged.
typedef struct _gles3_renderer_internal__text_vertex_t
{
    float x;   // 0x0
    float y;   // 0x4
    float r;   // 0x8
    float g;   // 0xC
    float b;   // 0x10
    float a;   // 0x14
    float u;   // 0x18
    float v;   // 0x1C
} _gles3_renderer_internal__text_vertex_t;

#define _GLES3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096
#define _GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS    64

// per-atlas "run" record. a frame may emit glyphs from multiple
// (family, size) pairs -- each combination lives in its own atlas
// texture, so we partition the frame's text VBO into runs and issue
// one glDrawArrays per run with the matching texture bound.
typedef struct _gles3_renderer_internal__text_run_t
{
    GLuint atlas;        // 0x0
    int64  vert_start;   // 0x8
    int64  vert_count;   // 0x10
} _gles3_renderer_internal__text_run_t;

// ============================================================================
// SHADER SOURCES (GLSL ES 3.00)
// ============================================================================
// differences from the desktop GL 3.30 shaders:
// - "#version 300 es" instead of "#version 330 core"
// - explicit "precision" qualifier required for floats in the fragment
// shader (ES has no implicit default for floats). we use highp for
// consistent subpixel precision with the desktop backend; mediump
// would be fine for most phones but drops ~2-3 bits on low-end.
// - vertex shader's precision defaults to highp, fragment shader's
// defaults to undefined-in-GLSL-ES-3.0 -> must be specified.

#include "shaders/generated/rect.vert.h"

#include "shaders/generated/rect.frag.h"

// TEXT VS/FS -- GLES 3.0 port of opengl3's text shaders. same math as
// the desktop legacy (non-sRGB) variant: sample the atlas (R8 alpha
// coverage), apply the pow(a, 1/2.2) gamma approximation, multiply by
// vertex alpha. matches the Windows backends' text look.
// precision qualifier is required in the FS on GLES (there's no
// implicit default for floats); we use highp to stay consistent with
// the rect pipeline so subpixel glyph positions don't snap to the
// mediump grid on low-end phones.
#include "shaders/generated/text.vert.h"

#include "shaders/generated/text.frag.h"

// ============================================================================
// STATE
// ============================================================================

typedef struct _gles3_renderer_internal__state_t
{
    GLuint program;                                                                           // 0x0
    GLint  u_viewport_loc;                                                                    // 0x4
    GLuint vao;                                                                               // 0x8
    GLuint vbo;                                                                               // 0xC

    _gles3_renderer_internal__vertex_t* cpu_verts;                                              // 0x10
    int64                             vert_count;                                             // 0x18
    int64                             vert_cap;                                               // 0x20

    int64 viewport_w;                                                                         // 0x28
    int64 viewport_h;                                                                         // 0x30

    // text pipeline (separate shader + VAO + VBO + cpu staging).
    // drawn after the solid-rect pass in end_frame so glyphs land
    // on top of button background rects. see the opengl3 backend
    // for the full commentary on per-atlas run batching.
    GLuint text_program;                                                                      // 0x38
    GLint  text_u_viewport_loc;                                                               // 0x3C
    GLint  text_u_atlas_loc;                                                                  // 0x40
    GLuint text_vao;                                                                          // 0x44
    GLuint text_vbo;                                                                          // 0x48

    _gles3_renderer_internal__text_vertex_t* text_cpu_verts;                                    // 0x50
    int64                                  text_vert_count;                                   // 0x58
    int64                                  text_vert_cap;                                     // 0x60

    GLuint current_text_atlas;                                                                // 0x68

    _gles3_renderer_internal__text_run_t text_runs[_GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS];    // 0x70
    int64                              text_run_count;                                        // 0x670

    // Image pipeline (RGBA textures, one-quad-per-submit). Lazy-built
    // on first submit_image call. Matches opengl3_renderer.c.
    GLuint image_program;                                                                     // 0x678
    GLint  image_u_viewport_loc;                                                              // 0x67C
    GLint  image_u_tex_loc;                                                                   // 0x680
    GLuint image_vao;                                                                         // 0x684
    GLuint image_vbo;                                                                         // 0x688

    // Image-batch state. Mirrors the text-batch shape: one CPU
    // vertex buffer + a run list keyed by texture, drained in
    // flush_batches between the rect and text passes. Consecutive
    // submit_image calls targeting the same texture accumulate
    // into the SAME run -- so 50 icons sharing one atlas collapse
    // to a single glDrawArrays. Replaces the previous "one draw
    // call per image, with full GL state churn" path that capped
    // the swipe at ~15 Hz with 150 icons on screen.
    // (x,y,r,g,b,a,u,v) per vertex
    float*                            image_cpu_verts;                                        // 0x690
    int64                             image_vert_count;                                       // 0x698
    int64                             image_vert_cap;                                         // 0x6A0
    _gles3_renderer_internal__text_run_t image_runs[_GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS];   // 0x6A8
    int64                             image_run_count;                                        // 0xCA8
    // last appended run's tex
    GLuint                            image_current_tex;                                      // 0xCB0
    // Legacy image-pipeline state cache (kept for compatibility
    // with the previous one-draw-per-submit path; now mostly
    // redundant since image batching renders all images in one
    // pass during flush_batches). Survives across consecutive
    // image submits; invalidated when rect/text pipelines bind
    // their own program / VAO / VBO.
    boole  image_state_bound;                                                                 // 0xCB4
    GLuint image_last_tex;                                                                    // 0xCB8

    // GPU backdrop blur. blur_region runs the source rect through a
    // ping-pong: glBlitFramebuffer downsamples backbuffer -> blur_tex[0],
    // shader applies horizontal separable Gaussian into blur_tex[1],
    // shader applies vertical pass back into blur_tex[0], then the
    // image pipeline draws blur_tex[0] over the original rect with
    // bilinear upsampling. Replaces the previous CPU path that did
    // glReadPixels + CPU Gaussian + upload (three GPU/CPU round-trips
    // that murdered phone perf at sigma=18 over a 1080x2246 panel).
    GLuint blur_program;                                                                      // 0xCBC
    GLint  blur_u_src_loc;                                                                    // 0xCC0
    GLint  blur_u_step_loc;                                                                   // 0xCC4
    GLint  blur_u_sigma_loc;                                                                  // 0xCC8
    GLuint blur_vao;                                                                          // 0xCCC
    GLuint blur_vbo;                                                                          // 0xCD0
    GLuint blur_fbo[2];                                                                       // 0xCD4
    GLuint blur_tex[2];                                                                       // 0xCDC
    int    blur_tex_w;                                                                        // 0xCE4
    int    blur_tex_h;                                                                        // 0xCE8
    boole  blur_built;                                                                        // 0xCEC

    // Scissor stack. Same shape as opengl3_renderer.c: push records the
    // new clip rect, the GL scissor is set to the intersection of all
    // stacked rects, and pop drops the top of the stack.
    gui_rect_t scissor_stack[32];                                                               // 0xCF0
    int64    scissor_depth;                                                                   // 0xEF0
    // Last effective scissor rect we applied to the GPU. push/pop
    // computes the new effective rect and skips both the batch
    // flush and the glScissor call when it matches -- nested
    // containers that share the same intersected clip (e.g. a
    // scrollable div whose child <text> nodes each push the same
    // padded inner rect) thus avoid one draw-call per nested push.
    gui_rect_t scissor_last_applied;                                                            // 0xEF8
    boole    scissor_last_valid;                                                              // 0xF08
} _gles3_renderer_internal__state_t;

static _gles3_renderer_internal__state_t _gles3_renderer_internal__g;

// ============================================================================
// FORWARD DECLS
// ============================================================================

static boole  _gles3_renderer_internal__build_pipeline(void);
static boole  _gles3_renderer_internal__build_text_pipeline(void);
static GLuint _gles3_renderer_internal__compile_shader(GLenum kind, char* src);
static GLuint _gles3_renderer_internal__link_program(GLuint vs, GLuint fs);
static void   _gles3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c);
static void   _gles3_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color_t c);
static void   _gles3_renderer_internal__flush_batches(void);
static boole  _gles3_renderer_internal__build_blur_pipeline(void);
static void   _gles3_renderer_internal__blur_resize_textures(int w, int h);
static void   _gles3_renderer_internal__capture_if_requested(void);

// ============================================================================
// PUBLIC: lifecycle
// ============================================================================
// renderer__init assumes the platform layer already made an EGL context
// current. native_window is unused on Android -- the platform already
// consumed the ANativeWindow when creating the EGL surface.

/**
 * @brief bring up the graphics context and initialize the per-frame pipeline. Called once at startup AFTER the platform layer has created its window. The renderer owns whatever GPU objects it needs (GL context, D3D device+swapchain, shaders, vertex buffers) and stores them in its own file-static state.
 *
 * @param void* native_window -> Platform window handle. On Windows this is HWND (`struct HWND__*`). Backends cast as needed.
 *
 * @return boole -> TRUE on success; FALSE if context creation, shader compilation, or resource allocation failed.
 */
static boole _gles3_renderer_internal__adopted;

boole renderer__init(void* native_window)
{
    (void)native_window;
    _gles3_renderer_internal__adopted=FALSE;
    stdlib__memset(&_gles3_renderer_internal__g, 0, sizeof(_gles3_renderer_internal__g));
    boole ok = _gles3_renderer_internal__build_pipeline();
    // Atlas owns GPU texture pages used for batched image draws. Init
    // it here -- needs a current GL context (guaranteed because
    // _build_pipeline above already required one). atlas__alloc fails
    // gracefully so a non-init or shutdown atlas just steers callers
    // to the per-image fallback path.
    atlas__init();
    return ok;
}

/**
 * @brief tear down the graphics context and free every GPU/CPU resource owned by the renderer. Symmetric with renderer__init; safe to call even if init failed partway.
 *
 * @return void
 */
boole renderer__init_adopt_gles3(void)
{
    if (!renderer__init(NULL_POINTER)) return FALSE;
    _gles3_renderer_internal__adopted=TRUE;
    return TRUE;
}

void renderer__shutdown(void)
{
    _gles3_renderer_internal__adopted=FALSE;
    // solid-rect pipeline objects
    if (_gles3_renderer_internal__g.vbo != 0)
    {
        glDeleteBuffers(1, &_gles3_renderer_internal__g.vbo);
    }
    if (_gles3_renderer_internal__g.vao != 0)
    {
        glDeleteVertexArrays(1, &_gles3_renderer_internal__g.vao);
    }
    if (_gles3_renderer_internal__g.program != 0)
    {
        glDeleteProgram(_gles3_renderer_internal__g.program);
    }
    if (_gles3_renderer_internal__g.cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_gles3_renderer_internal__g.cpu_verts);
    }

    // text pipeline objects
    if (_gles3_renderer_internal__g.text_vbo != 0)
    {
        glDeleteBuffers(1, &_gles3_renderer_internal__g.text_vbo);
    }
    if (_gles3_renderer_internal__g.text_vao != 0)
    {
        glDeleteVertexArrays(1, &_gles3_renderer_internal__g.text_vao);
    }
    if (_gles3_renderer_internal__g.text_program != 0)
    {
        glDeleteProgram(_gles3_renderer_internal__g.text_program);
    }
    if (_gles3_renderer_internal__g.text_cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_gles3_renderer_internal__g.text_cpu_verts);
    }

    // image pipeline (lazy-built; guarded against "never used" case).
    // Textures created via renderer__create_texture_rgba are owned by
    // widget_image and released through renderer__destroy_texture.
    if (_gles3_renderer_internal__g.image_vbo != 0)
    {
        glDeleteBuffers(1, &_gles3_renderer_internal__g.image_vbo);
    }
    if (_gles3_renderer_internal__g.image_vao != 0)
    {
        glDeleteVertexArrays(1, &_gles3_renderer_internal__g.image_vao);
    }
    if (_gles3_renderer_internal__g.image_program != 0)
    {
        glDeleteProgram(_gles3_renderer_internal__g.image_program);
    }
    if (_gles3_renderer_internal__g.image_cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_gles3_renderer_internal__g.image_cpu_verts);
    }

    // Blur ping-pong cleanup. Order: detach textures by deleting the
    // FBOs first (their attachments auto-detach on delete), then the
    // textures themselves, then the shader and quad VBO.
    if (_gles3_renderer_internal__g.blur_built)
    {
        if (_gles3_renderer_internal__g.blur_fbo[0] != 0 || _gles3_renderer_internal__g.blur_fbo[1] != 0)
        {
            glDeleteFramebuffers(2, _gles3_renderer_internal__g.blur_fbo);
        }
        if (_gles3_renderer_internal__g.blur_tex[0] != 0 || _gles3_renderer_internal__g.blur_tex[1] != 0)
        {
            glDeleteTextures(2, _gles3_renderer_internal__g.blur_tex);
        }
        if (_gles3_renderer_internal__g.blur_vbo != 0)
        {
            glDeleteBuffers(1, &_gles3_renderer_internal__g.blur_vbo);
        }
        if (_gles3_renderer_internal__g.blur_vao != 0)
        {
            glDeleteVertexArrays(1, &_gles3_renderer_internal__g.blur_vao);
        }
        if (_gles3_renderer_internal__g.blur_program != 0)
        {
            glDeleteProgram(_gles3_renderer_internal__g.blur_program);
        }
    }

    // Atlas pages own GL textures and must be deleted while a GL
    // context is current -- shutdown order: atlas first, then this
    // function's local cleanup completes, then platform tears down
    // the GL/EGL context.
    atlas__shutdown();

    stdlib__memset(&_gles3_renderer_internal__g, 0, sizeof(_gles3_renderer_internal__g));
}

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
    _gles3_renderer_internal__g.viewport_w       = viewport_w;
    _gles3_renderer_internal__g.viewport_h       = viewport_h;
    _gles3_renderer_internal__g.vert_count       = 0;
    _gles3_renderer_internal__g.text_vert_count  = 0;
    _gles3_renderer_internal__g.text_run_count   = 0;
    _gles3_renderer_internal__g.image_vert_count = 0;
    _gles3_renderer_internal__g.image_run_count  = 0;
    _gles3_renderer_internal__g.image_current_tex = 0;
    // Image-pipeline cache resets per frame -- GL binding lifetime
    // is "frame, give or take" (eglSwapBuffers + glClear may not
    // touch program/VAO state, but anything between frames is
    // outside our knowledge). Cheap to re-bind on first submit_image
    // each frame; consecutive submits within the frame reuse.
    _gles3_renderer_internal__g.image_state_bound = FALSE;
    _gles3_renderer_internal__g.image_last_tex    = 0;
    // current_text_atlas is intentionally preserved across frames;
    // the first font__draw call each frame rebinds it anyway. runs,
    // though, reset every frame: a run belongs to the frame that
    // produced it.

    glViewport(0, 0, (GLsizei)viewport_w, (GLsizei)viewport_h);
    if (!_gles3_renderer_internal__adopted) {
        glClearColor(clear.r, clear.g, clear.b, clear.a);
        glClear(GL_COLOR_BUFFER_BIT);
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
    if (_gles3_renderer_internal__g.vert_count + _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD > _gles3_renderer_internal__g.vert_cap)
    {
        return;
    }

    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f)   radius = 0.0f;
    if (radius > max_r)  radius = max_r;

    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

    _gles3_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c);
    _gles3_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
}

// Shadow: multi-layer concentric rects with quadratic falloff.
// See the opengl3 twin for the rationale.
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
    if (_gles3_renderer_internal__g.vert_count + 3 > _gles3_renderer_internal__g.vert_cap) return;
    // Expand the rectangle mask past the geometry. Triangle rasterization
    // owns the edges; the existing colour pipeline needs no extra shader.
    for (i = 0; i < 3; i++)
        _gles3_renderer_internal__push_vertex(vertices[i].x, vertices[i].y,
            vertices[i].x - left, vertices[i].y - top, width, height, 0, color);
}

void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    int64 count = 0;
    int64 i     = 0;
    float max_r = 0.0f;

    count = gradient__tessellate(g, r, scale, opacity, verts, GRADIENT_MAX_VERTS);

    if (count <= 0 || _gles3_renderer_internal__g.vert_count + count > _gles3_renderer_internal__g.vert_cap)
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
        _gles3_renderer_internal__push_vertex(verts[i].x, verts[i].y, verts[i].x - r.x, verts[i].y - r.y, r.w, r.h, radius, verts[i].c);
    }
}

// ============================================================================
// INTERNAL: flush_batches
// ============================================================================
// Draws whatever solid rects are currently buffered, then resets the
// count so subsequent submissions start a fresh batch. Called from
// end_frame and from push/pop_scissor (a state change can't be merged
// into a single Draw, so we flush before the change).

static void _gles3_renderer_internal__flush_image_state(void); // fwd; def near submit_image
static void _gles3_renderer_internal__flush_image_batch(void); // fwd; def near submit_image

static void _gles3_renderer_internal__flush_batches(void)
{
    // Note: image-pipeline state cache invalidation happens INSIDE
    // the rect / text branches below, not at function entry. That
    // way submit_image -> flush_batches with no rect/text queued
    // (the consecutive-image case) is a true no-op and preserves
    // the image-pipeline state for the next submit_image to reuse.
    // If we invalidated here unconditionally, every image submit
    // would re-bind program / VAO / VBO -- defeating the cache.

    // PASS 1: solid rects. Conditional -- if nothing's queued we
    // skip straight to the text pass rather than early-return for
    // the whole function. This is the bug the select popup was
    // hitting: if the caller pushed scissor with ONLY text queued
    // (not rects), the old early-return skipped the text flush,
    // so tree labels leaked out of the scissor bracket and drew
    // on top of the popup panel at end_frame.
    if (_gles3_renderer_internal__g.vert_count > 0)
    {
        // Rect pipeline shares program/VAO/VBO slots with the image
        // pipeline -- we're about to overwrite them. Invalidate the
        // image-state cache so a subsequent submit_image rebinds.
        _gles3_renderer_internal__g.image_state_bound = FALSE;
        _gles3_renderer_internal__g.image_last_tex    = 0;

        glUseProgram(_gles3_renderer_internal__g.program);
        glUniform2f(_gles3_renderer_internal__g.u_viewport_loc, (float)_gles3_renderer_internal__g.viewport_w, (float)_gles3_renderer_internal__g.viewport_h);

        glBindVertexArray(_gles3_renderer_internal__g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.vbo);

        // Buffer ORPHAN before SubData -- see opengl3_renderer.c for the
        // detailed rationale. Critical for mid-frame multi-flush correctness:
        // without it, the second SubData this frame races the first Draw
        // and produces flicker as alpha animations play out.
        GLsizeiptr bytes     = (GLsizeiptr)_gles3_renderer_internal__g.vert_count * (GLsizeiptr)sizeof(_gles3_renderer_internal__vertex_t);
        GLsizeiptr cap_bytes = (GLsizeiptr)_gles3_renderer_internal__g.vert_cap   * (GLsizeiptr)sizeof(_gles3_renderer_internal__vertex_t);
        glBufferData(GL_ARRAY_BUFFER, cap_bytes, NULL_POINTER, GL_DYNAMIC_DRAW); // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, _gles3_renderer_internal__g.cpu_verts);

        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)_gles3_renderer_internal__g.vert_count);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);

        _gles3_renderer_internal__g.vert_count = 0;
    }

    // PASS 2: text glyphs, one draw per atlas run. Runs after the
    // solid-rect pass so glyphs land on top of button bg rects in
    // the same flush. Conditional on its own batch state.
    // PASS 1.5: image batch. Drawn AFTER rects (so icons land on
    // top of card backgrounds) and BEFORE text (so labels render
    // on top of icons -- the natural launcher z-order). Each run
    // is a sequence of quads sharing one texture, drawn in one
    // glDrawArrays call. With the atlas, all icons collapse into
    // a single run; without it, runs alternate per texture.
    _gles3_renderer_internal__flush_image_batch();

    if (_gles3_renderer_internal__g.text_vert_count > 0 && _gles3_renderer_internal__g.text_run_count  > 0)
    {
        // Text pipeline also clobbers the image pipeline's program /
        // VAO / VBO bindings. Invalidate the cache (same as the
        // rect branch above).
        _gles3_renderer_internal__g.image_state_bound = FALSE;
        _gles3_renderer_internal__g.image_last_tex    = 0;

        glUseProgram(_gles3_renderer_internal__g.text_program);
        glUniform2f(_gles3_renderer_internal__g.text_u_viewport_loc, (float)_gles3_renderer_internal__g.viewport_w, (float)_gles3_renderer_internal__g.viewport_h);
        glUniform1i(_gles3_renderer_internal__g.text_u_atlas_loc, 0);
        glActiveTexture(GL_TEXTURE0);

        glBindVertexArray(_gles3_renderer_internal__g.text_vao);
        glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.text_vbo);

        GLsizeiptr tbytes     = (GLsizeiptr)_gles3_renderer_internal__g.text_vert_count * (GLsizeiptr)sizeof(_gles3_renderer_internal__text_vertex_t);
        GLsizeiptr tcap_bytes = (GLsizeiptr)_gles3_renderer_internal__g.text_vert_cap   * (GLsizeiptr)sizeof(_gles3_renderer_internal__text_vertex_t);
        glBufferData(GL_ARRAY_BUFFER, tcap_bytes, NULL_POINTER, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, tbytes, _gles3_renderer_internal__g.text_cpu_verts);

        for (int64 i = 0; i < _gles3_renderer_internal__g.text_run_count; i++)
        {
            _gles3_renderer_internal__text_run_t* run = &_gles3_renderer_internal__g.text_runs[i];
            glBindTexture(GL_TEXTURE_2D, run->atlas);
            glDrawArrays(GL_TRIANGLES, (GLint)run->vert_start, (GLsizei)run->vert_count);
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);

        _gles3_renderer_internal__g.text_vert_count = 0;
        _gles3_renderer_internal__g.text_run_count  = 0;
    }
}

// ============================================================================
// PUBLIC: renderer__push_scissor / renderer__pop_scissor
// ============================================================================
// GLES has the same glScissor / glEnable(GL_SCISSOR_TEST) shape as
// desktop GL. Each push/pop forces a flush of the current batch, then
// updates GPU scissor state. Stack-nested rects intersect (smaller of
// the two clips wins).
// GL's scissor uses BOTTOM-LEFT-origin coordinates; we flip Y from our
// top-left convention.

static gui_rect_t _gles3_renderer_internal__intersect(gui_rect_t a, gui_rect_t b)
{
    float x0  = (a.x > b.x) ? a.x : b.x;
    float y0  = (a.y > b.y) ? a.y : b.y;
    float x1a = a.x + a.w, x1b = b.x + b.w;
    float y1a = a.y + a.h, y1b = b.y + b.h;
    float x1 = (x1a < x1b) ? x1a : x1b;
    float y1 = (y1a < y1b) ? y1a : y1b;
    gui_rect_t r;
    r.x = x0;
    r.y = y0;
    r.w = (x1 > x0) ? (x1 - x0) : 0.0f;
    r.h = (y1 > y0) ? (y1 - y0) : 0.0f;
    return r;
}

// Compute the effective rect (intersection of every rect on the
// stack) without touching GPU state. Returns a w=h=0 rect when
// the stack is empty, signalling "no scissor active".
static gui_rect_t _gles3_renderer_internal__effective_scissor(void)
{
    gui_rect_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (_gles3_renderer_internal__g.scissor_depth <= 0)
    {
        return zero;
    }
    gui_rect_t r = _gles3_renderer_internal__g.scissor_stack[0];
    for (int64 i = 1; i < _gles3_renderer_internal__g.scissor_depth; i++)
    {
        r = _gles3_renderer_internal__intersect(r, _gles3_renderer_internal__g.scissor_stack[i]);
    }
    return r;
}

static void _gles3_renderer_internal__apply_scissor_top(void)
{
    if (_gles3_renderer_internal__g.scissor_depth <= 0)
    {
        glDisable(GL_SCISSOR_TEST);
        _gles3_renderer_internal__g.scissor_last_valid = FALSE;
        return;
    }
    gui_rect_t r = _gles3_renderer_internal__effective_scissor();
    GLint   gl_x = (GLint)r.x;
    // Quantize top-left edges before flipping Y, matching the other renderer backends.
    GLint   top = (GLint)r.y;
    GLint   right = (GLint)(r.x + r.w);
    GLint   bottom = (GLint)(r.y + r.h);
    GLint   gl_y = (GLint)_gles3_renderer_internal__g.viewport_h - bottom;
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
    _gles3_renderer_internal__g.scissor_last_applied = r;
    _gles3_renderer_internal__g.scissor_last_valid   = TRUE;
}

// Same-rect optimization: if the new effective rect equals what
// we last applied, skip both the batch flush (expensive) and the
// glScissor call. Nested containers that resolve to the same
// intersected clip thus produce one draw call instead of N.
static boole _gles3_renderer_internal__rects_equal(gui_rect_t a, gui_rect_t b)
{
    return (boole)(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
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
    if (_gles3_renderer_internal__g.scissor_depth >= (int64)(sizeof(_gles3_renderer_internal__g.scissor_stack) / sizeof(_gles3_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("%s", "renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    // Tentatively push so effective_scissor sees the new rect, then
    // compare against last_applied. If unchanged, leave the batch
    // in place (no flush, no GPU state update). If changed, flush
    // and apply.
    _gles3_renderer_internal__g.scissor_stack[_gles3_renderer_internal__g.scissor_depth++] = rect;
    gui_rect_t new_eff = _gles3_renderer_internal__effective_scissor();
    if (_gles3_renderer_internal__g.scissor_last_valid && _gles3_renderer_internal__rects_equal(new_eff, _gles3_renderer_internal__g.scissor_last_applied))
    {
        return;
    }
    _gles3_renderer_internal__flush_batches();
    _gles3_renderer_internal__apply_scissor_top();
}

/**
 * @brief pop the most recent scissor rect off the stack. If the stack is now empty, scissor testing is disabled entirely (everything draws).
 *
 * @return void
 */
void renderer__pop_scissor(void)
{
    if (_gles3_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("%s", "renderer__pop_scissor: stack already empty");
        return;
    }
    _gles3_renderer_internal__g.scissor_depth--;
    gui_rect_t new_eff = _gles3_renderer_internal__effective_scissor();
    boole becoming_disabled = (_gles3_renderer_internal__g.scissor_depth == 0);
    if (becoming_disabled == FALSE && _gles3_renderer_internal__g.scissor_last_valid && _gles3_renderer_internal__rects_equal(new_eff, _gles3_renderer_internal__g.scissor_last_applied))
    {
        return;
    }
    _gles3_renderer_internal__flush_batches();
    _gles3_renderer_internal__apply_scissor_top();
}

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

    // Drain pending batches so the backbuffer reflects everything
    // queued before this blur call (otherwise the blit below copies
    // stale pixels from before the rects/text we want to blur).
    _gles3_renderer_internal__flush_batches();

    int vw = (int)_gles3_renderer_internal__g.viewport_w;
    int vh = (int)_gles3_renderer_internal__g.viewport_h;

    // Clamp to viewport. Blitting past the framebuffer edges is
    // undefined per spec.
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

    if (_gles3_renderer_internal__build_blur_pipeline() == FALSE)
    {
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }
    _gles3_renderer_internal__state_t* s = &_gles3_renderer_internal__g;

    // Pick a downsample factor that keeps the working textures small.
    // Power-of-two so glBlitFramebuffer's GL_LINEAR resample is well-
    // defined; capped at 384 px on the long edge so the 33-tap shader
    // kernel covers a generous sigma range without thrashing texture
    // cache. For a 1080x2246 panel-fill blur we end up at ~270x562
    // (ds=4) -- two orders of magnitude less work than the previous
    // CPU path's 528M-op separable Gaussian.
    int ds = 1;
    while ((w / ds) > 384 || (h / ds) > 384)
    {
        ds *= 2;
    }
    int tw = w / ds;
    int th = h / ds;
    if (tw < 1)
    {
        tw = 1;
    }
    if (th < 1)
    {
        th = 1;
    }

    _gles3_renderer_internal__blur_resize_textures(tw, th);

    // GLES origin is bottom-left; meek-ui's coords are top-left.
    // Mirror Y for the source rect when feeding glBlitFramebuffer.
    int gl_y0 = vh - (y + h);
    int gl_y1 = vh - y;

    // Step 1: downsample backbuffer rect into blur_tex[0] via blit.
    // Read FB = default (0), draw FB = blur_fbo[0]. GL_LINEAR filter
    // gives us a free 2x box-average per ds level.
    // Y-flip during the blit: GL's default-FB Y is bottom-up, the
    // image pipeline samples textures expecting top-row-first
    // (matches what create_texture_rgba's CPU upload path uses).
    // Swapping dstY0 and dstY1 makes glBlitFramebuffer mirror in Y
    // so the texture comes out in top-down convention. Without
    // this, submit_image draws the blur upside-down and shifted --
    // it visually reads as if the panel is showing a snapshot of
    // a totally different region of the screen.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->blur_fbo[0]);
    glBlitFramebuffer(x, gl_y0, x + w, gl_y1, 0, th,   tw,    0, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Step 2 + 3: separable Gaussian. Render fullscreen quad twice,
    // first sampling tex[0] -> tex[1] (horizontal), then tex[1] ->
    // tex[0] (vertical). Sigma scales by ds since we're working in
    // the downsampled texture's coordinate space.
    float small_sigma = sigma_px / (float)ds;
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s->blur_program);
    glUniform1i(s->blur_u_src_loc,   0);
    glUniform1f(s->blur_u_sigma_loc, small_sigma);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s->blur_vao);

    glBindFramebuffer(GL_FRAMEBUFFER, s->blur_fbo[1]);
    glViewport(0, 0, tw, th);
    glBindTexture(GL_TEXTURE_2D, s->blur_tex[0]);
    glUniform2f(s->blur_u_step_loc, 1.0f / (float)tw, 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, s->blur_fbo[0]);
    glViewport(0, 0, tw, th);
    glBindTexture(GL_TEXTURE_2D, s->blur_tex[1]);
    glUniform2f(s->blur_u_step_loc, 0.0f, 1.0f / (float)th);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Restore default framebuffer, viewport, and the per-frame blend
    // state the rect/text/image pipelines expect.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)vw, (GLsizei)vh);
    glEnable(GL_BLEND);
    glBindVertexArray(0);

    // Re-apply the active scissor (if any) so subsequent rect/image
    // draws clip correctly. The blur passes above disabled SCISSOR
    // so the fullscreen quad covered the whole tex.
    if (s->scissor_depth > 0 && s->scissor_last_valid)
    {
        gui_rect_t r = s->scissor_last_applied;
        glEnable(GL_SCISSOR_TEST);
        int sx = (int)r.x;
        int sy = vh - (int)(r.y + r.h);
        int sw = (int)(r.x + r.w) - sx;
        int sh = (int)(r.y + r.h) - (int)r.y;
        glScissor(sx, sy, sw, sh);
    }

    // Step 4: draw the blurred small texture back at the original
    // rect via the image pipeline. submit_image takes the texture
    // handle as a void* (cast from GLuint via uintptr_t) and draws
    // a textured quad with bilinear filtering -- which transparently
    // upsamples our small blur_tex[0] back to rect.w x rect.h.
    gui_rect_t draw_rect;
    draw_rect.x = (float)x;
    draw_rect.y = (float)y;
    draw_rect.w = (float)w;
    draw_rect.h = (float)h;
    gui_color_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
    renderer__submit_image(draw_rect, (void*)(uintptr_t)s->blur_tex[0], white);

    // Force the draw so the texture is sampled before any subsequent
    // blur_region call could overwrite blur_tex[0]. We deliberately
    // do NOT destroy the texture here -- it lives for the renderer's
    // lifetime as part of the blur ping-pong pair.
    _gles3_renderer_internal__flush_batches();
}

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
    _gles3_renderer_internal__flush_batches();
}

/**
 * @brief finalize the frame: upload the batched verts, issue the draw, then present the back buffer to the window (swap buffers / swapchain Present). After this call returns, the frame is on screen.
 *
 * @return void
 */
void renderer__end_frame(void)
{
    // note: eglSwapBuffers is called by platform_android, not here. on
    // Android the platform owns the EGLSurface and therefore the
    // present. same split as what a future refactor would do on Windows.
    // flush_batches drains BOTH pending rect and text batches in the
    // right order (rects first, then text-on-top), so one call is
    // enough here. Scissor push/pop during the frame may have drained
    // partial queues already; the final call flushes whatever's left.
    _gles3_renderer_internal__flush_batches();
    // Image pipeline may have been left bound across the frame (if
    // the last submit was an image, no rect/text after). Tear it
    // down so GL state at frame end is the same regardless of last-
    // call type -- prevents surprises in cross-frame consumers
    // (texture imports, blur helpers) that assume a baseline state.
    _gles3_renderer_internal__flush_image_state();

    // defensive scissor cleanup: if a widget pushed but failed to pop,
    // the next frame would inherit the stale scissor. clear and disable.
    if (_gles3_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("%s %lld %s", "renderer__end_frame: scissor stack non-empty (", (long long)_gles3_renderer_internal__g.scissor_depth, "); resetting");
        _gles3_renderer_internal__g.scissor_depth = 0;
    }
    glDisable(GL_SCISSOR_TEST);

    // the armed frame capture reads the finished back buffer here, before
    // the platform presents it (eglSwapBuffers lives in the platform layer)
    _gles3_renderer_internal__capture_if_requested();
}

// ============================================================================
// PUBLIC: text pipeline -- STUBS for the MVP Android port
// ============================================================================
// font.c calls these during font rasterization. returning NULL from
// create_atlas_r8 cascades through: font__at returns NULL, font__draw
// does nothing, widgets with text render only their bg rect. this lets
// the solid-rect path stand alone on Android until we add the textured
// glyph pipeline here (mirror of the opengl3 backend's text pass).

// Upload an R8 coverage bitmap to a fresh texture. font.c calls us
// once per (family, size) pair; we return the GLuint cast to a void*
// and font.c holds onto it across frames. LINEAR filtering +
// CLAMP_TO_EDGE matches the opengl3 backend (same VISUAL CONTRACT).
// GLES 3.0 supports GL_R8 as a sized internal format + GL_RED as a
// transfer format, so the upload shape matches opengl3 exactly.
// GL_UNPACK_ALIGNMENT is set to 1 before the upload because an R8
// bitmap's row stride is one byte per pixel; leaving alignment at
// the GL default of 4 corrupts rows whose width isn't a multiple of
// 4 (our 1024/2048/4096 power-of-two atlases happen to be safe but
// we set alignment defensively).
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
        log_error("%s", "gles3: glGenTextures failed for text atlas");
        return NULL_POINTER;
    }
    glBindTexture(GL_TEXTURE_2D, tex);

    GLint prev_align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)width, (GLsizei)height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);

    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    // cast GLuint -> uintptr_t -> void* so the handle round-trips
    // through font.c's void* storage without tripping -Wint-to-pointer
    // warnings (GLuint is 32-bit, void* is 64-bit on arm64).
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
    glDeleteTextures(1, &tex);
}

// select which atlas the NEXT submit_text_glyph calls will use.
// if the atlas differs from the one active in the current run,
// we open a new run (sealing off the previous one).
/**
 * @brief bind the atlas to be sampled by subsequent renderer__submit_text_glyph calls (until either another atlas is bound or the frame ends). The text draw pass uses the last-bound atlas -- so all text in one frame that uses different fonts needs to be submitted in font-coherent runs.
 *
 * @param void* atlas -> Handle from create_atlas_r8.
 *
 * @return void
 */
void renderer__set_text_atlas(void* atlas)
{
    GLuint tex = (GLuint)(uintptr_t)atlas;
    if (tex == _gles3_renderer_internal__g.current_text_atlas)
    {
        return;
    }
    _gles3_renderer_internal__g.current_text_atlas = tex;

    // force the next submit_text_glyph to open a new run by
    // marking "the in-progress run (if any) is closed". we leave
    // text_run_count alone -- a new run is opened lazily when the
    // next vertex actually arrives.
    if (_gles3_renderer_internal__g.text_run_count > 0)
    {
        _gles3_renderer_internal__text_run_t* last = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count - 1];
        if (last->atlas != tex)
        {
            // mark it closed by ensuring the next vertex goes to a
            // fresh run. submit_text_glyph detects "top run's atlas
            // != current_text_atlas" and opens a new entry.
        }
    }
}

// append six vertices for one glyph quad. extends the current run
// if its atlas matches current_text_atlas; otherwise opens a new run.
// no-op if we'd exceed the per-frame glyph cap.
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
    if (_gles3_renderer_internal__g.current_text_atlas == 0)
    {
        return; // no atlas set yet; caller should set_text_atlas first
    }
    GLuint atlas = _gles3_renderer_internal__g.current_text_atlas;
    if (_gles3_renderer_internal__g.text_vert_cap < _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }
    boole new_run = (boole)(_gles3_renderer_internal__g.text_run_count == 0 ||
        _gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count - 1].atlas != atlas);
    if (_gles3_renderer_internal__g.text_vert_count + _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD > _gles3_renderer_internal__g.text_vert_cap ||
        (new_run && _gles3_renderer_internal__g.text_run_count >= _GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS))
    {
        // Keep the active scissor and draw order, then continue this string.
        // The flush resets the atlas, so restore it for subsequent glyphs.
        _gles3_renderer_internal__flush_batches();
        _gles3_renderer_internal__g.current_text_atlas = atlas;
    }

    // decide whether to extend the current run or open a new one.
    // new-run conditions: no runs yet, OR top run's atlas differs.
    boole need_new_run = TRUE;
    if (_gles3_renderer_internal__g.text_run_count > 0)
    {
        _gles3_renderer_internal__text_run_t* last = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count - 1];
        if (last->atlas == _gles3_renderer_internal__g.current_text_atlas)
        {
            need_new_run = FALSE;
        }
    }

    if (need_new_run)
    {
        _gles3_renderer_internal__text_run_t* run = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count++];
        run->atlas      = _gles3_renderer_internal__g.current_text_atlas;
        run->vert_start = _gles3_renderer_internal__g.text_vert_count;
        run->vert_count = 0;
    }

    // push the six vertices for this glyph. quad winding matches the
    // solid-rect pipeline: TL, TR, BL, TR, BR, BL -> two triangles.
    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;
    float u0 = uv.x;
    float v0 = uv.y;
    float u1 = uv.x + uv.w;
    float v1 = uv.y + uv.h;

    _gles3_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _gles3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _gles3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _gles3_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _gles3_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _gles3_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);

    // extend the current run by the six verts we just pushed.
    _gles3_renderer_internal__text_run_t* run = &_gles3_renderer_internal__g.text_runs[_gles3_renderer_internal__g.text_run_count - 1];
    run->vert_count += _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

// ============================================================================
// INTERNAL: pipeline
// ============================================================================

static boole _gles3_renderer_internal__build_pipeline(void)
{
    GLuint vs = _gles3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   _gles3_renderer_internal__vs_src);
    GLuint fs = _gles3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, _gles3_renderer_internal__fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.program = _gles3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_gles3_renderer_internal__g.program == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.u_viewport_loc =
        glGetUniformLocation(_gles3_renderer_internal__g.program, "u_viewport");

    glGenVertexArrays(1, &_gles3_renderer_internal__g.vao);
    glGenBuffers(1,      &_gles3_renderer_internal__g.vbo);

    glBindVertexArray(_gles3_renderer_internal__g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.vbo);

    GLsizeiptr max_bytes = (GLsizeiptr)(_GLES3_RENDERER_INTERNAL__MAX_QUADS* _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_gles3_renderer_internal__vertex_t);
    glBufferData(GL_ARRAY_BUFFER, max_bytes, NULL_POINTER, GL_DYNAMIC_DRAW);

    GLsizei stride = (GLsizei)sizeof(_gles3_renderer_internal__vertex_t);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex_t, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex_t, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex_t, lx));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex_t, rect_w));

    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__vertex_t, radius));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    _gles3_renderer_internal__g.vert_cap = _GLES3_RENDERER_INTERNAL__MAX_QUADS * _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _gles3_renderer_internal__g.cpu_verts = (_gles3_renderer_internal__vertex_t*)GUI_MALLOC_T((size_t)_gles3_renderer_internal__g.vert_cap * sizeof(_gles3_renderer_internal__vertex_t), MM_TYPE__RENDERER);
    if (_gles3_renderer_internal__g.cpu_verts == NULL_POINTER)
    {
        log_error("%s", "gles3: out of memory for cpu vertex buffer");
        return FALSE;
    }

    // Alpha blending. Same separate-alpha setup as opengl3 -- preserves
    // dst.a = 1 across overlapping draws so the framebuffer's alpha
    // channel doesn't decay during animations. Some Android compositors
    // multiply by framebuffer alpha at present time; we want fully
    // opaque output regardless of how many translucent rects we draw.
    // Part of the VISUAL CONTRACT in renderer.h.
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Text pipeline (second shader + VAO + VBO + cpu staging). Must
    // succeed or the whole renderer__init fails -- host apps can
    // gracefully handle that, but silently having no text pipeline
    // after init would produce invisible text with no error signal.
    if (_gles3_renderer_internal__build_text_pipeline() == FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

static GLuint _gles3_renderer_internal__compile_shader(GLenum kind, char* src)
{
    GLuint sh = glCreateShader(kind);
    if (sh == 0)
    {
        log_error("%s", "gles3: glCreateShader returned 0");
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
            char* log_buf = (char*)GUI_MALLOC(len);
            if (log_buf != NULL_POINTER)
            {
                glGetShaderInfoLog(sh, len, NULL_POINTER, log_buf);
                log_error("%s %s", "gles3: shader compile error:\n", log_buf);
                GUI_FREE(log_buf);
            }
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint _gles3_renderer_internal__link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    if (p == 0)
    {
        log_error("%s", "gles3: glCreateProgram returned 0");
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
            char* log_buf = (char*)GUI_MALLOC(len);
            if (log_buf != NULL_POINTER)
            {
                glGetProgramInfoLog(p, len, NULL_POINTER, log_buf);
                log_error("%s %s", "gles3: program link error:\n", log_buf);
                GUI_FREE(log_buf);
            }
        }
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void _gles3_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c)
{
    _gles3_renderer_internal__vertex_t* v = &_gles3_renderer_internal__g.cpu_verts[_gles3_renderer_internal__g.vert_count++];
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

static void _gles3_renderer_internal__push_text_vertex(float x, float y, float u, float v_uv, gui_color_t c)
{
    _gles3_renderer_internal__text_vertex_t* tv =
        &_gles3_renderer_internal__g.text_cpu_verts[_gles3_renderer_internal__g.text_vert_count++];
    tv->x = x;
    tv->y = y;
    tv->r = c.r;
    tv->g = c.g;
    tv->b = c.b;
    tv->a = c.a;
    tv->u = u;
    tv->v = v_uv;
}

// build the text pipeline's shaders, VAO, VBO, cpu staging buffer.
// mirrors the solid-rect setup but with the simpler vertex format
// (pos + color + uv; no SDF bookkeeping). called at the end of
// build_pipeline so a single init failure is noticed before we
// return TRUE from renderer__init.
static boole _gles3_renderer_internal__build_text_pipeline(void)
{
    GLuint vs = _gles3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   _gles3_renderer_internal__text_vs_src);
    GLuint fs = _gles3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, _gles3_renderer_internal__text_fs_src);
    if (vs == 0 || fs == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.text_program = _gles3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (_gles3_renderer_internal__g.text_program == 0)
    {
        return FALSE;
    }

    _gles3_renderer_internal__g.text_u_viewport_loc =
        glGetUniformLocation(_gles3_renderer_internal__g.text_program, "u_viewport");
    _gles3_renderer_internal__g.text_u_atlas_loc =
        glGetUniformLocation(_gles3_renderer_internal__g.text_program, "u_atlas");

    glGenVertexArrays(1, &_gles3_renderer_internal__g.text_vao);
    glGenBuffers(1,      &_gles3_renderer_internal__g.text_vbo);

    glBindVertexArray(_gles3_renderer_internal__g.text_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.text_vbo);

    GLsizeiptr max_text_bytes = (GLsizeiptr)(_GLES3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS* _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD) * (GLsizeiptr)sizeof(_gles3_renderer_internal__text_vertex_t);
    glBufferData(GL_ARRAY_BUFFER, max_text_bytes, NULL_POINTER, GL_DYNAMIC_DRAW);

    GLsizei stride = (GLsizei)sizeof(_gles3_renderer_internal__text_vertex_t);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__text_vertex_t, x));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__text_vertex_t, r));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(_gles3_renderer_internal__text_vertex_t, u));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    _gles3_renderer_internal__g.text_vert_cap  = _GLES3_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _gles3_renderer_internal__g.text_cpu_verts = (_gles3_renderer_internal__text_vertex_t*)GUI_MALLOC_T((size_t)_gles3_renderer_internal__g.text_vert_cap * sizeof(_gles3_renderer_internal__text_vertex_t), MM_TYPE__RENDERER);
    if (_gles3_renderer_internal__g.text_cpu_verts == NULL_POINTER)
    {
        log_error("%s", "gles3: out of memory for text cpu vertex buffer");
        return FALSE;
    }

    // Image-batch CPU staging. 8 floats per vert * 6 verts per
    // quad = 48 floats per image. Capacity of 2048 images per
    // frame is comfortably above the visible-tile count (~50
    // during a swipe) with headroom for compounds like the
    // task-switcher card thumbnails.
    #define _GLES3_RENDERER_INTERNAL__MAX_IMAGE_QUADS 2048
    _gles3_renderer_internal__g.image_vert_cap   = _GLES3_RENDERER_INTERNAL__MAX_IMAGE_QUADS * _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _gles3_renderer_internal__g.image_cpu_verts  = (float*)GUI_MALLOC_T((size_t)_gles3_renderer_internal__g.image_vert_cap * 8 * sizeof(float), MM_TYPE__RENDERER);
    if (_gles3_renderer_internal__g.image_cpu_verts == NULL_POINTER)
    {
        log_error("%s", "gles3: out of memory for image cpu vertex buffer");
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// IMAGE PIPELINE (RGBA textures, one-quad-per-submit, lazy-initialized)
// ============================================================================

#include "shaders/generated/image.vert.h"

#include "shaders/generated/image.frag.h"

static boole _gles3_renderer_internal__ensure_image_pipeline(void)
{
    if (_gles3_renderer_internal__g.image_program != 0)
    {
        return TRUE;
    }
    GLuint vs = _gles3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   (char*)_GLES3_RENDERER_INTERNAL__IMAGE_VS);
    GLuint fs = _gles3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, (char*)_GLES3_RENDERER_INTERNAL__IMAGE_FS);
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
    GLuint prog = _gles3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (prog == 0)
    {
        return FALSE;
    }
    _gles3_renderer_internal__g.image_program        = prog;
    _gles3_renderer_internal__g.image_u_viewport_loc = glGetUniformLocation(prog, "u_viewport");
    _gles3_renderer_internal__g.image_u_tex_loc      = glGetUniformLocation(prog, "u_tex");

    glGenVertexArrays(1, &_gles3_renderer_internal__g.image_vao);
    glGenBuffers(1, &_gles3_renderer_internal__g.image_vbo);
    glBindVertexArray(_gles3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.image_vbo);

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

// ============================================================================
// Blur pipeline (GPU separable Gaussian via FBO ping-pong)
// ============================================================================

// Vertex shader -- fullscreen-quad pass-through. The blur VBO holds 4
// vertices in NDC clip space (-1..1) plus matching uvs (0..1); the VS
// just passes both through.
#include "shaders/generated/blur.vert.h"

// Fragment shader -- one pass of a separable Gaussian. u_step is the
// texel step direction in source-uv space: (1.0/tw, 0) for the
// horizontal pass, (0, 1.0/th) for the vertical. Kernel radius is
// fixed at 16 (33 taps total) so the for-loop has a constant bound;
// u_sigma scales the weights inside that fixed radius. Effective
// kernel radius = 3*sigma -- weights past the cutoff are tiny but
// the cost of evaluating them is the same. Capping the radius at 16
// taps means the caller should downsample the source so the effective
// (sigma_in_texture_space) stays in roughly the 1..6 range; the
// blur_resize_textures helper picks a downsample factor that keeps
// the texture <= 384 px on each edge for that reason.
#include "shaders/generated/blur.frag.h"

static boole _gles3_renderer_internal__build_blur_pipeline(void)
{
    if (_gles3_renderer_internal__g.blur_built)
    {
        return TRUE;
    }
    GLuint vs = _gles3_renderer_internal__compile_shader(GL_VERTEX_SHADER,   (char*)_GLES3_RENDERER_INTERNAL__BLUR_VS);
    GLuint fs = _gles3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, (char*)_GLES3_RENDERER_INTERNAL__BLUR_FS);
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
    GLuint prog = _gles3_renderer_internal__link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (prog == 0)
    {
        return FALSE;
    }
    _gles3_renderer_internal__g.blur_program     = prog;
    _gles3_renderer_internal__g.blur_u_src_loc   = glGetUniformLocation(prog, "u_src");
    _gles3_renderer_internal__g.blur_u_step_loc  = glGetUniformLocation(prog, "u_step");
    _gles3_renderer_internal__g.blur_u_sigma_loc = glGetUniformLocation(prog, "u_sigma");

    // Fullscreen-quad VBO: 4 vertices, triangle-strip layout. Stored
    // as (pos.x, pos.y, uv.x, uv.y) tuples so a single 4-float stride
    // covers the VS inputs.
    static const float QUAD_VERTS[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenVertexArrays(1, &_gles3_renderer_internal__g.blur_vao);
    glGenBuffers(1, &_gles3_renderer_internal__g.blur_vbo);
    glBindVertexArray(_gles3_renderer_internal__g.blur_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.blur_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);
    GLsizei stride = (GLsizei)(4 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Two ping-pong textures + matching FBOs. Allocated empty here at
    // 0x0; blur_resize_textures grows them on first use to match the
    // requested rect size (downsampled).
    glGenTextures(2, _gles3_renderer_internal__g.blur_tex);
    glGenFramebuffers(2, _gles3_renderer_internal__g.blur_fbo);
    for (int i = 0; i < 2; i++)
    {
        glBindTexture(GL_TEXTURE_2D, _gles3_renderer_internal__g.blur_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    _gles3_renderer_internal__g.blur_tex_w = 0;
    _gles3_renderer_internal__g.blur_tex_h = 0;
    _gles3_renderer_internal__g.blur_built = TRUE;
    return TRUE;
}

// Resize both ping-pong textures to (w, h) and re-attach to their
// FBOs. Called by blur_region when the requested size changes from
// the previous call.
static void _gles3_renderer_internal__blur_resize_textures(int w, int h)
{
    _gles3_renderer_internal__state_t* s = &_gles3_renderer_internal__g;
    if (s->blur_tex_w == w && s->blur_tex_h == h)
    {
        return;
    }
    for (int i = 0; i < 2; i++)
    {
        glBindTexture(GL_TEXTURE_2D, s->blur_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL_POINTER);
        glBindFramebuffer(GL_FRAMEBUFFER, s->blur_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->blur_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s->blur_tex_w = w;
    s->blur_tex_h = h;
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
static void* _gles3_renderer_internal__create_texture_rgba(const ubyte* rgba, int width, int height, boole generate_mips)
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
    return _gles3_renderer_internal__create_texture_rgba(rgba, width, height, TRUE);
}

// The compositor's final surface is displayed at one texel per viewport pixel.
// Its lower mip levels would be rebuilt and uploaded every frame but never used.
GUI_API void* renderer_backend__create_frame_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _gles3_renderer_internal__create_texture_rgba(rgba, width, height, FALSE);
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

// Tear down the legacy image-pipeline state cache. Carried over
// from the pre-batching path; effectively a no-op now since
// flush_image_batch handles all image draws and never leaves
// state bound across a flush. Kept for API stability (end_frame
// + flush_batches still call it) so a future non-batched submit
// could re-enable cache behaviour without re-introducing the
// teardown wiring.
static void _gles3_renderer_internal__flush_image_state(void)
{
    if (_gles3_renderer_internal__g.image_state_bound == FALSE)
    {
        return;
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    _gles3_renderer_internal__g.image_state_bound = FALSE;
    _gles3_renderer_internal__g.image_last_tex    = 0;
}

// Append (or extend) an image run. If the incoming texture
// matches the current run's texture, just bump that run's
// vert_count. Otherwise start a new run. If the run table is
// full, flush the batch inline so we can reset and start fresh
// -- still better than the old "flush per submit" path because
// we only flush at the 32-run boundary, not per image.
static void _gles3_renderer_internal__flush_image_batch(void); // fwd; defined below

static void _gles3_renderer_internal__push_image_run(GLuint tex, int64 quad_vert_count)
{
    if (_gles3_renderer_internal__g.image_run_count > 0)
    {
        _gles3_renderer_internal__text_run_t* last =
            &_gles3_renderer_internal__g.image_runs[_gles3_renderer_internal__g.image_run_count - 1];
        if (last->atlas == tex)
        {
            last->vert_count += quad_vert_count;
            return;
        }
    }
    if (_gles3_renderer_internal__g.image_run_count >= _GLES3_RENDERER_INTERNAL__MAX_TEXT_RUNS)
    {
        // Run table is full. Flush what we have, reset, then
        // start a new run for this texture.
        _gles3_renderer_internal__flush_image_batch();
    }
    _gles3_renderer_internal__text_run_t* run =
        &_gles3_renderer_internal__g.image_runs[_gles3_renderer_internal__g.image_run_count++];
    run->atlas      = tex;
    run->vert_start = _gles3_renderer_internal__g.image_vert_count - quad_vert_count;
    run->vert_count = quad_vert_count;
}

/**
 * @brief uV-aware variant of submit_image. Samples the source texture using the explicit (u0,v0,u1,v1) rect instead of the full [0,1]^2 region. Required by the atlas path: many small icons share one big GL texture, each addressing a different sub-rect. Consecutive submit_image / submit_image_uv calls targeting the same `tex` get batched into a single draw call -- 50 icons sharing one atlas collapse to one glDrawArrays. Backwards- compatible submit_image is implemented as submit_image_uv with uv=(0,0,1,1).
 *
 * @param gui_rect_t rect -> Destination rect in pixel coords.
 * @param void* tex -> Texture handle. NULL = skip draw.
 * @param gui_color_t tint -> Multiplied against the sampled RGBA.
 * @param float u0 -> ,v0,u1,v1 Source UV in [0,1]. (0,0,1,1) = full texture.
 *
 * @return void
 */
void renderer__submit_image_uv(gui_rect_t rect, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    if (tex == NULL_POINTER)
    {
        return;
    }
    if (_gles3_renderer_internal__g.image_cpu_verts == NULL_POINTER)
    {
        return;
    }

    // Out of vert capacity for this frame? Flush so we can start
    // fresh. ~2048 image quads is more than the visible-icon count
    // by an order of magnitude, so this branch should rarely fire.
    if (_gles3_renderer_internal__g.image_vert_count + _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD > _gles3_renderer_internal__g.image_vert_cap)
    {
        _gles3_renderer_internal__flush_image_batch();
    }

    float x0 = (float)rect.x;
    float y0 = (float)rect.y;
    float x1 = x0 + (float)rect.w;
    float y1 = y0 + (float)rect.h;
    float r = tint.r, g = tint.g, b = tint.b, a = tint.a;

    // Append 6 verts (two triangles) directly into the CPU buffer.
    // Vertex layout matches the image-pipeline's VAO: (pos, color,
    // uv) -- 8 floats / 32 bytes per vertex.
    float* v = _gles3_renderer_internal__g.image_cpu_verts + _gles3_renderer_internal__g.image_vert_count * 8;
    *v++ = x0; *v++ = y0; *v++ = r; *v++ = g; *v++ = b; *v++ = a; *v++ = u0; *v++ = v0;
    *v++ = x1; *v++ = y0; *v++ = r; *v++ = g; *v++ = b; *v++ = a; *v++ = u1; *v++ = v0;
    *v++ = x0; *v++ = y1; *v++ = r; *v++ = g; *v++ = b; *v++ = a; *v++ = u0; *v++ = v1;
    *v++ = x1; *v++ = y0; *v++ = r; *v++ = g; *v++ = b; *v++ = a; *v++ = u1; *v++ = v0;
    *v++ = x1; *v++ = y1; *v++ = r; *v++ = g; *v++ = b; *v++ = a; *v++ = u1; *v++ = v1;
    *v++ = x0; *v++ = y1; *v++ = r; *v++ = g; *v++ = b; *v++ = a; *v++ = u0; *v++ = v1;
    _gles3_renderer_internal__g.image_vert_count += _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD;

    _gles3_renderer_internal__push_image_run((GLuint)(uintptr_t)tex, _GLES3_RENDERER_INTERNAL__VERTS_PER_QUAD);
}

/**
 * @brief draw one image quad immediately. Flushes any pending solid-rect or text batch first so previously-submitted content stays underneath and subsequent submissions stay on top. Full-texture draw (UV 0..1 on both axes); no subregion support in this pass (crop via node bounds + scissor instead).
 *
 * @param gui_rect_t rect -> Destination rect in top-left-origin pixel coords.
 * @param void* tex -> Texture handle from create_texture_rgba. NULL = skip draw.
 * @param gui_color_t tint -> Multiplied against the sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 *
 * @return void
 */
void renderer__submit_image(gui_rect_t rect, void* tex, gui_color_t tint)
{
    // Backwards-compatible wrapper: full-texture UV. Existing
    // callers that don't care about UV subregions go through this
    // path and get batching for free.
    renderer__submit_image_uv(rect, tex, tint, 0.0f, 0.0f, 1.0f, 1.0f);
}

// Drain the accumulated image batch: one glDrawArrays per run
// (per texture), all from the same VBO upload. Mirrors the text
// flush's shape -- only difference is uniform layout (image
// shader's sampler is u_tex) and vertex format (matches image
// VAO setup, identical layout to text vertices: pos, color, uv).
static void _gles3_renderer_internal__flush_image_batch(void)
{
    if (_gles3_renderer_internal__g.image_vert_count == 0 || _gles3_renderer_internal__g.image_run_count  == 0)
    {
        return;
    }
    if (_gles3_renderer_internal__ensure_image_pipeline() == FALSE)
    {
        _gles3_renderer_internal__g.image_vert_count = 0;
        _gles3_renderer_internal__g.image_run_count  = 0;
        return;
    }

    glUseProgram(_gles3_renderer_internal__g.image_program);
    if (_gles3_renderer_internal__g.image_u_viewport_loc >= 0)
    {
        glUniform2f(_gles3_renderer_internal__g.image_u_viewport_loc, (float)_gles3_renderer_internal__g.viewport_w, (float)_gles3_renderer_internal__g.viewport_h);
    }
    if (_gles3_renderer_internal__g.image_u_tex_loc >= 0)
    {
        glUniform1i(_gles3_renderer_internal__g.image_u_tex_loc, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(_gles3_renderer_internal__g.image_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _gles3_renderer_internal__g.image_vbo);

    GLsizeiptr bytes     = (GLsizeiptr)_gles3_renderer_internal__g.image_vert_count * (GLsizeiptr)(8 * sizeof(float));
    GLsizeiptr cap_bytes = (GLsizeiptr)_gles3_renderer_internal__g.image_vert_cap   * (GLsizeiptr)(8 * sizeof(float));
    // Orphan-then-subdata: same correctness pattern as the rect /
    // text passes. Without it, two flushes in the same frame race
    // and produce flicker as the second SubData overwrites the
    // memory the first Draw is still consuming.
    glBufferData(GL_ARRAY_BUFFER, cap_bytes, NULL_POINTER, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, _gles3_renderer_internal__g.image_cpu_verts);

    for (int64 i = 0; i < _gles3_renderer_internal__g.image_run_count; i++)
    {
        _gles3_renderer_internal__text_run_t* run = &_gles3_renderer_internal__g.image_runs[i];
        glBindTexture(GL_TEXTURE_2D, run->atlas);
        glDrawArrays(GL_TRIANGLES, (GLint)run->vert_start, (GLsizei)run->vert_count);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    _gles3_renderer_internal__g.image_vert_count = 0;
    _gles3_renderer_internal__g.image_run_count  = 0;
}

// ===== frame capture (see renderer.h) ========================================
// Same shape as opengl3: glReadPixels of the whole back buffer at the end
// of renderer__end_frame, rows flipped from GL's bottom-up order to
// top-down while copying out. GLES 3.0 guarantees GL_RGBA /
// GL_UNSIGNED_BYTE as a readable format.
static ubyte* _gles3_renderer_internal__capture_out  = NULL_POINTER;
static int64  _gles3_renderer_internal__capture_cap  = 0;
static boole  _gles3_renderer_internal__capture_done = FALSE;

static void _gles3_renderer_internal__capture_if_requested(void)
{
    int64  w   = (int64)_gles3_renderer_internal__g.viewport_w;
    int64  h   = (int64)_gles3_renderer_internal__g.viewport_h;
    ubyte* tmp = NULL_POINTER;
    int64  y   = 0;

    if (_gles3_renderer_internal__capture_out == NULL_POINTER || w <= 0 || h <= 0 || _gles3_renderer_internal__capture_cap < w * h * 4)
    {
        return;
    }

    tmp = (ubyte*)GUI_MALLOC_T((size_t)(w * h * 4), MM_TYPE__RENDERER);

    if (tmp == NULL_POINTER)
    {
        _gles3_renderer_internal__capture_out = NULL_POINTER;
        return;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);

    for (y = 0; y < h; y++)
    {
        stdlib__memcpy(_gles3_renderer_internal__capture_out + (size_t)y * (size_t)w * 4, tmp + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
    }

    GUI_FREE(tmp);
    _gles3_renderer_internal__capture_out  = NULL_POINTER;
    _gles3_renderer_internal__capture_done = TRUE;
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
    int64 w = (int64)_gles3_renderer_internal__g.viewport_w;
    int64 h = (int64)_gles3_renderer_internal__g.viewport_h;

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
 * @brief arms a copy of the next finished frame into out_rgba (top-down RGBA8, width * height * 4 bytes), taken at the end of the next renderer__end_frame before the platform presents it
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

    _gles3_renderer_internal__capture_out  = out_rgba;
    _gles3_renderer_internal__capture_cap  = cap;
    _gles3_renderer_internal__capture_done = FALSE;

    return TRUE;
}

/**
 * @brief tells whether the armed capture has been copied out (it is, once renderer__end_frame ran after the request)
 *
 * @return boole -> TRUE after the copy
 */
boole renderer__capture_done(void)
{
    return _gles3_renderer_internal__capture_done;
}

// Ordinary standalone scenes can use this renderer directly.
GUI_API boole renderer_backend__needs_compositor(void) { return _gles3_renderer_internal__adopted; }

// Layer mode is not implemented for this backend yet; the whole-frame
// compositor draws its extended scenes (renderer_backend.h).
GUI_API boole renderer_backend__supports_layers(void)
{
    return FALSE;
}

GUI_API void* renderer_backend__create_layer_texture(const ubyte* premultiplied_rgba, int width, int height)
{
    (void)premultiplied_rgba;
    (void)width;
    (void)height;
    return NULL_POINTER;
}

GUI_API void renderer_backend__submit_layer(void* texture, const float* corners, float opacity, int blend_mode)
{
    (void)texture;
    (void)corners;
    (void)opacity;
    (void)blend_mode;
}

GUI_API boole renderer_backend__update_layer_texture(void* texture, const ubyte* premultiplied_rgba, int width, int height)
{
    (void)texture;
    (void)premultiplied_rgba;
    (void)width;
    (void)height;
    return FALSE;
}

GUI_API boole renderer_backend__read_region(int x, int y, int w, int h, ubyte* out_rgba)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)out_rgba;
    return FALSE;
}

GUI_API boole renderer_backend__begin_layer(int width, int height)
{
    (void)width;
    (void)height;
    return FALSE;
}

GUI_API void* renderer_backend__end_layer(void)
{
    return NULL_POINTER;
}

GUI_API void renderer_backend__submit_tiles(gui_rect_t box, float radius, gui_rect_t tile, int columns, int rows, const gui_gradient_t* gradient, gui_color_t color, float scale, float opacity)
{
    (void)box;
    (void)radius;
    (void)tile;
    (void)columns;
    (void)rows;
    (void)gradient;
    (void)color;
    (void)scale;
    (void)opacity;
}

GUI_API boole renderer_backend__blur_layer(int x, int y, int w, int h, int radius) { (void)x; (void)y; (void)w; (void)h; (void)radius; return FALSE; }
