// Desktop OpenGL 2 transport for the portable painter. The CPU paints all
// primitives, text and effects; fixed-function OpenGL composites the finished
// straight-alpha texture over the host. No shader or OpenGL 3 loader is used.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#if !defined(_WIN32)
#include <GL/glx.h>
#include <EGL/egl.h>
#endif
#include <stdio.h>
#include <string.h>
#define MEEK_RENDERER_BACKEND
#include "renderer.h"
#include "renderer_backend.h"
#include "opengl2_renderer.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"

typedef struct gl2_texture_t
{
    GLuint name;
} gl2_texture_t;
typedef struct gl2_state_t
{
    GLint program, active, client_active, array, element, pack, unpack;
    GLint matrix_mode, sampler, draw_fbo, read_fbo;
    GLfloat projection[16], model[16], texture[16];
} gl2_state_t;
static gl2_state_t saved;
static PFNGLACTIVETEXTUREPROC active_texture;
static PFNGLCLIENTACTIVETEXTUREARBPROC client_active_texture;
static PFNGLUSEPROGRAMPROC use_program;
static PFNGLBINDBUFFERPROC bind_buffer;
static PFNGLBLENDFUNCSEPARATEPROC blend_func;
static PFNGLBLENDEQUATIONSEPARATEPROC blend_equation;
static PFNGLBINDFRAMEBUFFERPROC bind_framebuffer;
static PFNGLBINDSAMPLERPROC bind_sampler;
static meek_opengl2_get_proc_fn resolver;
static boole ready, adopted, state_saved, has_pbo, has_srgb, has_split_fbo, has_discard;
static boole has_vertex_program, has_fragment_program, has_rectangle;
static int frame_w, frame_h;
static ubyte* capture_out;
static int64 capture_cap;
static boole captured;
static ubyte _opengl2_renderer_internal__display_gamma[3][256];
static boole _opengl2_renderer_internal__correct_gamma = FALSE;
#if defined(_WIN32)
static HWND own_window;
static HDC own_dc;
static HGLRC own_context;
#endif

static void* proc(const char* name)
{
    if (resolver)
    {
        return resolver(name);
    }
#if defined(_WIN32)
    void* p = (void*)wglGetProcAddress(name);
    if (p && p != (void*)1 && p != (void*)2 && p != (void*)3 && p != (void*)-1)
    {
        return p;
    }
    return (void*)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
#else
    return (void*)glXGetProcAddressARB((const GLubyte*)name);
#endif
}

static boole extension(const char* list, const char* name)
{
    size_t size = strlen(name);
    const char* p = list;
    if (!p)
    {
        return FALSE;
    }
    while ((p = strstr(p, name)) != NULL)
    {
        if ((p == list || p[-1] == ' ') && (p[size] == ' ' || p[size] == 0))
        {
            return TRUE;
        }
        p += size;
    }
    return FALSE;
}

boole renderer__init_adopt_opengl2(meek_opengl2_get_proc_fn loader)
{
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* extensions;
    int major = 0, minor = 0;
    GLint profile = 0;
    if (ready)
    {
        return FALSE;
    }
    if (!version || sscanf(version, "%d.%d", &major, &minor) != 2 || major < 2)
    {
        log_error("OpenGL 2: a current desktop OpenGL 2.0+ compatibility context is required");
        return FALSE;
    }
    if (major > 3 || (major == 3 && minor >= 2))
    {
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
        if (!(profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT))
        {
            return FALSE;
        }
    }
    extensions = (const char*)glGetString(GL_EXTENSIONS);
    has_vertex_program = extension(extensions, "GL_ARB_vertex_program");
    has_fragment_program = extension(extensions, "GL_ARB_fragment_program");
    has_rectangle = (boole)(major > 3 || (major == 3 && minor >= 1) || extension(extensions, "GL_ARB_texture_rectangle") || extension(extensions, "GL_EXT_texture_rectangle") || extension(extensions, "GL_NV_texture_rectangle"));
    if (major == 3 && minor == 1 && !extension(extensions, "GL_ARB_compatibility"))
    {
        return FALSE;
    }
    resolver = loader;
    active_texture = (PFNGLACTIVETEXTUREPROC)proc("glActiveTexture");
    client_active_texture = (PFNGLCLIENTACTIVETEXTUREARBPROC)proc("glClientActiveTexture");
    use_program = (PFNGLUSEPROGRAMPROC)proc("glUseProgram");
    bind_buffer = (PFNGLBINDBUFFERPROC)proc("glBindBuffer");
    blend_func = (PFNGLBLENDFUNCSEPARATEPROC)proc("glBlendFuncSeparate");
    blend_equation = (PFNGLBLENDEQUATIONSEPARATEPROC)proc("glBlendEquationSeparate");
    if (!active_texture || !client_active_texture || !use_program || !bind_buffer || !blend_func || !blend_equation)
    {
        return FALSE;
    }
    has_pbo = (boole)(major > 2 || minor >= 1 || extension(extensions, "GL_ARB_pixel_buffer_object"));
    has_srgb = (boole)(major >= 3 || extension(extensions, "GL_EXT_framebuffer_sRGB") || extension(extensions, "GL_ARB_framebuffer_sRGB"));
    has_split_fbo = (boole)(major >= 3 || extension(extensions, "GL_ARB_framebuffer_object"));
    has_discard = (boole)(major >= 3 || extension(extensions, "GL_EXT_transform_feedback"));
    bind_framebuffer = has_split_fbo ? (PFNGLBINDFRAMEBUFFERPROC)proc("glBindFramebuffer") : extension(extensions, "GL_EXT_framebuffer_object") ? (PFNGLBINDFRAMEBUFFERPROC)proc("glBindFramebufferEXT")
                                                                                                                                                : NULL;
    bind_sampler = (major > 3 || (major == 3 && minor >= 3) || extension(extensions, "GL_ARB_sampler_objects")) ? (PFNGLBINDSAMPLERPROC)proc("glBindSampler") : NULL;
    if (has_split_fbo && !bind_framebuffer)
    {
        return FALSE;
    }
    ready = adopted = TRUE;
    log_info("OpenGL 2 fixed-function transport: %s", version);
    return TRUE;
}

#if !defined(_WIN32)
static void* egl_proc(const char* name)
{
    return (void*)eglGetProcAddress(name);
}
#endif

boole renderer__init(void* native_window)
{
#if defined(_WIN32)
    PIXELFORMATDESCRIPTOR format = {0};
    if (ready || !native_window)
    {
        return FALSE;
    }
    own_window = (HWND)native_window;
    own_dc = GetDC(own_window);
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cAlphaBits = 8;
    int chosen = ChoosePixelFormat(own_dc, &format);
    if (!chosen || !SetPixelFormat(own_dc, chosen, &format))
    {
        goto fail;
    }
    own_context = wglCreateContext(own_dc);
    if (!own_context || !wglMakeCurrent(own_dc, own_context) || !renderer__init_adopt_opengl2(NULL))
    {
        goto fail;
    }
    adopted = FALSE;
    return TRUE;
fail:
    renderer_backend__shutdown();
    return FALSE;
#else
    (void)native_window;
    // The Linux X11 platform owns its desktop EGL context and presentation.
    if (!renderer__init_adopt_opengl2(egl_proc))
    {
        return FALSE;
    }
    adopted = FALSE;
    return TRUE;
#endif
}

boole renderer__opengl2_save_state(void)
{
    GLint depth, limit;
    if (!ready || state_saved)
    {
        return FALSE;
    }
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &depth);
    glGetIntegerv(GL_MAX_ATTRIB_STACK_DEPTH, &limit);
    if (depth >= limit)
    {
        return FALSE;
    }
    glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &depth);
    glGetIntegerv(GL_MAX_CLIENT_ATTRIB_STACK_DEPTH, &limit);
    if (depth >= limit)
    {
        return FALSE;
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved.active);
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &saved.client_active);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved.array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &saved.element);
    glGetIntegerv(GL_MATRIX_MODE, &saved.matrix_mode);
    glGetFloatv(GL_PROJECTION_MATRIX, saved.projection);
    glGetFloatv(GL_MODELVIEW_MATRIX, saved.model);
    if (has_pbo)
    {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved.pack);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &saved.unpack);
    }
    if (bind_framebuffer)
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved.draw_fbo);
        if (has_split_fbo)
        {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved.read_fbo);
        }
    }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    active_texture(GL_TEXTURE0);
    glGetFloatv(GL_TEXTURE_MATRIX, saved.texture);
    if (bind_sampler)
    {
        glGetIntegerv(GL_SAMPLER_BINDING, &saved.sampler);
    }
    state_saved = TRUE;
    return TRUE;
}

void renderer__opengl2_restore_state(void)
{
    if (!state_saved)
    {
        return;
    }
    active_texture(GL_TEXTURE0);
    glMatrixMode(GL_TEXTURE);
    glLoadMatrixf(saved.texture);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(saved.projection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(saved.model);
    if (bind_sampler)
    {
        bind_sampler(0, (GLuint)saved.sampler);
    }
    if (bind_framebuffer)
    {
        bind_framebuffer(has_split_fbo ? GL_DRAW_FRAMEBUFFER : GL_FRAMEBUFFER, (GLuint)saved.draw_fbo);
        if (has_split_fbo)
        {
            bind_framebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved.read_fbo);
        }
    }
    glPopClientAttrib();
    glPopAttrib();
    use_program((GLuint)saved.program);
    bind_buffer(GL_ARRAY_BUFFER, (GLuint)saved.array);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)saved.element);
    if (has_pbo)
    {
        bind_buffer(GL_PIXEL_PACK_BUFFER, (GLuint)saved.pack);
        bind_buffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)saved.unpack);
    }
    active_texture((GLenum)saved.active);
    client_active_texture((GLenum)saved.client_active);
    glMatrixMode((GLenum)saved.matrix_mode);
    state_saved = FALSE;
}

static void prepare_pixels(void)
{
    if (has_pbo)
    {
        bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
        bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    const GLenum stores[] = {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS,
                             GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS,
                             GL_PACK_SWAP_BYTES, GL_UNPACK_SWAP_BYTES, GL_PACK_LSB_FIRST, GL_UNPACK_LSB_FIRST};
    for (unsigned i = 0; i < sizeof(stores) / sizeof(stores[0]); i++)
    {
        glPixelStorei(stores[i], 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
    const GLenum scales[] = {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE};
    const GLenum biases[] = {GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS};
    for (int i = 0; i < 4; i++)
    {
        glPixelTransferf(scales[i], 1);
        glPixelTransferf(biases[i], 0);
    }
}

void renderer__begin_frame(int64 width, int64 height, gui_color_t clear)
{
    GLint units = 1, clips = 6;
    if (!ready || width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        frame_w = frame_h = 0;
        return;
    }
    frame_w = (int)width;
    frame_h = (int)height;
    use_program(0);
    if (bind_framebuffer)
    {
        bind_framebuffer(GL_FRAMEBUFFER, 0);
    }
    if (bind_sampler)
    {
        bind_sampler(0, 0);
    }
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    for (int i = 0; i < units; i++)
    {
        active_texture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_1D);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_3D);
        glDisable(GL_TEXTURE_CUBE_MAP);
        if (has_rectangle)
        {
            glDisable(GL_TEXTURE_RECTANGLE);
        }
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
        glDisable(GL_TEXTURE_GEN_R);
        glDisable(GL_TEXTURE_GEN_Q);
    }
    active_texture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    const GLenum disabled[] = {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_LIGHTING,
                               GL_COLOR_MATERIAL, GL_COLOR_SUM, GL_FOG, GL_ALPHA_TEST, GL_COLOR_LOGIC_OP, GL_SCISSOR_TEST,
                               GL_POLYGON_STIPPLE, GL_POLYGON_SMOOTH, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_ALPHA_TO_ONE, GL_SAMPLE_COVERAGE};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(disabled[0]); i++)
    {
        glDisable(disabled[i]);
    }
    if (has_srgb)
    {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }
    if (has_discard)
    {
        glDisable(GL_RASTERIZER_DISCARD);
    }
    if (has_vertex_program)
    {
        glDisable(GL_VERTEX_PROGRAM_ARB);
    }
    if (has_fragment_program)
    {
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
    }
    glGetIntegerv(GL_MAX_CLIP_PLANES, &clips);
    for (int i = 0; i < clips; i++)
    {
        glDisable(GL_CLIP_PLANE0 + i);
    }
    glEnable(GL_BLEND);
    blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    blend_equation(GL_FUNC_ADD, GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_SMOOTH);
    glViewport(0, 0, frame_w, frame_h);
    prepare_pixels();
    GLboolean double_buffer;
    glGetBooleanv(GL_DOUBLEBUFFER, &double_buffer);
    glDrawBuffer(double_buffer ? GL_BACK : GL_FRONT);
    glReadBuffer(double_buffer ? GL_BACK : GL_FRONT);
    if (!adopted)
    {
        glClearColor(clear.r, clear.g, clear.b, clear.a);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void renderer__end_frame(void)
{
    if (!ready || !frame_w || !frame_h)
    {
        return;
    }
    if (capture_out && capture_cap >= (int64)frame_w * frame_h * 4)
    {
        // One readback avoids hundreds of driver calls. Flip rows in place
        // without allocating another full-size capture buffer.
        glReadPixels(0, 0, frame_w, frame_h, GL_RGBA, GL_UNSIGNED_BYTE, capture_out);
        for (int y = 0; y < frame_h / 2; y++)
        {
            ubyte* first = capture_out + (size_t)y * frame_w * 4;
            ubyte* last = capture_out + (size_t)(frame_h - 1 - y) * frame_w * 4;
            for (int x = 0; x < frame_w * 4; x++)
            {
                ubyte value = first[x];
                first[x] = last[x];
                last[x] = value;
            }
        }
        captured = TRUE;
    }
    capture_out = NULL;
#if defined(_WIN32)
    if (!adopted)
    {
        SwapBuffers(own_dc);
    }
#endif
}

void renderer__shutdown(void)
{
    ready = adopted = state_saved = FALSE;
    _opengl2_renderer_internal__correct_gamma = FALSE;
    frame_w = frame_h = 0;
    capture_out = NULL;
    capture_cap = 0;
    captured = FALSE;
#if defined(_WIN32)
    if (own_context)
    {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(own_context);
        own_context = NULL;
    }
    if (own_dc)
    {
        ReleaseDC(own_window, own_dc);
        own_dc = NULL;
    }
    own_window = NULL;
#endif
}

static void* upload(const ubyte* pixels, int width, int height, GLenum format)
{
    if (!ready || !pixels || width <= 0 || height <= 0)
    {
        return NULL;
    }
    GLint max_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (width > max_size || height > max_size)
    {
        return NULL;
    }
    gl2_texture_t* texture = GUI_CALLOC_T(1, sizeof(*texture), MM_TYPE__RENDERER);
    if (!texture)
    {
        return NULL;
    }
    glGenTextures(1, &texture->name);
    if (!texture->name)
    {
        GUI_FREE(texture);
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    prepare_pixels();
    glTexImage2D(GL_TEXTURE_2D, 0, format == GL_RGBA ? GL_RGBA8 : GL_ALPHA8, width, height, 0, format, GL_UNSIGNED_BYTE, pixels);
    return texture;
}

void* renderer__create_texture_rgba(const ubyte* pixels, int width, int height)
{
    return upload(pixels, width, height, GL_RGBA);
}
boole renderer__opengl2_set_display_gamma(const ushort* ramp)
{
    ubyte inverse[3][256] = { 0 };
    int channel = 0;
    int value = 0;
    int sample = 0;
    int desired = 0;
    int previous_error = 0;
    int current_error = 0;
    boole changed = FALSE;

    if (ramp == NULL_POINTER)
    {
        _opengl2_renderer_internal__correct_gamma = FALSE;
        return TRUE;
    }
    for (channel = 0; channel < 3; channel++)
    {
        for (sample = 1; sample < 256; sample++)
        {
            if (ramp[channel * 256 + sample] < ramp[channel * 256 + sample - 1])
            {
                return FALSE;
            }
        }
        sample = 0;
        for (value = 0; value < 256; value++)
        {
            desired = value * 257;
            while (sample < 255 && ramp[channel * 256 + sample] < desired)
            {
                sample++;
            }
            current_error = (int)ramp[channel * 256 + sample] - desired;
            previous_error = sample > 0 ? desired - (int)ramp[channel * 256 + sample - 1] : 65536;
            inverse[channel][value] = (ubyte)((sample > 0 && previous_error < current_error) ? sample - 1 : sample);
            if (inverse[channel][value] != value)
            {
                changed = TRUE;
            }
        }
    }
    memcpy(_opengl2_renderer_internal__display_gamma, inverse, sizeof(inverse));
    _opengl2_renderer_internal__correct_gamma = changed;
    return TRUE;
}

void* renderer_backend__create_frame_texture_rgba(const ubyte* pixels, int width, int height)
{
    ubyte* corrected = NULL_POINTER;
    void* texture = NULL_POINTER;
    size_t size = 0;
    size_t offset = 0;

    if (_opengl2_renderer_internal__correct_gamma == FALSE)
    {
        return upload(pixels, width, height, GL_RGBA);
    }
    if (pixels == NULL_POINTER || width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        return NULL_POINTER;
    }
    size = (size_t)width * height * 4;
    corrected = GUI_MALLOC_T(size, MM_TYPE__RENDERER);
    if (corrected == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    for (offset = 0; offset < size; offset += 4)
    {
        corrected[offset] = _opengl2_renderer_internal__display_gamma[0][pixels[offset]];
        corrected[offset + 1] = _opengl2_renderer_internal__display_gamma[1][pixels[offset + 1]];
        corrected[offset + 2] = _opengl2_renderer_internal__display_gamma[2][pixels[offset + 2]];
        corrected[offset + 3] = pixels[offset + 3];
    }
    texture = upload(corrected, width, height, GL_RGBA);
    GUI_FREE(corrected);
    return texture;
}
void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    return upload(pixels, width, height, GL_ALPHA);
}
void renderer__destroy_texture(void* handle)
{
    gl2_texture_t* texture = handle;
    if (!texture)
    {
        return;
    }
    glDeleteTextures(1, &texture->name);
    GUI_FREE(texture);
}
void renderer__destroy_atlas(void* texture)
{
    renderer__destroy_texture(texture);
}
void renderer__submit_image_uv(gui_rect_t r, void* handle, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    gl2_texture_t* texture = handle;
    if (!ready || !texture || !frame_w || !frame_h)
    {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glColor4f(tint.r, tint.g, tint.b, tint.a);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0);
    glVertex2f(r.x, r.y);
    glTexCoord2f(u1, v0);
    glVertex2f(r.x + r.w, r.y);
    glTexCoord2f(u1, v1);
    glVertex2f(r.x + r.w, r.y + r.h);
    glTexCoord2f(u0, v1);
    glVertex2f(r.x, r.y + r.h);
    glEnd();
}
void renderer__submit_image(gui_rect_t r, void* handle, gui_color_t tint)
{
    renderer__submit_image_uv(r, handle, tint, 0, 0, 1, 1);
}
boole renderer_backend__presents_with_vsync(void)
{
    return FALSE;
}
boole renderer_backend__needs_compositor(void)
{
    return TRUE;
}

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
void renderer__flush_pending_draws(void)
{
}

// These private entry points are bypassed by the mandatory portable painter.
// Report a failed compositor allocation instead of pretending partial drawing
// implements the public contract. Public renderer__submit_* works through it.
static void painter_unavailable(void)
{
    log_error("OpenGL 2: portable painter unavailable; frame cannot be painted");
}
void renderer__submit_rect(gui_rect_t r, gui_color_t c, float radius)
{
    painter_unavailable();
}
void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color)
{
    painter_unavailable();
}
void renderer__set_text_atlas(void* atlas)
{
}
void renderer__submit_text_glyph(gui_rect_t r, gui_rect_t uv, gui_color_t c)
{
    painter_unavailable();
}
void renderer__submit_shadow(gui_rect_t r, gui_color_t c, float radius, float blur)
{
    painter_unavailable();
}
void renderer__submit_rect_gradient(gui_rect_t r, gui_color_t a, gui_color_t b, int direction, float radius)
{
    painter_unavailable();
}
void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    painter_unavailable();
}
void renderer__blur_region(gui_rect_t r, float sigma)
{
    painter_unavailable();
}
void renderer__push_scissor(gui_rect_t r)
{
}
void renderer__pop_scissor(void)
{
}
boole renderer__capture_size(int64* width, int64* height)
{
    if (!ready || frame_w <= 0 || frame_h <= 0 || !width || !height)
    {
        return FALSE;
    }
    *width = frame_w;
    *height = frame_h;
    return TRUE;
}
boole renderer__request_capture(ubyte* pixels, int64 capacity)
{
    if (!ready || !pixels || frame_w <= 0 || frame_h <= 0 || capacity < (int64)frame_w * frame_h * 4)
    {
        return FALSE;
    }
    capture_out = pixels;
    capture_cap = capacity;
    captured = FALSE;
    return TRUE;
}
boole renderer__capture_done(void)
{
    return captured;
}

GUI_API boole renderer_backend__blur_layer(int x, int y, int w, int h, int radius) { (void)x; (void)y; (void)w; (void)h; (void)radius; return FALSE; }
