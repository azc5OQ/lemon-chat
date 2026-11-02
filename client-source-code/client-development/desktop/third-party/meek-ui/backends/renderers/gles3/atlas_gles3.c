// ============================================================================
// atlas.c - GPU texture-atlas allocator (shelf packer + multi-page).
// ============================================================================
// See atlas.h for the API + motivation. This file is the
// implementation: keeps per-page state, runs the shelf packer,
// owns the GL texture lifecycle.
// Why shelf packing and not a full 2D bin packer (skyline / max-
// rects)?  Icons are roughly uniform in size (160-256 px square).
// Shelf packing on uniform inputs degrades to "perfect grid" so
// the better algorithms wouldn't recover any real space. Shelf
// is also tiny (~30 lines of logic) which means fewer corner
// cases to get wrong on first ship.

#if defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION 1
  #include <OpenGL/gl3.h>
#else
  #include <GLES3/gl3.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "atlas.h"
#include "third_party/log.h"
#include "clib/stdlib.h"
#include "clib/memory_manager.h"
#include "memory_types.h"

// Padding around each allocated region. Bilinear filtering at the
// region's edge samples a 2x2 footprint -- without padding, the
// neighbor pixel slot is sampled and bleeds the wrong color in.
// 2px gives enough margin for the four-tap footprint of GL_LINEAR
// at any magnification we'd reasonably use.
#define _ATLAS_INTERNAL__PAD 2

// Per-page shelf state. A shelf is a horizontal strip of a fixed
// height. Pages can have up to _ATLAS_INTERNAL__MAX_SHELVES shelves
// before that page is considered full; in practice we hit page-
// height-full before shelf-count-full so the constant is generous.
#define _ATLAS_INTERNAL__MAX_SHELVES 32

typedef struct _atlas_internal__shelf_t
{
    // top edge of this shelf, in page-pixel coords.
    int y;        // 0x0
    // height of this shelf.
    int h;        // 0x4
    // current right-edge cursor; next alloc lands here.
    int x_used;   // 0x8
} _atlas_internal__shelf_t;

typedef struct _atlas_internal__page_t
{
    // GL texture handle, 0 = unallocated.
    GLuint                    tex;                                     // 0x0
    // top of next-to-open shelf.
    int                       y_next;                                  // 0x4
    int                       shelf_count;                             // 0x8
    _atlas_internal__shelf_t    shelves[_ATLAS_INTERNAL__MAX_SHELVES];   // 0xC
} _atlas_internal__page_t;

static struct
{
    _atlas_internal__page_t pages[ATLAS_MAX_PAGES];
    int                   page_count;
    int                   initialized;
} _atlas_internal__g;

// Allocate a fresh atlas page: glGenTextures + glTexImage2D with
// transparent pixels. Returns the new page index, or -1 on
// failure. The transparent fill prevents reads of unallocated
// regions from showing whatever uninitialized GPU memory had.
static int _atlas_internal__open_new_page(void)
{
    if (_atlas_internal__g.page_count >= ATLAS_MAX_PAGES)
    {
        log_warn("%s %d %s", "[atlas] all", ATLAS_MAX_PAGES, "pages full; no more atlas space");
        return -1;
    }
    int idx = _atlas_internal__g.page_count;
    _atlas_internal__page_t* p = &_atlas_internal__g.pages[idx];

    glGenTextures(1, &p->tex);
    if (p->tex == 0)
    {
        log_warn("%s %d", "[atlas] glGenTextures failed for page", idx);
        return -1;
    }
    glBindTexture(GL_TEXTURE_2D, p->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Allocate storage with transparent pixels. 2048x2048 RGBA8 =
    // 16 MB on the GPU; the malloc here for the zeroed staging
    // buffer is also 16 MB but is freed immediately after the
    // upload. Acceptable one-time cost per page.
    {
        size_t bytes = (size_t)ATLAS_PAGE_SIZE * (size_t)ATLAS_PAGE_SIZE * 4;
        unsigned char* zeros = (unsigned char*)GUI_CALLOC_T(1, bytes, MM_TYPE__RENDERER);
        if (zeros == NULL_POINTER)
        {
            log_warn("%s %zu %s %d", "[atlas] failed to allocate", bytes, "-byte staging buffer for page", idx);
            glDeleteTextures(1, &p->tex);
            p->tex = 0;
            glBindTexture(GL_TEXTURE_2D, 0);
            return -1;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_PAGE_SIZE, ATLAS_PAGE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros);
        GUI_FREE(zeros);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    p->y_next      = 0;
    p->shelf_count = 0;
    _atlas_internal__g.page_count = idx + 1;
    log_info("%s %d %s %u %s %d %s %d %s", "[atlas] opened page", idx, "(tex=", (unsigned)p->tex, ",", ATLAS_PAGE_SIZE, "x", ATLAS_PAGE_SIZE, ")");
    return idx;
}

// Try to fit a (W x H) region (already padded -- caller has added
// 2*PAD to the requested dims) in page `p`. On success: writes the
// top-left of the OUTER (padded) region to *out_x / *out_y, and
// returns TRUE. Search order: existing shelves first (best fit by
// closest height match), then new shelf at y_next.
static boole _atlas_internal__try_pack(_atlas_internal__page_t* p, int W, int H, int* out_x, int* out_y)
{
    // Look for an existing shelf this region can land on. A shelf
    // accepts a region if remaining_width >= W AND shelf_height >=
    // H. We don't shrink-fit (placing a short item on a tall
    // shelf wastes the unused vertical pixels), but we also don't
    // search for the "best" shelf -- first-fit by walking in shelf-
    // order is cheap and the wasted space tends to be small with
    // uniform inputs.
    for (int i = 0; i < p->shelf_count; i++)
    {
        _atlas_internal__shelf_t* sh = &p->shelves[i];
        if (sh->h >= H && (ATLAS_PAGE_SIZE - sh->x_used) >= W)
        {
            *out_x = sh->x_used;
            *out_y = sh->y;
            sh->x_used += W;
            return TRUE;
        }
    }
    // No existing shelf fits. Try opening a new one at y_next.
    if (p->shelf_count >= _ATLAS_INTERNAL__MAX_SHELVES)
    {
        return FALSE;
    }
    if (p->y_next + H > ATLAS_PAGE_SIZE)
    {
        return FALSE;
    }
    if (W > ATLAS_PAGE_SIZE)
    {
        return FALSE;
    }

    _atlas_internal__shelf_t* sh = &p->shelves[p->shelf_count];
    sh->y      = p->y_next;
    sh->h      = H;
    sh->x_used = W;
    *out_x     = 0;
    *out_y     = p->y_next;
    p->shelf_count++;
    p->y_next += H;
    return TRUE;
}

void atlas__init(void)
{
    if (_atlas_internal__g.initialized)
    {
        return;
    }
    stdlib__memset(&_atlas_internal__g, 0, sizeof(_atlas_internal__g));
    _atlas_internal__g.initialized = 1;
    log_info("%s %d %s %d %s %d %s", "[atlas] init (page_size=", ATLAS_PAGE_SIZE, ", max_pages=", ATLAS_MAX_PAGES, ", pad=", _ATLAS_INTERNAL__PAD, ")");
}

void atlas__shutdown(void)
{
    if (_atlas_internal__g.initialized == 0)
    {
        return;
    }
    for (int i = 0; i < _atlas_internal__g.page_count; i++)
    {
        _atlas_internal__page_t* p = &_atlas_internal__g.pages[i];
        if (p->tex != 0)
        {
            glDeleteTextures(1, &p->tex);
            p->tex = 0;
        }
    }
    stdlib__memset(&_atlas_internal__g, 0, sizeof(_atlas_internal__g));
}

boole atlas__alloc(int w, int h, int* out_page, gui_rect_t* out_rect_px)
{
    if (_atlas_internal__g.initialized == 0)
    {
        log_warn("%s", "[atlas] alloc called before init");
        return FALSE;
    }
    if (w <= 0 || h <= 0)
    {
        return FALSE;
    }
    // Outer (padded) dims. Reject anything that can't physically
    // fit on a page after accounting for the border.
    int W = w + 2 * _ATLAS_INTERNAL__PAD;
    int H = h + 2 * _ATLAS_INTERNAL__PAD;
    if (W > ATLAS_PAGE_SIZE || H > ATLAS_PAGE_SIZE)
    {
        return FALSE;
    }

    // Walk existing pages first, then open a new one if all are
    // full. Stop the first time a page fits -- linear search is
    // fine; ATLAS_MAX_PAGES is small.
    for (int i = 0; i < _atlas_internal__g.page_count; i++)
    {
        int ox, oy;
        if (_atlas_internal__try_pack(&_atlas_internal__g.pages[i], W, H, &ox, &oy))
        {
            *out_page         = i;
            out_rect_px->x    = (float)(ox + _ATLAS_INTERNAL__PAD);
            out_rect_px->y    = (float)(oy + _ATLAS_INTERNAL__PAD);
            out_rect_px->w    = (float)w;
            out_rect_px->h    = (float)h;
            return TRUE;
        }
    }
    // Existing pages full. Open a new one.
    int new_page_idx = _atlas_internal__open_new_page();
    if (new_page_idx < 0)
    {
        return FALSE;
    }
    {
        int ox, oy;
        if (_atlas_internal__try_pack(&_atlas_internal__g.pages[new_page_idx], W, H, &ox, &oy))
        {
            *out_page         = new_page_idx;
            out_rect_px->x    = (float)(ox + _ATLAS_INTERNAL__PAD);
            out_rect_px->y    = (float)(oy + _ATLAS_INTERNAL__PAD);
            out_rect_px->w    = (float)w;
            out_rect_px->h    = (float)h;
            return TRUE;
        }
    }
    // Region doesn't fit on a fresh page either -- should be
    // unreachable given the W/H <= PAGE_SIZE check above.
    return FALSE;
}

void atlas__upload(int page, gui_rect_t rect_px, const unsigned char* pixels)
{
    if (_atlas_internal__g.initialized == 0)
    {
        return;
    }
    if (page < 0 || page >= _atlas_internal__g.page_count)
    {
        return;
    }
    if (pixels == NULL_POINTER)
    {
        return;
    }
    _atlas_internal__page_t* p = &_atlas_internal__g.pages[page];
    if (p->tex == 0)
    {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, p->tex);
    // Tight pack: pitch == w * 4. UNPACK_ALIGNMENT defaults to 4,
    // which would mis-step on odd widths. Set to 1 for safety; we
    // restore the default after the upload so we don't pollute
    // global GL state for other code that assumes the default.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, (int)rect_px.x, (int)rect_px.y, (int)rect_px.w, (int)rect_px.h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void* atlas__get_texture(int page)
{
    if (_atlas_internal__g.initialized == 0)
    {
        return NULL_POINTER;
    }
    if (page < 0 || page >= _atlas_internal__g.page_count)
    {
        return NULL_POINTER;
    }
    return (void*)(uintptr_t)_atlas_internal__g.pages[page].tex;
}

void atlas__rect_to_uv(gui_rect_t rect_px, float* u0, float* v0, float* u1, float* v1)
{
    float s = 1.0f / (float)ATLAS_PAGE_SIZE;
    if (u0)
    {
        *u0 = rect_px.x * s;
    }
    if (v0)
    {
        *v0 = rect_px.y * s;
    }
    if (u1)
    {
        *u1 = (rect_px.x + rect_px.w) * s;
    }
    if (v1)
    {
        *v1 = (rect_px.y + rect_px.h) * s;
    }
}
