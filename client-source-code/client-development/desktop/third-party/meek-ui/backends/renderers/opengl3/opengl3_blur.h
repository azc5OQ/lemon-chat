// GPU-only backdrop copy/filter/copy. No pixel-pack/readback or CPU staging.
#include "shaders/generated/blur.vert.h"
#include "shaders/generated/blur.frag.h"

static struct {
    GLuint program;
    GLuint vao;
    GLuint textures[2];
    GLuint framebuffers[2];
    GLint radius;
    GLint axis;
    GLint size;
    int w;
    int h;
} _opengl3_blur;

static void _opengl3_renderer_internal__release_blur(void)
{
    if (_opengl3_blur.program) glDeleteProgram(_opengl3_blur.program);
    if (_opengl3_blur.vao) glDeleteVertexArrays(1, &_opengl3_blur.vao);
    if (_opengl3_blur.framebuffers[0]) glDeleteFramebuffers(2, _opengl3_blur.framebuffers);
    if (_opengl3_blur.textures[0]) glDeleteTextures(2, _opengl3_blur.textures);
    stdlib__null_memory(&_opengl3_blur, sizeof(_opengl3_blur));
}

static boole _opengl3_renderer_internal__prepare_blur(int w, int h)
{
    if (!_opengl3_blur.program) {
        GLuint vs = _opengl3_renderer_internal__compile_shader(GL_VERTEX_SHADER, (char*)_opengl3_blur_vs);
        GLuint fs = _opengl3_renderer_internal__compile_shader(GL_FRAGMENT_SHADER, (char*)_opengl3_blur_fs);
        GLuint program = vs && fs ? _opengl3_renderer_internal__link_program(vs, fs) : 0;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        if (!program) return FALSE;
        _opengl3_blur.program = program;
        _opengl3_blur.radius = glGetUniformLocation(program, "u_radius");
        _opengl3_blur.size = glGetUniformLocation(program, "u_size");
        _opengl3_blur.axis = glGetUniformLocation(program, "u_axis");
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "u_tex"), 0);
        glGenVertexArrays(1, &_opengl3_blur.vao);
    }
    if (!_opengl3_blur.textures[0] || _opengl3_blur.w < w || _opengl3_blur.h < h) {
        if (_opengl3_blur.framebuffers[0]) glDeleteFramebuffers(2, _opengl3_blur.framebuffers);
        if (_opengl3_blur.textures[0]) glDeleteTextures(2, _opengl3_blur.textures);
        w = w > _opengl3_blur.w ? w : _opengl3_blur.w;
        h = h > _opengl3_blur.h ? h : _opengl3_blur.h;
        glGenTextures(2, _opengl3_blur.textures);
        glGenFramebuffers(2, _opengl3_blur.framebuffers);
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, _opengl3_blur.textures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL_POINTER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _opengl3_blur.framebuffers[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _opengl3_blur.textures[i], 0);
            if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { _opengl3_renderer_internal__release_blur(); return FALSE; }
        }
        _opengl3_blur.w = w;
        _opengl3_blur.h = h;
    }
    return TRUE;
}

GUI_API boole renderer_backend__blur_layer(int x, int y, int w, int h, int radius)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || radius < 1 || radius > 256 || w > _opengl3_renderer_internal__g.viewport_w - x || h > _opengl3_renderer_internal__g.viewport_h - y) return FALSE;
    GLint draw_fbo = 0;
    GLint read_fbo = 0;
    GLint samples = 0;
    GLint red_bits = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_SAMPLES, &samples);
    // Default 8-bit framebuffer only for now. Other targets retain CPU fallback.
    if (draw_fbo != 0 || samples > 0) return FALSE;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &red_bits);
    if (red_bits != 8) return FALSE;
    _opengl3_renderer_internal__flush_batches();
    GLint viewport[4];
    GLint scissor[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLboolean dither = glIsEnabled(GL_DITHER);
    GLboolean blend = glIsEnabled(GL_BLEND);
    boole ok = FALSE;
    glActiveTexture(GL_TEXTURE0);
    if (!_opengl3_renderer_internal__prepare_blur(w, h)) goto restore;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_DITHER);
    glDisable(GL_BLEND);
    int bottom = (int)_opengl3_renderer_internal__g.viewport_h - y - h;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)draw_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _opengl3_blur.framebuffers[0]);
    glBlitFramebuffer(x, bottom, x+w, bottom+h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glViewport(0, 0, w, h);
    glBindVertexArray(_opengl3_blur.vao);
    glUseProgram(_opengl3_blur.program);
    glUniform1i(_opengl3_blur.radius, radius);
    glUniform2f(_opengl3_blur.size, (float)w, (float)h);
    for (int pass = 0; pass < 6; pass++) {
        int source = pass & 1;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _opengl3_blur.framebuffers[1-source]);
        glBindTexture(GL_TEXTURE_2D, _opengl3_blur.textures[source]);
        glUniform1i(_opengl3_blur.axis, source);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _opengl3_blur.framebuffers[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
    glBlitFramebuffer(0, 0, w, h, x, bottom, x+w, bottom+h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    ok = TRUE;
restore:
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    if (scissor_enabled) glEnable(GL_SCISSOR_TEST);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    if (dither) glEnable(GL_DITHER);
    if (blend) glEnable(GL_BLEND);
    return ok;
}
