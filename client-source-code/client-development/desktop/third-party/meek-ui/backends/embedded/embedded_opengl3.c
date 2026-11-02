// backends/embedded/embedded_opengl3.c
//
// OpenGL backend for drawing meek-ui inside a frame the host owns. The
// standalone opengl3 renderer's adopt mode draws on the context that is
// current (the host's), never clears and never swaps. This file adds what
// an overlay needs on top of that:
//
// - the host's GL state is read back before the frame and put back after
//   it. The fixed-function era did this with glPushAttrib, which core
//   profiles no longer have, so every piece of state is read with glGet*
//   and restored call by call: program, vertex array, buffers, texture
//   unit 0, sampler, framebuffers, viewport, scissor, blend function and
//   equation, color mask, polygon mode, pixel store alignment and the
//   enable bits the renderer or the host may flip;
// - the state the renderer relies on is applied at the start of every
//   frame, because the standalone renderer sets it once at build time
//   and the restore at the end of the previous frame undid it: blending
//   on with the visual-contract blend function, depth / cull / stencil /
//   logic op / clip planes / rasterizer discard / sRGB off, solid fill,
//   all color channels enabled, the default framebuffer bound, multisampling
//   enabled with host coverage/mask and alpha-to-one overrides disabled.
//
// Requires desktop OpenGL 3.3+. Use embedded_opengl2 for strict OpenGL 2
// compatibility; context creation syntax does not establish its capabilities.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "gl_funcs.h"
#include "opengl3_renderer.h"
#include "embedded_opengl3.h"
#include "clib/stdlib.h"
#include "third_party/log.h"

// enums the windows gl 1.1 header lacks
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_BINDING
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_SAMPLER_BINDING
#define GL_SAMPLER_BINDING 0x8919
#endif
#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#endif
#ifndef GL_BLEND_DST_RGB
#define GL_BLEND_DST_RGB 0x80C8
#endif
#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA 0x80CB
#endif
#ifndef GL_BLEND_DST_ALPHA
#define GL_BLEND_DST_ALPHA 0x80CA
#endif
#ifndef GL_BLEND_EQUATION_RGB
#define GL_BLEND_EQUATION_RGB 0x8009
#endif
#ifndef GL_BLEND_EQUATION_ALPHA
#define GL_BLEND_EQUATION_ALPHA 0x883D
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_SAMPLE_ALPHA_TO_COVERAGE
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_SAMPLE_ALPHA_TO_ONE
#define GL_SAMPLE_ALPHA_TO_ONE 0x809F
#endif
#ifndef GL_SAMPLE_COVERAGE
#define GL_SAMPLE_COVERAGE 0x80A0
#endif
#ifndef GL_SAMPLE_MASK
#define GL_SAMPLE_MASK 0x8E51
#endif
#ifndef GL_RASTERIZER_DISCARD
#define GL_RASTERIZER_DISCARD 0x8C89
#endif
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif

#define EMBEDDED_OPENGL3_CLIP_DISTANCES 8

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif
static const GLenum _embedded_opengl3_internal__pixel_store[] = {
    GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS,
    GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS
};

typedef void (APIENTRY* _embedded_opengl3_internal__fncp_glBindFramebuffer)(GLenum target, GLuint framebuffer);
typedef void (APIENTRY* _embedded_opengl3_internal__fncp_glBlendEquationSeparate)(GLenum mode_rgb, GLenum mode_alpha);
typedef void (APIENTRY* _embedded_opengl3_internal__fncp_glBindSampler)(GLuint unit, GLuint sampler);

// everything the host had bound or enabled when embedded_opengl3__begin_frame ran
typedef struct _embedded_opengl3_internal__backup_t
{
    GLint pack_buffer;
    GLint unpack_buffer;
    GLint pixel_store[6];
    GLint program;                                                  // 0x0
    GLint vertex_array;                                             // 0x4
    GLint array_buffer;                                             // 0x8
    GLint element_array_buffer;                                     // 0xC
    GLint active_texture;                                           // 0x10
    GLint texture_2d_unit0;                                         // 0x14
    GLint sampler_unit0;                                            // 0x18
    GLint draw_framebuffer;                                         // 0x1C
    GLint read_framebuffer;                                         // 0x20
    GLint viewport[4];                                              // 0x24
    GLint scissor_box[4];                                           // 0x34
    GLint blend_src_rgb;                                            // 0x44
    GLint blend_dst_rgb;                                            // 0x48
    GLint blend_src_alpha;                                          // 0x4C
    GLint blend_dst_alpha;                                          // 0x50
    GLint blend_equation_rgb;                                       // 0x54
    GLint blend_equation_alpha;                                     // 0x58
    GLint polygon_mode[2];                                          // 0x5C
    GLint unpack_alignment;                                         // 0x64
    GLint pack_alignment;                                           // 0x68
    GLboolean color_mask[4];                                        // 0x6C
    GLboolean blend_enabled;                                        // 0x70
    GLboolean scissor_enabled;                                      // 0x71
    GLboolean depth_test_enabled;                                   // 0x72
    GLboolean cull_face_enabled;                                    // 0x73
    GLboolean stencil_test_enabled;                                 // 0x74
    GLboolean srgb_enabled;                                         // 0x75
    GLboolean logic_op_enabled;                                     // 0x76
    GLboolean alpha_to_coverage_enabled;                            // 0x77
    GLboolean rasterizer_discard_enabled;                           // 0x78
    GLboolean clip_distance_enabled[EMBEDDED_OPENGL3_CLIP_DISTANCES]; // 0x79
    GLboolean multisample_enabled;
    GLboolean alpha_to_one_enabled;
    GLboolean sample_coverage_enabled;
    GLboolean sample_mask_enabled;
} _embedded_opengl3_internal__backup_t;

static _embedded_opengl3_internal__backup_t g_embedded_opengl3_backup;
static _embedded_opengl3_internal__fncp_glBindFramebuffer g_embedded_opengl3_bind_framebuffer = NULL_POINTER;
static _embedded_opengl3_internal__fncp_glBlendEquationSeparate g_embedded_opengl3_blend_equation_separate = NULL_POINTER;
static _embedded_opengl3_internal__fncp_glBindSampler g_embedded_opengl3_bind_sampler = NULL_POINTER;
static boole g_embedded_opengl3_attached = FALSE;
static boole g_embedded_opengl3_in_frame = FALSE;

static void _embedded_opengl3_internal__backup(void);
static void _embedded_opengl3_internal__apply_frame_state(void);
static void _embedded_opengl3_internal__restore(void);
static void _embedded_opengl3_internal__set_cap(GLenum cap, GLboolean enabled);

/**
 * @brief enables or disables one capability to match a saved value
 *
 * @param GLenum cap -> the capability
 * @param GLboolean enabled -> the saved state
 *
 * @return void
 */
static void _embedded_opengl3_internal__set_cap(GLenum cap, GLboolean enabled)
{
    if (enabled == GL_TRUE)
    {
        glEnable(cap);
    }
    else
    {
        glDisable(cap);
    }
}

/**
 * @brief reads back every piece of GL state the renderer or this file changes
 *
 * @attention leaves texture unit 0 active; the renderer binds its atlases on the active unit and its shaders sample unit 0
 *
 * @return void
 */
static void _embedded_opengl3_internal__backup(void)
{
    _embedded_opengl3_internal__backup_t* b = &g_embedded_opengl3_backup;
    int64 i = 0;

    stdlib__memset(b, 0, sizeof(*b));

    glGetIntegerv(GL_CURRENT_PROGRAM, &b->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &b->vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &b->array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &b->element_array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &b->active_texture);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &b->draw_framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &b->read_framebuffer);
    glGetIntegerv(GL_VIEWPORT, b->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, b->scissor_box);
    glGetIntegerv(GL_BLEND_SRC_RGB, &b->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &b->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &b->blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &b->blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &b->blend_equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &b->blend_equation_alpha);
    glGetIntegerv(GL_POLYGON_MODE, b->polygon_mode);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &b->unpack_alignment);
    glGetIntegerv(GL_PACK_ALIGNMENT, &b->pack_alignment);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &b->pack_buffer);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &b->unpack_buffer);
    for (i = 0; i < 6; i++) glGetIntegerv(_embedded_opengl3_internal__pixel_store[i], &b->pixel_store[i]);
    glGetBooleanv(GL_COLOR_WRITEMASK, b->color_mask);
    b->blend_enabled = glIsEnabled(GL_BLEND);
    b->scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    b->depth_test_enabled = glIsEnabled(GL_DEPTH_TEST);
    b->cull_face_enabled = glIsEnabled(GL_CULL_FACE);
    b->stencil_test_enabled = glIsEnabled(GL_STENCIL_TEST);
    b->srgb_enabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    b->logic_op_enabled = glIsEnabled(GL_COLOR_LOGIC_OP);
    b->alpha_to_coverage_enabled = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    b->multisample_enabled = glIsEnabled(GL_MULTISAMPLE);
    b->alpha_to_one_enabled = glIsEnabled(GL_SAMPLE_ALPHA_TO_ONE);
    b->sample_coverage_enabled = glIsEnabled(GL_SAMPLE_COVERAGE);
    b->sample_mask_enabled = glIsEnabled(GL_SAMPLE_MASK);
    b->rasterizer_discard_enabled = glIsEnabled(GL_RASTERIZER_DISCARD);

    for (i = 0; i < EMBEDDED_OPENGL3_CLIP_DISTANCES; i++)
    {
        b->clip_distance_enabled[i] = glIsEnabled((GLenum)(GL_CLIP_DISTANCE0 + i));
    }

    // texture unit 0 is where the renderer binds; its texture and sampler are read after making it active
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &b->texture_2d_unit0);

    if (g_embedded_opengl3_bind_sampler != NULL_POINTER)
    {
        glGetIntegerv(GL_SAMPLER_BINDING, &b->sampler_unit0);
    }
}

/**
 * @brief puts the GL into the state the renderer's pipelines assume; the standalone renderer sets this once at build time, an overlay has to do it every frame
 *
 * @return void
 */
static void _embedded_opengl3_internal__apply_frame_state(void)
{
    _embedded_opengl3_internal__backup_t* b = &g_embedded_opengl3_backup;
    int64 i = 0;

    // the overlay goes to the window, never into a framebuffer object of the host
    if (g_embedded_opengl3_bind_framebuffer != NULL_POINTER && (b->draw_framebuffer != 0 || b->read_framebuffer != 0))
    {
        g_embedded_opengl3_bind_framebuffer(GL_FRAMEBUFFER, 0);
    }

    // a sampler object on unit 0 would override the atlas texture's own filtering
    if (g_embedded_opengl3_bind_sampler != NULL_POINTER && b->sampler_unit0 != 0)
    {
        g_embedded_opengl3_bind_sampler(0, 0);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    // Disable the host's coverage overrides without modifying their values,
    // inversion flag or indexed mask words. Restoring their enables is enough.
    glEnable(GL_MULTISAMPLE);
    glDisable(GL_SAMPLE_ALPHA_TO_ONE);
    glDisable(GL_SAMPLE_COVERAGE);
    glDisable(GL_SAMPLE_MASK);
    glDisable(GL_RASTERIZER_DISCARD);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (i = 0; i < 6; i++) glPixelStorei(_embedded_opengl3_internal__pixel_store[i], 0);

    for (i = 0; i < EMBEDDED_OPENGL3_CLIP_DISTANCES; i++)
    {
        glDisable((GLenum)(GL_CLIP_DISTANCE0 + i));
    }

    // the visual contract blend: straight alpha for color, destination alpha kept at one
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (g_embedded_opengl3_blend_equation_separate != NULL_POINTER)
    {
        g_embedded_opengl3_blend_equation_separate(GL_FUNC_ADD, GL_FUNC_ADD);
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

/**
 * @brief puts the saved GL state back, in the reverse order of the backup
 *
 * @return void
 */
static void _embedded_opengl3_internal__restore(void)
{
    _embedded_opengl3_internal__backup_t* b = &g_embedded_opengl3_backup;
    int64 i = 0;

    if (g_embedded_opengl3_bind_framebuffer != NULL_POINTER)
    {
        g_embedded_opengl3_bind_framebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)b->draw_framebuffer);
        g_embedded_opengl3_bind_framebuffer(GL_READ_FRAMEBUFFER, (GLuint)b->read_framebuffer);
    }

    glUseProgram((GLuint)b->program);
    glBindVertexArray((GLuint)b->vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)b->array_buffer);
    // The element binding belongs to the VAO. Rebinding the host VAO restores
    // it; changing it on VAO 0 would be invalid in a core context.

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)b->texture_2d_unit0);

    if (g_embedded_opengl3_bind_sampler != NULL_POINTER)
    {
        g_embedded_opengl3_bind_sampler(0, (GLuint)b->sampler_unit0);
    }

    glActiveTexture((GLenum)b->active_texture);

    glViewport(b->viewport[0], b->viewport[1], (GLsizei)b->viewport[2], (GLsizei)b->viewport[3]);
    glScissor(b->scissor_box[0], b->scissor_box[1], (GLsizei)b->scissor_box[2], (GLsizei)b->scissor_box[3]);
    glBlendFuncSeparate((GLenum)b->blend_src_rgb, (GLenum)b->blend_dst_rgb, (GLenum)b->blend_src_alpha, (GLenum)b->blend_dst_alpha);

    if (g_embedded_opengl3_blend_equation_separate != NULL_POINTER)
    {
        g_embedded_opengl3_blend_equation_separate((GLenum)b->blend_equation_rgb, (GLenum)b->blend_equation_alpha);
    }

    glPolygonMode(GL_FRONT_AND_BACK, (GLenum)b->polygon_mode[0]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, b->unpack_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, b->pack_alignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)b->pack_buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)b->unpack_buffer);
    for (i = 0; i < 6; i++) glPixelStorei(_embedded_opengl3_internal__pixel_store[i], b->pixel_store[i]);
    glColorMask(b->color_mask[0], b->color_mask[1], b->color_mask[2], b->color_mask[3]);

    _embedded_opengl3_internal__set_cap(GL_BLEND, b->blend_enabled);
    _embedded_opengl3_internal__set_cap(GL_SCISSOR_TEST, b->scissor_enabled);
    _embedded_opengl3_internal__set_cap(GL_DEPTH_TEST, b->depth_test_enabled);
    _embedded_opengl3_internal__set_cap(GL_CULL_FACE, b->cull_face_enabled);
    _embedded_opengl3_internal__set_cap(GL_STENCIL_TEST, b->stencil_test_enabled);
    _embedded_opengl3_internal__set_cap(GL_FRAMEBUFFER_SRGB, b->srgb_enabled);
    _embedded_opengl3_internal__set_cap(GL_COLOR_LOGIC_OP, b->logic_op_enabled);
    _embedded_opengl3_internal__set_cap(GL_SAMPLE_ALPHA_TO_COVERAGE, b->alpha_to_coverage_enabled);
    _embedded_opengl3_internal__set_cap(GL_MULTISAMPLE, b->multisample_enabled);
    _embedded_opengl3_internal__set_cap(GL_SAMPLE_ALPHA_TO_ONE, b->alpha_to_one_enabled);
    _embedded_opengl3_internal__set_cap(GL_SAMPLE_COVERAGE, b->sample_coverage_enabled);
    _embedded_opengl3_internal__set_cap(GL_SAMPLE_MASK, b->sample_mask_enabled);
    _embedded_opengl3_internal__set_cap(GL_RASTERIZER_DISCARD, b->rasterizer_discard_enabled);

    for (i = 0; i < EMBEDDED_OPENGL3_CLIP_DISTANCES; i++)
    {
        _embedded_opengl3_internal__set_cap((GLenum)(GL_CLIP_DISTANCE0 + i), b->clip_distance_enabled[i]);
    }
}

/**
 * @brief adopts the OpenGL context current on this thread and builds the renderer's pipelines on it
 *
 * @attention call from the thread that owns the host's context, with that context current (inside a SwapBuffers hook that is already the case); call memory_manager__init and widget_registry__bootstrap_builtins before, font__init after
 *
 * @return boole -> TRUE when attached
 */
boole embedded_opengl3__attach(void)
{
    return embedded_opengl3__attach_with_loader(NULL_POINTER);
}

boole embedded_opengl3__attach_with_loader(meek_opengl_get_proc_fn resolver)
{
    if (g_embedded_opengl3_attached == TRUE)
    {
        log_warn("%s", "embedded_opengl3__attach: already attached");
        return TRUE;
    }

    if (renderer__prepare_adopt_opengl(resolver) == FALSE) return FALSE;
    g_embedded_opengl3_bind_framebuffer = (_embedded_opengl3_internal__fncp_glBindFramebuffer)renderer__opengl_proc_address("glBindFramebuffer");
    g_embedded_opengl3_blend_equation_separate = (_embedded_opengl3_internal__fncp_glBlendEquationSeparate)renderer__opengl_proc_address("glBlendEquationSeparate");
    g_embedded_opengl3_bind_sampler = (_embedded_opengl3_internal__fncp_glBindSampler)renderer__opengl_proc_address("glBindSampler");
    if (!g_embedded_opengl3_bind_framebuffer || !g_embedded_opengl3_blend_equation_separate || !g_embedded_opengl3_bind_sampler) return FALSE;
    _embedded_opengl3_internal__backup();
    boole ready = renderer__init_adopt_opengl();
    _embedded_opengl3_internal__restore();
    if (ready == FALSE) return FALSE;

    g_embedded_opengl3_attached = TRUE;
    g_embedded_opengl3_in_frame = FALSE;

    return TRUE;
}

/**
 * @brief deletes the renderer's GL objects; the host's context stays as it was
 *
 * @return void
 */
void embedded_opengl3__detach(void)
{
    if (g_embedded_opengl3_attached == FALSE)
    {
        return;
    }

    if (g_embedded_opengl3_in_frame == TRUE)
    {
        embedded_opengl3__end_frame();
    }

    _embedded_opengl3_internal__backup();
    renderer__shutdown();
    _embedded_opengl3_internal__restore();

    g_embedded_opengl3_bind_framebuffer = NULL_POINTER;
    g_embedded_opengl3_blend_equation_separate = NULL_POINTER;
    g_embedded_opengl3_bind_sampler = NULL_POINTER;
    g_embedded_opengl3_attached = FALSE;
}

/**
 * @brief tells whether attach succeeded and detach has not run
 *
 * @return boole -> TRUE while attached
 */
boole embedded_opengl3__is_attached(void)
{
    return g_embedded_opengl3_attached;
}

/**
 * @brief backs up the host's GL state, applies the state the renderer needs and starts a meek-ui frame on the window without clearing it
 *
 * @param int64 width -> window width in pixels
 * @param int64 height -> window height in pixels
 *
 * @return boole -> FALSE when not attached
 */
boole embedded_opengl3__begin_frame(int64 width, int64 height)
{
    gui_color_t no_clear = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (g_embedded_opengl3_attached == FALSE || width <= 0 || height <= 0)
    {
        return FALSE;
    }

    if (g_embedded_opengl3_in_frame == TRUE)
    {
        log_warn("%s", "embedded_opengl3__begin_frame: frame already open, ending it first");
        embedded_opengl3__end_frame();
    }

    _embedded_opengl3_internal__backup();
    _embedded_opengl3_internal__apply_frame_state();
    renderer__begin_frame(width, height, no_clear);
    g_embedded_opengl3_in_frame = TRUE;

    return TRUE;
}

/**
 * @brief flushes meek-ui's pending draws and restores the host's GL state; the host swaps afterwards
 *
 * @return void
 */
void embedded_opengl3__end_frame(void)
{
    if (g_embedded_opengl3_in_frame == FALSE)
    {
        return;
    }

    renderer__end_frame();
    _embedded_opengl3_internal__restore();
    g_embedded_opengl3_in_frame = FALSE;
}
