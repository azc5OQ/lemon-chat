#include <GLES3/gl3.h>
#include <string.h>
#include <stdio.h>
#include "embedded_gles3.h"
#include "gles3_renderer.h"
#include "renderer.h"

typedef struct state_t
{
    GLint program, vao, array, active, texture, sampler, draw_fbo, read_fbo;
    GLint viewport[4], scissor[4], blend[6], pixel_store[8], pack, unpack;
    GLboolean mask[4], enabled[8];
} state_t;
static state_t saved;
static boole attached, in_frame;
static const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE,
                              GL_SCISSOR_TEST, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE, GL_RASTERIZER_DISCARD};
static const GLenum stores[] = {GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_UNPACK_ROW_LENGTH,
                                GL_PACK_SKIP_PIXELS, GL_UNPACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS, GL_UNPACK_SKIP_ROWS};
static const GLenum blend_values[] = {GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA,
                                      GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA};

static boole available(void)
{
    GLboolean active, paused;
    glGetBooleanv(GL_TRANSFORM_FEEDBACK_ACTIVE, &active);
    glGetBooleanv(GL_TRANSFORM_FEEDBACK_PAUSED, &paused);
    return (boole)(!active || paused);
}
static void save(void)
{
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved.array);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved.active);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved.draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved.read_fbo);
    glGetIntegerv(GL_VIEWPORT, saved.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, saved.scissor);
    glGetBooleanv(GL_COLOR_WRITEMASK, saved.mask);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved.pack);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &saved.unpack);
    for (int i = 0; i < 8; i++)
    {
        glGetIntegerv(stores[i], &saved.pixel_store[i]);
        saved.enabled[i] = glIsEnabled(caps[i]);
    }
    for (int i = 0; i < 6; i++)
    {
        glGetIntegerv(blend_values[i], &saved.blend[i]);
    }
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved.texture);
    glGetIntegerv(GL_SAMPLER_BINDING, &saved.sampler);
}
static void prepare(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindSampler(0, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (int i = 0; i < 8; i++)
    {
        glPixelStorei(stores[i], i < 2 ? 1 : 0);
    }
    for (int i = 1; i < 8; i++)
    {
        glDisable(caps[i]);
    }
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}
static void restore(void)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved.draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved.read_fbo);
    glUseProgram((GLuint)saved.program);
    glBindVertexArray((GLuint)saved.vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)saved.array);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)saved.texture);
    glBindSampler(0, (GLuint)saved.sampler);
    glActiveTexture(saved.active);
    glViewport(saved.viewport[0], saved.viewport[1], saved.viewport[2], saved.viewport[3]);
    glScissor(saved.scissor[0], saved.scissor[1], saved.scissor[2], saved.scissor[3]);
    glBlendFuncSeparate(saved.blend[0], saved.blend[1], saved.blend[2], saved.blend[3]);
    glBlendEquationSeparate(saved.blend[4], saved.blend[5]);
    glColorMask(saved.mask[0], saved.mask[1], saved.mask[2], saved.mask[3]);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)saved.pack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)saved.unpack);
    for (int i = 0; i < 8; i++)
    {
        glPixelStorei(stores[i], saved.pixel_store[i]);
        if (saved.enabled[i])
        {
            glEnable(caps[i]);
        }
        else
        {
            glDisable(caps[i]);
        }
    }
}
boole embedded_gles3__attach(void)
{
    if (attached)
    {
        return TRUE;
    }
    const char* version = (const char*)glGetString(GL_VERSION);
    int major = 0, minor = 0;
    if (!version || sscanf(version, "OpenGL ES %d.%d", &major, &minor) != 2 || major < 3 || !available())
    {
        return FALSE;
    }
    save();
    prepare();
    attached = renderer__init_adopt_gles3();
    if (!attached)
    {
        renderer__shutdown();
    }
    restore();
    return attached;
}
boole embedded_gles3__is_attached(void)
{
    return attached;
}
boole embedded_gles3__begin_frame(int64 width, int64 height)
{
    if (!attached || in_frame || width <= 0 || height <= 0 || width > 8192 || height > 8192 || !available())
    {
        return FALSE;
    }
    save();
    prepare();
    renderer__begin_frame(width, height, (gui_color_t){0, 0, 0, 0});
    in_frame = TRUE;
    return TRUE;
}
void embedded_gles3__end_frame(void)
{
    if (!in_frame)
    {
        return;
    }
    renderer__end_frame();
    restore();
    in_frame = FALSE;
}
void embedded_gles3__detach(void)
{
    if (!attached)
    {
        return;
    }
    embedded_gles3__end_frame();
    save();
    renderer__shutdown();
    restore();
    attached = FALSE;
}
