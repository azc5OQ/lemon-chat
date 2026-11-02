#ifndef GL_FUNCS_H
#define GL_FUNCS_H

// gl_funcs.h - minimal opengl 3.3 core function loader.
// declares only what the renderer uses. no dependency on glext / glew / glad.
// function pointers are defined and loaded in opengl3_renderer.c after a
// core 3.3 context is made current.
// lives alongside opengl3_renderer.c in backends/renderers/opengl3/; the embedded OpenGL adapter also uses it
// because its contents are opengl-specific -- moving it out of src/ keeps
// the d3d backends and non-renderer code independent of the GL toolchain.

#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>
#include "types.h"

// opengl types not in windows' gl 1.1 header. gl types stay at the api
// boundary; our own code converts to project types internally.
typedef char       GLchar;
typedef ptrdiff_t  GLsizeiptr;
typedef ptrdiff_t  GLintptr;

#ifndef APIENTRY
    #define APIENTRY
#endif
#ifndef APIENTRYP
    #define APIENTRYP APIENTRY*
#endif

// enums missing from gl 1.1
#define GL_VERTEX_SHADER        0x8B31
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_INFO_LOG_LENGTH      0x8B84
#define GL_ARRAY_BUFFER         0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW          0x88E0
#define GL_STATIC_DRAW          0x88E4
#define GL_DYNAMIC_DRAW         0x88E8

// texture-related enums missing from GL 1.1. windows' <GL/gl.h> stops
// at 1.1, so anything added in 1.2+ (GL_CLAMP_TO_EDGE), 3.0+ (GL_R8,
// GL_RED as internal format), or 1.3+ (texture-unit enums) has to be
// declared here.
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_RED                  0x1903
#define GL_R8                   0x8229
#define GL_TEXTURE0             0x84C0
#define GL_GENERATE_MIPMAP      0x8191
#define GL_LINEAR_MIPMAP_LINEAR 0x2703

// sRGB framebuffer capability. GL_FRAMEBUFFER_SRGB is a GL 3.0 enable
// bit: when ON and the current draw framebuffer has an sRGB-capable
// color attachment, the GPU converts shader output values from linear
// to sRGB on write. Blending happens in linear space (physically
// correct; partial-coverage edges land at the perceptual midpoint
// instead of looking dim). Requires an sRGB-capable pixel format
// from wglChoosePixelFormatARB -- the legacy ChoosePixelFormat +
// PIXELFORMATDESCRIPTOR path has no way to ask for one.
#define GL_FRAMEBUFFER_SRGB     0x8DB9

// WGL_ARB_pixel_format attributes. Used by wglChoosePixelFormatARB
// (resolved via a throwaway dummy context, since the classic
// SetPixelFormat path is one-shot per HDC and requires a pixel
// format BEFORE any context exists). WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB
// is the bit that flips the default framebuffer into sRGB mode so
// GL_FRAMEBUFFER_SRGB has something to act on.
#define WGL_DRAW_TO_WINDOW_ARB              0x2001
#define WGL_ACCELERATION_ARB                0x2003
#define WGL_SUPPORT_OPENGL_ARB              0x2010
#define WGL_DOUBLE_BUFFER_ARB               0x2011
#define WGL_PIXEL_TYPE_ARB                  0x2013
#define WGL_COLOR_BITS_ARB                  0x2014
#define WGL_ALPHA_BITS_ARB                  0x201B
#define WGL_DEPTH_BITS_ARB                  0x2022
#define WGL_STENCIL_BITS_ARB                0x2023
#define WGL_FULL_ACCELERATION_ARB           0x2027
#define WGL_TYPE_RGBA_ARB                   0x202B
#define WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB    0x20A9

// wglChoosePixelFormatARB signature. Takes an HDC, two attribute
// lists (int for integer attribs, float for float attribs -- we
// pass NULL for the float list), a capacity + output array, and
// writes the number of matching pixel-format indices returned.
#if defined(_WIN32)
typedef BOOL (WINAPI* fncp_wglChoosePixelFormatARB)(HDC hdc, const int* piAttribIList, const FLOAT* pfAttribFList, UINT nMaxFormats, int* piFormats, UINT* nNumFormats);
#endif

// function pointer typedefs. naming convention: fncp_<name>.
// (the "PFN_" prefix common in gl headers means "page frame number"
// in windows kernel context, so this project uses fncp_ to disambiguate.)
typedef GLuint (APIENTRYP fncp_glCreateShader)(GLenum type);
typedef void   (APIENTRYP fncp_glDeleteShader)(GLuint shader);
typedef void   (APIENTRYP fncp_glShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void   (APIENTRYP fncp_glCompileShader)(GLuint shader);
typedef void   (APIENTRYP fncp_glGetShaderiv)(GLuint shader, GLenum pname, GLint* params);
typedef void   (APIENTRYP fncp_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);

typedef GLuint (APIENTRYP fncp_glCreateProgram)(void);
typedef void   (APIENTRYP fncp_glDeleteProgram)(GLuint program);
typedef void   (APIENTRYP fncp_glAttachShader)(GLuint program, GLuint shader);
typedef void   (APIENTRYP fncp_glLinkProgram)(GLuint program);
typedef void   (APIENTRYP fncp_glGetProgramiv)(GLuint program, GLenum pname, GLint* params);
typedef void   (APIENTRYP fncp_glGetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void   (APIENTRYP fncp_glUseProgram)(GLuint program);

typedef GLint  (APIENTRYP fncp_glGetUniformLocation)(GLuint program, const GLchar* name);
typedef void   (APIENTRYP fncp_glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
typedef void   (APIENTRYP fncp_glUniform1i)(GLint location, GLint v0);
typedef void   (APIENTRYP fncp_glUniform1f)(GLint location, GLfloat v0);
typedef void   (APIENTRYP fncp_glGenFramebuffers)(GLsizei n, GLuint* framebuffers);
typedef void   (APIENTRYP fncp_glDeleteFramebuffers)(GLsizei n, const GLuint* framebuffers);
typedef void   (APIENTRYP fncp_glBindFramebuffer)(GLenum target, GLuint framebuffer);
typedef void   (APIENTRYP fncp_glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (APIENTRYP fncp_glCheckFramebufferStatus)(GLenum target);
typedef void   (APIENTRYP fncp_glActiveTexture)(GLenum texture);

typedef void   (APIENTRYP fncp_glGenBuffers)(GLsizei n, GLuint* buffers);
typedef void   (APIENTRYP fncp_glDeleteBuffers)(GLsizei n, const GLuint* buffers);
typedef void   (APIENTRYP fncp_glBindBuffer)(GLenum target, GLuint buffer);
typedef void   (APIENTRYP fncp_glBufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void   (APIENTRYP fncp_glBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);

typedef void   (APIENTRYP fncp_glGenVertexArrays)(GLsizei n, GLuint* arrays);
typedef void   (APIENTRYP fncp_glBindVertexArray)(GLuint array);
typedef void   (APIENTRYP fncp_glDeleteVertexArrays)(GLsizei n, const GLuint* arrays);
typedef void   (APIENTRYP fncp_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
typedef void   (APIENTRYP fncp_glEnableVertexAttribArray)(GLuint index);

// OpenGL 1.4: separate blend factors for color and alpha. Needed so we
// can do classic alpha blending on RGB while keeping the framebuffer's
// alpha pinned at 1.0. Without this, the DWM compositor on Windows
// treats partial framebuffer alpha as window transparency, producing
// flicker as our own alpha animations modulate dst.a.
typedef void   (APIENTRYP fncp_glBlendFuncSeparate)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);

// OpenGL 3.0 core. Generates the full mipmap chain for the currently
// bound 2D texture. Our image pipeline uses it so photo-sized source
// images (6000x4000) don't shimmer / twinkle when sampled down to
// ~50 px tiles -- trilinear with mipmaps costs one texture read per
// pixel, vs the O(n) brute force a pure bilinear min would need.
typedef void   (APIENTRYP fncp_glGenerateMipmap)(GLenum target);

// extern function pointers. definitions and loading live in
// opengl3_renderer.c.
extern fncp_glCreateShader            p_glCreateShader;
extern fncp_glDeleteShader            p_glDeleteShader;
extern fncp_glShaderSource            p_glShaderSource;
extern fncp_glCompileShader           p_glCompileShader;
extern fncp_glGetShaderiv             p_glGetShaderiv;
extern fncp_glGetShaderInfoLog        p_glGetShaderInfoLog;

extern fncp_glCreateProgram           p_glCreateProgram;
extern fncp_glDeleteProgram           p_glDeleteProgram;
extern fncp_glAttachShader            p_glAttachShader;
extern fncp_glLinkProgram             p_glLinkProgram;
extern fncp_glGetProgramiv            p_glGetProgramiv;
extern fncp_glGetProgramInfoLog       p_glGetProgramInfoLog;
extern fncp_glUseProgram              p_glUseProgram;

extern fncp_glGetUniformLocation      p_glGetUniformLocation;
extern fncp_glUniform2f               p_glUniform2f;
extern fncp_glUniform1i               p_glUniform1i;
extern fncp_glUniform1f               p_glUniform1f;
extern fncp_glGenFramebuffers         p_glGenFramebuffers;
extern fncp_glDeleteFramebuffers      p_glDeleteFramebuffers;
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_SAMPLES
#define GL_SAMPLES 0x80A9
#endif
typedef void (APIENTRYP fncp_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
extern fncp_glBlitFramebuffer p_glBlitFramebuffer;
typedef void (APIENTRYP fncp_glGetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint*);
extern fncp_glGetFramebufferAttachmentParameteriv p_glGetFramebufferAttachmentParameteriv;
#define glGetFramebufferAttachmentParameteriv p_glGetFramebufferAttachmentParameteriv
#ifndef GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE
#define GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE 0x8212
#endif
#define glBlitFramebuffer p_glBlitFramebuffer
extern fncp_glBindFramebuffer         p_glBindFramebuffer;
extern fncp_glFramebufferTexture2D    p_glFramebufferTexture2D;
extern fncp_glCheckFramebufferStatus  p_glCheckFramebufferStatus;
extern fncp_glActiveTexture           p_glActiveTexture;

extern fncp_glGenBuffers              p_glGenBuffers;
extern fncp_glDeleteBuffers           p_glDeleteBuffers;
extern fncp_glBindBuffer              p_glBindBuffer;
extern fncp_glBufferData              p_glBufferData;
extern fncp_glBufferSubData           p_glBufferSubData;

extern fncp_glGenVertexArrays         p_glGenVertexArrays;
extern fncp_glBindVertexArray         p_glBindVertexArray;
extern fncp_glDeleteVertexArrays      p_glDeleteVertexArrays;
extern fncp_glVertexAttribPointer     p_glVertexAttribPointer;
extern fncp_glEnableVertexAttribArray p_glEnableVertexAttribArray;

extern fncp_glBlendFuncSeparate       p_glBlendFuncSeparate;
extern fncp_glGenerateMipmap          p_glGenerateMipmap;

// shorthand macros so renderer.c can write glCompileShader(...) without
// colliding with windows' gl 1.1 symbols.
#define glCreateShader            p_glCreateShader
#define glDeleteShader            p_glDeleteShader
#define glShaderSource            p_glShaderSource
#define glCompileShader           p_glCompileShader
#define glGetShaderiv             p_glGetShaderiv
#define glGetShaderInfoLog        p_glGetShaderInfoLog
#define glCreateProgram           p_glCreateProgram
#define glDeleteProgram           p_glDeleteProgram
#define glAttachShader            p_glAttachShader
#define glLinkProgram             p_glLinkProgram
#define glGetProgramiv            p_glGetProgramiv
#define glGetProgramInfoLog       p_glGetProgramInfoLog
#define glUseProgram              p_glUseProgram
#define glGetUniformLocation      p_glGetUniformLocation
#define glUniform2f               p_glUniform2f
#define glUniform1i               p_glUniform1i
#define glUniform1f               p_glUniform1f
#define glGenFramebuffers         p_glGenFramebuffers
#define glDeleteFramebuffers      p_glDeleteFramebuffers
#define glBindFramebuffer         p_glBindFramebuffer
#define glFramebufferTexture2D    p_glFramebufferTexture2D
#define glCheckFramebufferStatus  p_glCheckFramebufferStatus
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER            0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0      0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#endif
#define glActiveTexture           p_glActiveTexture
#define glGenBuffers              p_glGenBuffers
#define glDeleteBuffers           p_glDeleteBuffers
#define glBindBuffer              p_glBindBuffer
#define glBufferData              p_glBufferData
#define glBufferSubData           p_glBufferSubData
#define glGenVertexArrays         p_glGenVertexArrays
#define glBindVertexArray         p_glBindVertexArray
#define glDeleteVertexArrays      p_glDeleteVertexArrays
#define glVertexAttribPointer     p_glVertexAttribPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glBlendFuncSeparate       p_glBlendFuncSeparate
#define glGenerateMipmap          p_glGenerateMipmap

#endif
