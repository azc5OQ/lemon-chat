#include "session/tree_icons.h"
#include "attachments/attachments_internal.h"
#include "attachments/attachment_file.h"
#include "platform/memory.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <setjmp.h>

typedef struct desktop_svg_allocation_t
{
    void *pointer;
    uint64 capacity;
} desktop_svg_allocation_t;

// Each decoder thread has a bounded, short-lived allocation ledger for NanoSVG.
#ifdef _MSC_VER
#define DESKTOP_TREE_LOCAL __declspec(thread)
#else
#define DESKTOP_TREE_LOCAL _Thread_local
#endif
static DESKTOP_TREE_LOCAL desktop_svg_allocation_t g_svg_allocations[2048];
static DESKTOP_TREE_LOCAL uint64 g_svg_bytes;
static DESKTOP_TREE_LOCAL boole g_svg_failed;
static DESKTOP_TREE_LOCAL boole g_svg_active;
static DESKTOP_TREE_LOCAL jmp_buf g_svg_abort;

// declarations
static void _tree_icons_internal__abort(void);
static void *_tree_icons_internal__allocate(uint64 capacity);
static void _tree_icons_internal__release(void *pointer);
static void *_tree_icons_internal__resize(void *pointer, uint64 capacity);
static boole _tree_icons_internal__svg(char *source, ubyte *pixels);

// Private copies of the unchanged Meek decoders; no callbacks, scripts or external resources.
#define nuint meek_ui_nuint
#define nint meek_ui_nint
#define NSVG_MALLOC _tree_icons_internal__allocate
#define NSVG_REALLOC _tree_icons_internal__resize
#define NSVG_FREE _tree_icons_internal__release
#define nsvgParse desktop_tree_nsvgParse
#define nsvgDuplicatePath desktop_tree_nsvgDuplicatePath
#define nsvgDelete desktop_tree_nsvgDelete
#define nsvgCreateRasterizer desktop_tree_nsvgCreateRasterizer
#define nsvgRasterize desktop_tree_nsvgRasterize
#define nsvgDeleteRasterizer desktop_tree_nsvgDeleteRasterizer
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"
#undef nint
#undef nuint

/**
 * @brief stops the private decoder before it can use a failed allocation.
 * @return void
 */
static void _tree_icons_internal__abort(void)
{
    g_svg_failed = TRUE;
    if (g_svg_active)
    {
        longjmp(g_svg_abort, 1);
    }
}

/**
 * @brief bounds temporary SVG allocations and records their complete capacity for erasure.
 * @param uint64 capacity -> requested bytes from the vendor decoder
 * @return void* -> cleared allocation, or NULL when the 8 MiB budget is exhausted
 */
static void *_tree_icons_internal__allocate(uint64 capacity)
{
    int64 index = 0;
    void *pointer = NULL;
    if (!capacity || capacity > 8 * 1024 * 1024 - g_svg_bytes)
    {
        _tree_icons_internal__abort();
        return NULL;
    }
    for (index = 0; index < 2048; index++)
    {
        if (!g_svg_allocations[index].pointer)
        {
            pointer = calloc(1, capacity);
            if (pointer)
            {
                g_svg_allocations[index].pointer = pointer;
                g_svg_allocations[index].capacity = capacity;
                g_svg_bytes += capacity;
                return pointer;
            }
            break;
        }
    }
    _tree_icons_internal__abort();
    return NULL;
}

/**
 * @brief wipes one tracked vendor allocation before releasing it.
 * @param void* pointer -> owned pointer, or NULL
 * @return void
 */
static void _tree_icons_internal__release(void *pointer)
{
    int64 index = 0;
    if (!pointer)
    {
        return;
    }
    for (index = 0; index < 2048; index++)
    {
        if (g_svg_allocations[index].pointer == pointer)
        {
            desktop_memory__erase(pointer, g_svg_allocations[index].capacity);
            free(pointer);
            g_svg_bytes -= g_svg_allocations[index].capacity;
            clib__null_memory(&g_svg_allocations[index], sizeof(g_svg_allocations[index]));
            return;
        }
    }
}

/**
 * @brief grows a vendor buffer without leaving the old capacity unwiped.
 * @param void* pointer -> tracked old allocation, or NULL
 * @param uint64 capacity -> new capacity
 * @return void* -> replacement on success; old allocation remains owned on failure
 */
static void *_tree_icons_internal__resize(void *pointer, uint64 capacity)
{
    int64 index = 0;
    void *replacement = NULL;
    if (!pointer)
    {
        return _tree_icons_internal__allocate(capacity);
    }
    for (index = 0; index < 2048; index++)
    {
        if (g_svg_allocations[index].pointer == pointer)
        {
            replacement = _tree_icons_internal__allocate(capacity);
            if (replacement)
            {
                clib__memcpy(replacement, pointer, capacity < g_svg_allocations[index].capacity ? capacity : g_svg_allocations[index].capacity);
                _tree_icons_internal__release(pointer);
            }
            return replacement;
        }
    }
    _tree_icons_internal__abort();
    return NULL;
}

/**
 * @brief rasterizes a bounded vector icon into a fixed transparent 32-pixel square.
 * @param char* source -> mutable, terminated source no larger than 64 KiB
 * @param ubyte* pixels -> cleared 4096-byte output
 * @return boole -> TRUE when dimensions, geometry and resource budgets were accepted
 */
static boole _tree_icons_internal__svg(char *source, ubyte *pixels)
{
    NSVGimage *svg = NULL;
    NSVGrasterizer *raster = NULL;
    NSVGshape *shape = NULL;
    NSVGpath *path = NULL;
    int64 index = 0;
    int64 points = 0;
    float scale = 0;
    boole valid = FALSE;
    g_svg_failed = FALSE;
    g_svg_active = TRUE;
    if (setjmp(g_svg_abort) != 0)
    {
        valid = FALSE;
        goto label_finished;
    }
    svg = nsvgParse(source, "px", 96);
    if (!svg || !(svg->width >= 1 && svg->width <= 4096 && svg->height >= 1 && svg->height <= 4096) || g_svg_failed)
    {
        goto label_finished;
    }
    scale = 32.0f / clib__fmaxf(svg->width, svg->height);
    for (shape = svg->shapes; shape; shape = shape->next)
    {
        if (!(shape->strokeWidth >= 0 && shape->strokeWidth <= 4096))
        {
            goto label_finished;
        }
        for (path = shape->paths; path; path = path->next)
        {
            if (path->npts < 0 || path->npts > 8192 || points + path->npts > 16384)
            {
                goto label_finished;
            }
            points += path->npts;
            for (index = 0; index < (int64)path->npts * 2; index++)
            {
                if (!(path->pts[index] * scale >= -65536 && path->pts[index] * scale <= 65536))
                {
                    goto label_finished;
                }
            }
        }
    }
    raster = nsvgCreateRasterizer();
    if (raster && !g_svg_failed)
    {
        nsvgRasterize(raster, svg, (32 - svg->width * scale) * 0.5f, (32 - svg->height * scale) * 0.5f, scale, pixels, 32, 32, 128);
        valid = !g_svg_failed;
    }
label_finished:
    g_svg_active = FALSE;
    // Every decoder allocation is in this ledger, including incomplete objects after an abort.
    for (index = 0; index < 2048; index++)
    {
        _tree_icons_internal__release(g_svg_allocations[index].pointer);
    }
    if (!valid)
    {
        desktop_memory__erase(pixels, 4096);
    }
    return valid;
}

/**
 * @brief decodes only inline server icons, with byte, geometry and allocation limits.
 * @param const char* data -> borrowed data URI from the authenticated server
 * @param ubyte* pixels -> caller-owned 32x32 RGBA output
 * @param uint64 capacity -> actual output capacity
 * @return boole -> TRUE on complete decoding; no network or filesystem access occurs
 */
boole desktop_tree_icons__decode(const char *data, ubyte *pixels, uint64 capacity)
{
    const char *payload = NULL;
    ubyte *bytes = NULL;
    ubyte *image = NULL;
    size_t decoded_size = 0;
    uint64 length = 0;
    int64 width = 0;
    int64 height = 0;
    int64 row = 0;
    int64 x = 0;
    int64 y = 0;
    boole valid = FALSE;
    if (!pixels || capacity < 4096 || !data)
    {
        return FALSE;
    }
    clib__null_memory(pixels, capacity);
    length = clib__strlen(data);
    if (length >= 90000 || clib__strncmp(data, "data:image/", 11) != 0)
    {
        return FALSE;
    }
    payload = clib__strstr(data, ";base64,");
    if (!payload || payload - data > 40)
    {
        return FALSE;
    }
    payload += 8;
    bytes = calloc(1, 65537);
    if (!bytes || mbedtls_base64_decode(bytes, 65536, &decoded_size, (const ubyte *)payload, length - (uint64)(payload - data)) != 0 || !decoded_size)
    {
        goto label_finished;
    }
    if (clib__strncmp(data, "data:image/svg+xml;base64,", 26) == 0)
    {
        valid = !clib__memchr(bytes, 0, decoded_size) && _tree_icons_internal__svg((char *)bytes, pixels);
    }
    else
    {
        image = desktop_attachment_image__decode_bounded(bytes, decoded_size, &width, &height, 256, 32);
        if (image)
        {
            x = (32 - width) / 2;
            y = (32 - height) / 2;
            for (row = 0; row < height; row++)
            {
                clib__memcpy(pixels + ((row + y) * 32 + x) * 4, image + row * width * 4, width * 4);
            }
            valid = TRUE;
        }
    }
label_finished:
    desktop_memory__erase(image, (uint64)width * (uint64)height * 4);
    free(image);
    desktop_memory__erase(bytes, 65537);
    free(bytes);
    return valid;
}

/**
 * @brief loads one packaged country flag; a country code cannot select an arbitrary path.
 * @param const char* country -> exactly two lowercase ASCII letters
 * @param ubyte* pixels -> caller-owned RGBA output
 * @param uint64 capacity -> output capacity, at least 4096 bytes
 * @return boole -> TRUE when the bundled flag was rasterized
 */
boole desktop_tree_icons__flag(const char *country, ubyte *pixels, uint64 capacity)
{
    char path[64] = { 0 };
    ubyte *bytes = NULL;
    char *text = NULL;
    uint64 size = 0;
    boole valid = FALSE;
    if (!pixels || capacity < 4096 || !country || clib__strlen(country) != 2 || country[0] < 'a' || country[0] > 'z' || country[1] < 'a' || country[1] > 'z')
    {
        return FALSE;
    }
    clib__null_memory(pixels, capacity);
    clib__string_format(path, sizeof(path), "assets/flags/%s.svg", country);
    bytes = desktop_attachment_file__read(path, 65536, &size);
    if (bytes && size && !clib__memchr(bytes, 0, size))
    {
        text = calloc(1, size + 1);
        if (text)
        {
            clib__memcpy(text, bytes, size);
            valid = _tree_icons_internal__svg(text, pixels);
        }
    }
    desktop_memory__erase(text, size + 1);
    free(text);
    desktop_memory__erase(bytes, size);
    free(bytes);
    desktop_memory__erase(path, sizeof(path));
    return valid;
}
