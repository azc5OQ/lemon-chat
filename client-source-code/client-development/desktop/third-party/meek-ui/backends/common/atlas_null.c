// atlas_null.c - no-op texture atlas for renderers without a GL context.
// The real atlas (atlas_gles3.c) packs many small images into a few
// large GL textures so consecutive image draws batch into one call.
// It talks to GL directly, so it can only be linked with the gles3
// renderer. Every other renderer links this file instead:
// atlas__alloc always reports "no space", and widget_image falls back
// to one renderer__create_texture_rgba texture per image, which every
// backend implements. Same pixels on screen, one draw call per image.

#include "types.h"
#include "gui.h"
#include "atlas.h"

/**
 * @brief no-op; there are no atlas pages to create
 *
 * @return void
 */
void atlas__init(void)
{
}

/**
 * @brief no-op; nothing was allocated
 *
 * @return void
 */
void atlas__shutdown(void)
{
}

/**
 * @brief always fails so the caller takes its per-image texture fallback path
 *
 * @param int w -> requested width in pixels; ignored
 * @param int h -> requested height in pixels; ignored
 * @param int* out_page -> receives -1
 * @param gui_rect_t* out_rect_px -> receives a zero rect
 *
 * @return boole -> FALSE
 */
boole atlas__alloc(int w, int h, int* out_page, gui_rect_t* out_rect_px)
{
    (void)w;
    (void)h;
    if (out_page != NULL_POINTER)
    {
        *out_page = -1;
    }
    if (out_rect_px != NULL_POINTER)
    {
        out_rect_px->x = 0.0f;
        out_rect_px->y = 0.0f;
        out_rect_px->w = 0.0f;
        out_rect_px->h = 0.0f;
    }
    return FALSE;
}

/**
 * @brief no-op; atlas__alloc never hands out a region, so there is nothing to upload into
 *
 * @param int page -> ignored
 * @param gui_rect_t rect_px -> ignored
 * @param const unsigned char* pixels -> ignored
 *
 * @return void
 */
void atlas__upload(int page, gui_rect_t rect_px, const unsigned char* pixels)
{
    (void)page;
    (void)rect_px;
    (void)pixels;
}

/**
 * @brief there are no pages
 *
 * @param int page -> ignored
 *
 * @return void* -> NULL_POINTER
 */
void* atlas__get_texture(int page)
{
    (void)page;
    return NULL_POINTER;
}

/**
 * @brief writes the full [0,1] range; only reached if a caller ignores atlas__alloc's FALSE
 *
 * @param gui_rect_t rect_px -> ignored
 * @param float* u0 -> receives 0
 * @param float* v0 -> receives 0
 * @param float* u1 -> receives 1
 * @param float* v1 -> receives 1
 *
 * @return void
 */
void atlas__rect_to_uv(gui_rect_t rect_px, float* u0, float* v0, float* u1, float* v1)
{
    (void)rect_px;
    if (u0 != NULL_POINTER)
    {
        *u0 = 0.0f;
    }
    if (v0 != NULL_POINTER)
    {
        *v0 = 0.0f;
    }
    if (u1 != NULL_POINTER)
    {
        *u1 = 1.0f;
    }
    if (v1 != NULL_POINTER)
    {
        *v1 = 1.0f;
    }
}
