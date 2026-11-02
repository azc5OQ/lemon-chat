// compositor.c - portable effect surfaces over the existing draw contract.
// Ordinary scenes use the GPU paths unchanged. Extended CSS scenes rasterize
// into premultiplied surfaces, then present one RGBA texture through the backend.
#include "types.h"
#include "compositor.h"
#include "renderer_backend.h"
#include "renderer_debug.h"
#include "effect_acceleration.h"
#include "motion.h"
#include "widgets/widget_navigation.h"
#include "widgets/widget_image_cache.h"
#include "gradient.h"
#include "gradient_sample_internal.h"
#include "parser_style.h"
#include "scene.h"
#include "clib/stdlib.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "cpu_features.h"
#include "third_party/log.h"

// The x86 vector code below exists in x86 builds only (the intrinsics do
// not compile for ARM); whether it runs is decided at run time from the
// CPU's reported features (cpu_features.c), so one binary runs the scalar
// painter on a CPU without SSE2 and the vector painter on one with it.
// No worker thread or GPU setting is involved.
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#include <string.h>
#include <stdlib.h>
#define COMPOSITOR_X86_SIMD 1
#endif

#define COMPOSITOR_STACK_CAP 128
#define COMPOSITOR_LAYER_CAP EFFECT_BACKGROUND_LAYER_CAP
// the most tiles of one background layer the backend draws (renderer_backend__submit_tiles)
#define COMPOSITOR_TILE_CAP 2048
#define COMPOSITOR_BACKGROUND_CACHE_CAP 32
#define COMPOSITOR_DECORATION_CACHE_CAP 32
#define COMPOSITOR_DECORATION_PROPERTIES 18
#define COMPOSITOR_COORDINATE_CACHE_CAP 16

typedef struct _compositor_internal__surface_t
{
    ubyte* pixels; // 0x0
    int x;        // 0x8
    int y;        // 0xC
    int w;        // 0x10
    int h;        // 0x14
    int64 capacity; // 0x18: allocated bytes, retained across size changes
} _compositor_internal__surface_t;

typedef struct _compositor_internal__texture_t
{
    void* handle; // 0x0
    ubyte* pixels; // 0x8
    int w;        // 0x10
    int h;        // 0x14
    int channels; // 0x18
    struct _compositor_internal__texture_t* next; // 0x20
} _compositor_internal__texture_t;

typedef effect_background_layer_t _compositor_internal__layer_t;

typedef struct _compositor_internal__background_key_t
{
    _compositor_internal__layer_t layers[COMPOSITOR_LAYER_CAP]; // 0x0
    gui_color_t base;
    float x;
    float y;
    int w;
    int h;
    int count;
    int left;
    int top;
} _compositor_internal__background_key_t;

typedef struct _compositor_internal__background_cache_t
{
    gui_node_t* owner; // 0x0: identity only; never dereferenced
    gui_color_t* pixels; // 0x8
    int64 bytes; // 0x10
    uint64 used; // 0x18
    boole valid; // 0x20
    _compositor_internal__background_key_t key; // 0x24
} _compositor_internal__background_cache_t;

typedef struct _compositor_internal__group_t
{
    _compositor_internal__surface_t surface; // 0x0
    gui_rect_t clip; // 0x20
    gui_node_t* node; // 0x30
    gui_node_t* text_node; // 0x38
    float opacity; // 0x40
    int decoration; // cache slot + 1, or zero when not eligible
    boole reused; // effects are already applied to this surface
    boole fused; // leaf background was painted and composed into the parent
    boole parent_clipped;
    uint64 content_version;
    // a draw reached this surface (layer mode skips the upload of an
    // untouched top group, such as a node whose only image never decoded)
    boole painted;
    // the layer texture cache already holds this decoration content: the
    // surface pixels are not needed (no clear, no copy from the cache)
    boole texture_cached;
    // the surface holds defined pixels (zeroed, or copied from the
    // decoration cache); a top group in layer mode clears on first use
    boole cleared;
    // the backend draws this group's content into a render target
    // (renderer_backend__begin_layer): submits are forwarded in layer
    // coordinates and the surface pixels are never touched
    boole gpu;
} _compositor_internal__group_t;

typedef struct _compositor_internal__coordinate_key_t
{
    float x;
    float y;
    int left;
    int top;
    int w;
    int h;
    _compositor_internal__layer_t layer; // color ramp and angle cleared in key
} _compositor_internal__coordinate_key_t;

typedef struct _compositor_internal__coordinate_cache_t
{
    float* pixels;
    int64 bytes;
    uint64 used;
    boole valid;
    _compositor_internal__coordinate_key_t key;
} _compositor_internal__coordinate_cache_t;

typedef struct _compositor_internal__decoration_key_t
{
    gui_style_t style;
    gui_rect_t bounds;
    gui_rect_t clip;
    float scale;
    int x;
    int y;
    int w;
    int h;
    char values[COMPOSITOR_DECORATION_PROPERTIES][2048];
} _compositor_internal__decoration_key_t;

typedef struct _compositor_internal__decoration_cache_t
{
    gui_node_t* owner; // identity only; never dereferenced
    ubyte* pixels;
    int64 bytes;
    uint64 used;
    boole valid;
    _compositor_internal__decoration_key_t key;
    uint64 content_version;
} _compositor_internal__decoration_cache_t;

static _compositor_internal__texture_t* _compositor_internal__textures = NULL_POINTER;
static _compositor_internal__texture_t* _compositor_internal__atlas = NULL_POINTER;
static _compositor_internal__group_t _compositor_internal__groups[COMPOSITOR_STACK_CAP];
static gui_rect_t _compositor_internal__clips[COMPOSITOR_STACK_CAP];
static int _compositor_internal__depth = 0;
static int _compositor_internal__clip_count = 0;
static boole _compositor_internal__enabled = FALSE;
// the requested routing of extended scenes (renderer.h); AUTO resolves per
// frame to LAYERS on a backend that supports it, FRAME elsewhere
static gui_compositor_mode_e _compositor_internal__requested_mode = GUI_COMPOSITOR_MODE__AUTO;
static boole _compositor_internal__mode_from_environment = FALSE;
static boole _compositor_internal__mode_logged = FALSE;
// the CPU reported SSE2 (read once per frame from cpu_features): the
// vector sinks run only then, the scalar ones otherwise
static boole _compositor_internal__sse2 = FALSE;
// the frame's size in every mode. groups[0].surface describes the viewport
// only while the compositor is enabled; a backdrop blur of a frame the
// backend draws itself is bounded by these two
static int64 _compositor_internal__viewport_w = 0;
static int64 _compositor_internal__viewport_h = 0;
// backdrop blurs since the start, by what filtered them: the backend's GPU
// filter, or a CPU routine (read-back, filter, upload; or the frame
// compositor's own surface). renderer__backdrop_blur_counts reads them.
static uint64 _compositor_internal__blurs_gpu = 0;
static uint64 _compositor_internal__blurs_cpu = 0;

// Layer mode (session/LAYERS-DESIGN.md): the backend draws the frame, an
// effect group opened at depth 0 is rasterized on the CPU and drawn as one
// textured quad, an extended background of a plain node becomes an implicit
// layer. groups[0].surface is then a pixel-less viewport descriptor: clip
// queries see the viewport and nothing rasterizes into it.
#define COMPOSITOR_LAYER_TEXTURE_CAP 64
typedef struct _compositor_internal__layer_texture_t
{
    gui_node_t* owner; // identity only; never dereferenced
    uint64 content_version; // decoration content the texture holds, or zero
    uint64 key; // implicit background layer inputs the texture holds, or zero
    void* texture;
    int w;
    int h;
    uint64 used;
} _compositor_internal__layer_texture_t;

static boole _compositor_internal__layer_mode = FALSE;
// a read-back region composes on the CPU: the compute detour would upload
// and read the region back again
static boole _compositor_internal__no_acceleration = FALSE;
static _compositor_internal__layer_texture_t _compositor_internal__layer_textures[COMPOSITOR_LAYER_TEXTURE_CAP];
static int64 _compositor_internal__layer_texture_bytes = 0;
// textures drawn once this frame outside the cache (a full cache, a layer
// over budget); destroyed after the backend finished the frame
#define COMPOSITOR_LAYER_TRANSIENT_CAP 64
static void* _compositor_internal__layer_transients[COMPOSITOR_LAYER_TRANSIENT_CAP];
static int _compositor_internal__layer_transient_count = 0;
static _compositor_internal__surface_t _compositor_internal__layer_scratch;
// implicit background layers are drawn from their texture; the CPU float
// sample cache would be filled for nothing
static boole _compositor_internal__no_float_cache = FALSE;
// layer-mode counters since startup, logged at shutdown (and read by the
// paint audit): what a frame spends its layer work on
typedef struct _compositor_internal__stats_t
{
    uint64 frames;
    uint64 presents;
    uint64 readbacks;
    uint64 uploads;
    uint64 upload_bytes;
    uint64 texture_hits;
    uint64 implicit_rasters;
    uint64 implicit_hits;
    uint64 transients;
    uint64 scissor_forwards;
    uint64 blur_layers;
    uint64 gpu_groups;
    uint64 tile_draws; // backgrounds drawn as backend tiles
} _compositor_internal__stats_t;
static _compositor_internal__stats_t _compositor_internal__stats;
static gui_node_t* _compositor_internal__text_node = NULL_POINTER;
static ubyte* _compositor_internal__blur_scratch = NULL_POINTER;
static int64 _compositor_internal__blur_capacity = 0;
static _compositor_internal__background_cache_t _compositor_internal__background_cache[COMPOSITOR_BACKGROUND_CACHE_CAP];
static int64 _compositor_internal__background_bytes = 0;
static uint64 _compositor_internal__frame = 0;
static _compositor_internal__decoration_cache_t _compositor_internal__decorations[COMPOSITOR_DECORATION_CACHE_CAP];
static int64 _compositor_internal__decoration_bytes = 0;
// Never reset or recycle a content identity while an effect device can retain it.
static uint64 _compositor_internal__content_version = 0;
static _compositor_internal__coordinate_cache_t _compositor_internal__coordinates[COMPOSITOR_COORDINATE_CACHE_CAP];
static int64 _compositor_internal__coordinate_bytes = 0;

static float _compositor_internal__clamp(float x);
static gui_rect_t _compositor_internal__intersection(gui_rect_t a, gui_rect_t b);
static gui_rect_t _compositor_internal__clip(gui_rect_t bounds);
static _compositor_internal__texture_t* _compositor_internal__texture(void* handle);
static void _compositor_internal__retain(void* handle, const ubyte* pixels, int w, int h, int channels);
static void _compositor_internal__release(void* handle);
static void _compositor_internal__blend(int x, int y, gui_color_t color);
#if defined(COMPOSITOR_X86_SIMD)
static inline __m128 _compositor_internal__rgba_float(const ubyte* pixel);
static inline void _compositor_internal__store_rgba(ubyte* pixel, __m128 value);
static inline __m128 _compositor_internal__with_alpha(__m128 rgb, __m128 alpha4);
static void _compositor_internal__compose_blend_aligned(_compositor_internal__surface_t* destination, const _compositor_internal__surface_t* surface, gui_rect_t bounds, const float* inverse, float opacity, int blend_mode);
#endif
static float _compositor_internal__rounded(float x, float y, float w, float h, float radius);
static void _compositor_internal__rect(gui_rect_t r, gui_color_t color, const gui_gradient_t* gradient, float scale, float opacity, float radius);
static void _compositor_internal__image(gui_rect_t rect, _compositor_internal__texture_t* texture, gui_color_t tint, float u0, float v0, float u1, float v1);
static gui_color_t _compositor_internal__texel(_compositor_internal__texture_t* texture, float x, float y);
static const char* _compositor_internal__part(const char* p, char* out, int cap, char delimiter);
static int _compositor_internal__layers(gui_node_t* node, int property, _compositor_internal__layer_t* layers, int cap, float scale);
static gui_color_t _compositor_internal__layer_sample(const _compositor_internal__layer_t* layer, float x, float y);
static gui_color_t _compositor_internal__over(gui_color_t front, gui_color_t back);
static int _compositor_internal__blur_radius(float sigma);
static void _compositor_internal__blur(_compositor_internal__surface_t* surface, float sigma);
static void _compositor_internal__blur_direct(gui_rect_t rect, float sigma);
static void _compositor_internal__effects(gui_node_t* node, _compositor_internal__surface_t* surface, float scale);
static int _compositor_internal__polygon(const char* text, float* points, int cap, float w, float h, float scale);
static float _compositor_internal__polygon_coverage(const float* points, int count, float x, float y);
static void _compositor_internal__text_effects(gui_node_t* node, _compositor_internal__surface_t* surface, float scale);
static void _compositor_internal__filter(_compositor_internal__surface_t* surface, const char* filter, float scale);
static void _compositor_internal__four(const char* value, float* result, float reference, float scale);
static void _compositor_internal__matrix(gui_node_t* node, float scale, float* matrix);
static void _compositor_internal__project(const float* matrix, float x, float y, float* px, float* py);
static boole _compositor_internal__inverse(const float* matrix, float* inverse);
static void _compositor_internal__compose(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, uint64 content_version);
static inline boole _compositor_internal__painting(void);
static inline boole _compositor_internal__gpu_group(void);
static inline gui_rect_t _compositor_internal__gpu_rect(gui_rect_t r);
static inline gui_vec2_t _compositor_internal__gpu_point(gui_vec2_t p);
static boole _compositor_internal__gpu_group_possible(gui_node_t* node, float scale);
static boole _compositor_internal__background_gpu(gui_node_t* node, float scale);
static void _compositor_internal__present_texture(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, void* texture, int blend_mode);
static inline void _compositor_internal__touch(void);
static void _compositor_internal__ensure_cleared(_compositor_internal__group_t* group);
static boole _compositor_internal__layer_texture_cached(gui_node_t* owner, uint64 content_version);
static _compositor_internal__surface_t* _compositor_internal__scratch(int x, int y, int w, int h, boole clear);
static uint64 _compositor_internal__hash(uint64 h, const void* bytes, int64 length);
static void* _compositor_internal__layer_texture(gui_node_t* owner, uint64 content_version, uint64 key, const ubyte* pixels, int w, int h, boole* reused);
static void _compositor_internal__layer_textures_age(boole all);
static void _compositor_internal__present_layer(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, uint64 content_version);
static void _compositor_internal__present_readback(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, uint64 content_version);
static boole _compositor_internal__background_layer(gui_node_t* node, float scale);
static boole _compositor_internal__tile_grid(const _compositor_internal__layer_t* layer, gui_rect_t box, gui_rect_t clip, gui_rect_t* tile, int* columns, int* rows);
static boole _compositor_internal__background_tiles(gui_node_t* node, const _compositor_internal__layer_t* layers, int count, gui_rect_t box, gui_rect_t clip, float scale, float opacity);
static void _compositor_internal__blur_layer(gui_rect_t rect, float sigma);
static _compositor_internal__background_cache_t* _compositor_internal__cached_background(gui_node_t* node, const _compositor_internal__background_key_t* key, boole create);
static _compositor_internal__coordinate_cache_t* _compositor_internal__cached_coordinates(const _compositor_internal__background_key_t* background, int layer, boole create);
static float _compositor_internal__conic_coordinate(const _compositor_internal__layer_t* layer, float x, float y);

/**
 * @brief clamp a channel or coverage value
 * @param float x -> input
 * @return float -> value in zero to one
 */
static float _compositor_internal__clamp(float x)
{
    return stdlib__fminf(1.0f, stdlib__fmaxf(0.0f, x));
}

/**
 * @brief intersect two device rectangles
 * @param gui_rect_t a -> first rectangle
 * @param gui_rect_t b -> second rectangle
 * @return gui_rect_t -> nonnegative intersection
 */
static gui_rect_t _compositor_internal__intersection(gui_rect_t a, gui_rect_t b)
{
    gui_rect_t result;

    stdlib__null_memory(&result, sizeof(result));
    result.x = stdlib__fmaxf(a.x, b.x);
    result.y = stdlib__fmaxf(a.y, b.y);
    result.w = stdlib__fmaxf(0.0f, stdlib__fminf(a.x + a.w, b.x + b.w) - result.x);
    result.h = stdlib__fmaxf(0.0f, stdlib__fminf(a.y + a.h, b.y + b.h) - result.y);
    return result;
}

/**
 * @brief clip a draw to the current surface and scissor
 * @param gui_rect_t bounds -> draw rectangle
 * @return gui_rect_t -> drawable rectangle
 */
static gui_rect_t _compositor_internal__clip(gui_rect_t bounds)
{
    _compositor_internal__surface_t* surface = &_compositor_internal__groups[_compositor_internal__depth].surface;
    gui_rect_t area;

    stdlib__null_memory(&area, sizeof(area));
    area.x = (float)surface->x;
    area.y = (float)surface->y;
    area.w = (float)surface->w;
    area.h = (float)surface->h;
    bounds = _compositor_internal__intersection(bounds, area);
    if (_compositor_internal__clip_count > 0)
    {
        bounds = _compositor_internal__intersection(bounds, _compositor_internal__clips[_compositor_internal__clip_count - 1]);
    }
    return bounds;
}

/**
 * @brief find a retained texture by backend handle
 * @param void* handle -> backend texture
 * @return _compositor_internal__texture_t* -> matching CPU copy or NULL
 */
static _compositor_internal__texture_t* _compositor_internal__texture(void* handle)
{
    _compositor_internal__texture_t* item = _compositor_internal__textures;

    while (item != NULL_POINTER && item->handle != handle)
    {
        item = item->next;
    }
    return item;
}

/**
 * @brief retain source pixels so effect surfaces can consume ordinary image and glyph draws
 * @param void* handle -> backend handle
 * @param const ubyte* pixels -> source bytes
 * @param int w -> width
 * @param int h -> height
 * @param int channels -> one for a glyph atlas or four for an image
 * @return void
 */
static void _compositor_internal__retain(void* handle, const ubyte* pixels, int w, int h, int channels)
{
    _compositor_internal__texture_t* item = NULL_POINTER;
    int64 bytes = (int64)w * h * channels;

    if (handle == NULL_POINTER || pixels == NULL_POINTER || w <= 0 || h <= 0 || bytes > 268435456)
    {
        return;
    }
    item = GUI_CALLOC_T(1, sizeof(*item), MM_TYPE__GENERIC);
    if (item == NULL_POINTER)
    {
        return;
    }
    item->pixels = GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
    if (item->pixels == NULL_POINTER)
    {
        GUI_FREE(item);
        return;
    }
    stdlib__memcpy(item->pixels, pixels, bytes);
    item->handle = handle;
    item->w = w;
    item->h = h;
    item->channels = channels;
    item->next = _compositor_internal__textures;
    _compositor_internal__textures = item;
}

/**
 * @brief release the retained source for a backend texture
 * @param void* handle -> backend handle
 * @return void
 */
static void _compositor_internal__release(void* handle)
{
    _compositor_internal__texture_t** link = &_compositor_internal__textures;
    _compositor_internal__texture_t* item = NULL_POINTER;

    while (*link != NULL_POINTER)
    {
        item = *link;
        if (item->handle == handle)
        {
            *link = item->next;
            if (_compositor_internal__atlas == item)
            {
                _compositor_internal__atlas = NULL_POINTER;
            }
            GUI_FREE(item->pixels);
            GUI_FREE(item);
            return;
        }
        link = &item->next;
    }
}

/**
 * @brief composite one straight RGBA color into a premultiplied target
 * @param int x -> device x
 * @param int y -> device y
 * @param gui_color_t color -> straight RGBA color
 * @return void
 */
#if defined(COMPOSITOR_X86_SIMD)
// The per-pixel sinks below run the scalar formulas four channels at a
// time with the same float operations in the same order, so every byte
// they write equals the scalar result (IEEE single precision either way,
// no fused multiply-add, truncating conversion, saturating pack).

/**
 * @brief load four byte channels as four floats without alignment assumptions
 * @param const ubyte* pixel -> one complete RGBA pixel
 * @return __m128 -> channel values in the range zero to 255
 */
static inline __m128 _compositor_internal__rgba_float(const ubyte* pixel)
{
    uint packed;
    // Explicit bytes keep the unaligned, alias-safe load local to this loop.
    packed=(uint)pixel[0] | ((uint)pixel[1]<<8) | ((uint)pixel[2]<<16) | ((uint)pixel[3]<<24);
    __m128i bytes = _mm_cvtsi32_si128((int)packed);
    bytes = _mm_unpacklo_epi8(bytes, _mm_setzero_si128());
    bytes = _mm_unpacklo_epi16(bytes, _mm_setzero_si128());
    return _mm_cvtepi32_ps(bytes);
}

/**
 * @brief store four float channels as bytes the way the scalar `(ubyte)` cast does
 * @param ubyte* pixel -> destination RGBA pixel
 * @param __m128 value -> channel values, already offset by one half
 * @return void
 */
static inline void _compositor_internal__store_rgba(ubyte* pixel, __m128 value)
{
    __m128i bytes = _mm_cvttps_epi32(value);
    uint packed = 0;

    bytes = _mm_packs_epi32(bytes, bytes);
    bytes = _mm_packus_epi16(bytes, bytes);
    packed = (uint)_mm_cvtsi128_si32(bytes);
    pixel[0]=(ubyte)packed; pixel[1]=(ubyte)(packed>>8); pixel[2]=(ubyte)(packed>>16); pixel[3]=(ubyte)(packed>>24);
}

/**
 * @brief the rgb lanes of one vector with the alpha lane of another
 * @param __m128 rgb -> lanes 0 to 2 are kept
 * @param __m128 alpha4 -> lane 3 is placed in lane 3 of the result
 * @return __m128 -> (rgb.r, rgb.g, rgb.b, alpha4[3])
 */
static inline __m128 _compositor_internal__with_alpha(__m128 rgb, __m128 alpha4)
{
    return _mm_shuffle_ps(rgb, _mm_unpackhi_ps(rgb, alpha4), _MM_SHUFFLE(3, 0, 1, 0));
}
#endif

static void _compositor_internal__blend(int x, int y, gui_color_t color)
{
    _compositor_internal__surface_t* surface = &_compositor_internal__groups[_compositor_internal__depth].surface;
    ubyte* p = NULL_POINTER;
    float a = _compositor_internal__clamp(color.a);
    float inv = 1.0f - a;
#if defined(COMPOSITOR_X86_SIMD)
    __m128 source;
#endif

    if (a <= 0.0f || surface->pixels == NULL_POINTER || x < surface->x || y < surface->y || x >= surface->x + surface->w || y >= surface->y + surface->h)
    {
        return;
    }
    p = surface->pixels + ((int64)(y - surface->y) * surface->w + x - surface->x) * 4;
#if defined(COMPOSITOR_X86_SIMD)
    if (_compositor_internal__sse2 == TRUE)
    {
        // rgb lanes: clamp(c) * a * 255; alpha lane: a * 255 (its multiplier is one)
        source = _mm_min_ps(_mm_max_ps(_mm_set_ps(a, color.b, color.g, color.r), _mm_setzero_ps()), _mm_set1_ps(1.0f));
        source = _mm_mul_ps(_mm_mul_ps(source, _mm_set_ps(1.0f, a, a, a)), _mm_set1_ps(255.0f));
        _compositor_internal__store_rgba(p, _mm_add_ps(_mm_add_ps(source, _mm_mul_ps(_compositor_internal__rgba_float(p), _mm_set1_ps(inv))), _mm_set1_ps(0.5f)));
        return;
    }
#endif
    p[0] = (ubyte)(_compositor_internal__clamp(color.r) * a * 255.0f + p[0] * inv + 0.5f);
    p[1] = (ubyte)(_compositor_internal__clamp(color.g) * a * 255.0f + p[1] * inv + 0.5f);
    p[2] = (ubyte)(_compositor_internal__clamp(color.b) * a * 255.0f + p[2] * inv + 0.5f);
    p[3] = (ubyte)(a * 255.0f + p[3] * inv + 0.5f);
}

/**
 * @brief evaluate the renderer contract's rounded rectangle coverage
 * @param float x -> local x
 * @param float y -> local y
 * @param float w -> width
 * @param float h -> height
 * @param float radius -> circular radius
 * @return float -> antialiased coverage
 */
static float _compositor_internal__rounded(float x, float y, float w, float h, float radius)
{
    float qx = 0.0f;
    float qy = 0.0f;
    float d = 0.0f;
    float t = 0.0f;

    radius = stdlib__fminf(stdlib__fmaxf(radius, 0.0f), stdlib__fminf(w, h) * 0.5f);
    qx = stdlib__fabsf(x - w * 0.5f) - w * 0.5f + radius;
    qy = stdlib__fabsf(y - h * 0.5f) - h * 0.5f + radius;
    // Interior pixels have full coverage; only the edge band needs the SDF square root.
    if (qx <= 0.0f && qy <= 0.0f && stdlib__fmaxf(qx, qy) <= radius - 0.5f)
    {
        return 1.0f;
    }
    d = stdlib__fminf(stdlib__fmaxf(qx, qy), 0.0f) + stdlib__sqrtf(stdlib__fmaxf(qx, 0.0f) * stdlib__fmaxf(qx, 0.0f) + stdlib__fmaxf(qy, 0.0f) * stdlib__fmaxf(qy, 0.0f)) - radius;
    t = _compositor_internal__clamp(d + 0.5f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/**
 * @brief rasterize a solid or sampled gradient rectangle
 * @param gui_rect_t r -> device bounds
 * @param gui_color_t color -> solid color
 * @param const gui_gradient_t* gradient -> optional gradient
 * @param float scale -> logical scale
 * @param float opacity -> alpha multiplier
 * @param float radius -> corner radius
 * @return void
 */
static void _compositor_internal__rect(gui_rect_t r, gui_color_t color, const gui_gradient_t* gradient, float scale, float opacity, float radius)
{
    gui_rect_t clip;
    gradient_sampler_t sampler;
    gui_color_t c;
    int x = 0;
    int y = 0;
    int right = 0;
    int bottom = 0;
    ubyte* pixel = NULL_POINTER;
    ubyte rgba[4];
    int left = 0;
    int core_left, core_right, core_top, core_bottom;
    float alpha, inverse;
    float source[4];

    stdlib__null_memory(&sampler, sizeof(sampler));
    stdlib__null_memory(&c, sizeof(c));
    stdlib__null_memory(rgba, sizeof(rgba));
    if (gradient == NULL_POINTER && color.a * opacity <= 0.0f)
    {
        return;
    }
    clip = _compositor_internal__clip(r);
    _compositor_internal__touch();
    left = (int)stdlib__ceilf(clip.x - 0.5f);
    right = (int)stdlib__ceilf(clip.x + clip.w - 0.5f);
    bottom = (int)stdlib__ceilf(clip.y + clip.h - 0.5f);
    // Integer-aligned opaque boxes do not need destination reads or per-pixel coverage.
    if (gradient == NULL_POINTER && color.a * opacity >= 1.0f && radius == 0.0f && r.x == stdlib__floorf(r.x) && r.y == stdlib__floorf(r.y) && r.w == stdlib__floorf(r.w) && r.h == stdlib__floorf(r.h) && clip.w > 0.0f && clip.h > 0.0f)
    {
        left = (int)stdlib__ceilf(clip.x - 0.5f);
        rgba[0] = (ubyte)(_compositor_internal__clamp(color.r) * 255.0f + 0.5f);
        rgba[1] = (ubyte)(_compositor_internal__clamp(color.g) * 255.0f + 0.5f);
        rgba[2] = (ubyte)(_compositor_internal__clamp(color.b) * 255.0f + 0.5f);
        rgba[3] = 255;
        for (y = (int)stdlib__ceilf(clip.y - 0.5f); y < bottom; y++)
        {
            pixel = _compositor_internal__groups[_compositor_internal__depth].surface.pixels + ((int64)(y - _compositor_internal__groups[_compositor_internal__depth].surface.y) * _compositor_internal__groups[_compositor_internal__depth].surface.w + left - _compositor_internal__groups[_compositor_internal__depth].surface.x) * 4;
            for (x = left; x < right; x++)
            {
                pixel[0] = rgba[0];
                pixel[1] = rgba[1];
                pixel[2] = rgba[2];
                pixel[3] = rgba[3];
                pixel += 4;
            }
        }
        return;
    }
    if (gradient != NULL_POINTER)
    {
        gradient__prepare(gradient, r.w, r.h, scale, &sampler);
    }
    else
    {
        _compositor_internal__surface_t* surface = &_compositor_internal__groups[_compositor_internal__depth].surface;
        // A conservative interior is fully covered even at fractional bounds.
        // Keep a one-pixel margin beyond the normalized corner radius; edges
        // retain the original SDF and float blend order. No retained pixels.
        float normalized = stdlib__fminf(stdlib__fmaxf(radius, 0.0f), stdlib__fminf(r.w, r.h) * 0.5f);
        core_left = (int)stdlib__ceilf(r.x + normalized + 1.0f);
        core_right = (int)stdlib__floorf(r.x + r.w - normalized - 1.0f);
        core_top = (int)stdlib__ceilf(r.y + normalized + 1.0f);
        core_bottom = (int)stdlib__floorf(r.y + r.h - normalized - 1.0f);
        left = (int)stdlib__ceilf(clip.x - 0.5f);
        if (core_left < left) core_left = left;
        if (core_right > right) core_right = right;
        if (surface->pixels == NULL_POINTER) return;
        if (core_left < surface->x) core_left = surface->x;
        if (core_right > surface->x + surface->w) core_right = surface->x + surface->w;
        if (core_top < surface->y) core_top = surface->y;
        if (core_bottom > surface->y + surface->h) core_bottom = surface->y + surface->h;
        if (core_left < core_right && core_top < core_bottom)
        {
            alpha = _compositor_internal__clamp(color.a * opacity);
            inverse = 1.0f - alpha;
            source[0] = _compositor_internal__clamp(color.r) * alpha * 255.0f;
            source[1] = _compositor_internal__clamp(color.g) * alpha * 255.0f;
            source[2] = _compositor_internal__clamp(color.b) * alpha * 255.0f;
            source[3] = alpha * 255.0f;
            for (y = (int)stdlib__ceilf(clip.y - 0.5f); y < bottom; y++)
            {
                int first_edge_end = (y >= core_top && y < core_bottom) ? core_left : right;
                for (x = left; x < first_edge_end; x++)
                {
                    c = color;
                    c.a *= opacity * _compositor_internal__rounded(x + 0.5f - r.x, y + 0.5f - r.y, r.w, r.h, radius);
                    _compositor_internal__blend(x, y, c);
                }
                if (first_edge_end == right) continue;
                pixel = surface->pixels + ((int64)(y - surface->y) * surface->w + core_left - surface->x) * 4;
#if defined(COMPOSITOR_X86_SIMD)
                if (_compositor_internal__sse2 == TRUE)
                {
                    for (x = core_left; x < core_right; x++, pixel += 4)
                    {
                        _compositor_internal__store_rgba(pixel, _mm_add_ps(_mm_add_ps(_mm_loadu_ps(source), _mm_mul_ps(_compositor_internal__rgba_float(pixel), _mm_set1_ps(inverse))), _mm_set1_ps(0.5f)));
                    }
                    pixel = NULL_POINTER;
                }
#endif
                for (x = (pixel == NULL_POINTER) ? core_right : core_left; x < core_right; x++, pixel += 4)
                {
                    pixel[0] = (ubyte)(source[0] + pixel[0] * inverse + 0.5f);
                    pixel[1] = (ubyte)(source[1] + pixel[1] * inverse + 0.5f);
                    pixel[2] = (ubyte)(source[2] + pixel[2] * inverse + 0.5f);
                    pixel[3] = (ubyte)(source[3] + pixel[3] * inverse + 0.5f);
                }
                for (x = core_right; x < right; x++)
                {
                    c = color;
                    c.a *= opacity * _compositor_internal__rounded(x + 0.5f - r.x, y + 0.5f - r.y, r.w, r.h, radius);
                    _compositor_internal__blend(x, y, c);
                }
            }
            return;
        }
    }
    for (y = (int)stdlib__ceilf(clip.y - 0.5f); y < bottom; y++)
    {
        for (x = (int)stdlib__ceilf(clip.x - 0.5f); x < right; x++)
        {
            c = (gradient == NULL_POINTER) ? color : gradient__sample(&sampler, x + 0.5f - r.x, y + 0.5f - r.y);
            c.a *= opacity * _compositor_internal__rounded(x + 0.5f - r.x, y + 0.5f - r.y, r.w, r.h, radius);
            _compositor_internal__blend(x, y, c);
        }
    }
}

/**
 * @brief bilinearly sample a retained atlas or image
 * @param _compositor_internal__texture_t* texture -> source
 * @param float x -> texel x
 * @param float y -> texel y
 * @return gui_color_t -> straight color
 */
static gui_color_t _compositor_internal__texel(_compositor_internal__texture_t* texture, float x, float y)
{
    gui_color_t color = {0.0f, 0.0f, 0.0f, 0.0f};
    float fx = 0.0f;
    float fy = 0.0f;
    float w00 = 0.0f;
    float w10 = 0.0f;
    float w01 = 0.0f;
    float w11 = 0.0f;
    int ix = 0;
    int iy = 0;
    int x0 = 0;
    int x1 = 0;
    int y0 = 0;
    int y1 = 0;
    const ubyte* p00 = NULL_POINTER;
    const ubyte* p10 = NULL_POINTER;
    const ubyte* p01 = NULL_POINTER;
    const ubyte* p11 = NULL_POINTER;

    if (texture == NULL_POINTER)
    {
        return color;
    }
    ix = (int)stdlib__floorf(x);
    iy = (int)stdlib__floorf(y);
    fx = x - ix;
    fy = y - iy;
    x0 = (ix < 0) ? 0 : ((ix >= texture->w) ? texture->w - 1 : ix);
    x1 = (ix + 1 < 0) ? 0 : ((ix + 1 >= texture->w) ? texture->w - 1 : ix + 1);
    y0 = (iy < 0) ? 0 : ((iy >= texture->h) ? texture->h - 1 : iy);
    y1 = (iy + 1 < 0) ? 0 : ((iy + 1 >= texture->h) ? texture->h - 1 : iy + 1);
    w00 = (1.0f - fx) * (1.0f - fy) / 255.0f;
    w10 = fx * (1.0f - fy) / 255.0f;
    w01 = (1.0f - fx) * fy / 255.0f;
    w11 = fx * fy / 255.0f;
    p00 = texture->pixels + ((int64)y0 * texture->w + x0) * texture->channels;
    p10 = texture->pixels + ((int64)y0 * texture->w + x1) * texture->channels;
    p01 = texture->pixels + ((int64)y1 * texture->w + x0) * texture->channels;
    p11 = texture->pixels + ((int64)y1 * texture->w + x1) * texture->channels;
    if (texture->channels == 1)
    {
        color.r = 1.0f;
        color.g = 1.0f;
        color.b = 1.0f;
        color.a = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
    }
    else
    {
        color.r = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
        color.g = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
        color.b = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;
        color.a = p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11;
    }
    return color;
}

/**
 * @brief rasterize an image or glyph with the standard UV mapping
 * @param gui_rect_t rect -> destination
 * @param _compositor_internal__texture_t* texture -> retained source
 * @param gui_color_t tint -> RGBA multiplier
 * @param float u0 -> left UV
 * @param float v0 -> top UV
 * @param float u1 -> right UV
 * @param float v1 -> bottom UV
 * @return void
 */
static void _compositor_internal__image(gui_rect_t rect, _compositor_internal__texture_t* texture, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    _compositor_internal__touch();
    gui_rect_t clip;
    gui_color_t color;
    float u = 0.0f;
    float v = 0.0f;
    int x = 0;
    int y = 0;
    int right = 0;
    int bottom = 0;

    stdlib__null_memory(&color, sizeof(color));
    if (texture == NULL_POINTER || rect.w <= 0.0f || rect.h <= 0.0f)
    {
        return;
    }
    clip = _compositor_internal__clip(rect);
    // GPU quads cover pixel centers inside the rectangle. Visiting floor(x)
    // through ceil(right) also samples outside fractional glyph quads, leaking
    // neighboring atlas texels into the spaces between letters.
    right = (int)stdlib__ceilf(clip.x + clip.w - 0.5f);
    bottom = (int)stdlib__ceilf(clip.y + clip.h - 0.5f);
    for (y = (int)stdlib__ceilf(clip.y - 0.5f); y < bottom; y++)
    {
        v = v0 + (v1 - v0) * (y + 0.5f - rect.y) / rect.h;
        for (x = (int)stdlib__ceilf(clip.x - 0.5f); x < right; x++)
        {
            u = u0 + (u1 - u0) * (x + 0.5f - rect.x) / rect.w;
            color = _compositor_internal__texel(texture, u * texture->w - 0.5f, v * texture->h - 0.5f);
            if (texture->channels == 1 && color.a > 0 && color.a < 1)
            {
                // Match the native text shaders: reshape interpolated glyph
                // coverage before blending into the byte-color target.
                color.a = stdlib__powf(color.a, 1.0f / 2.2f);
            }
            color.r *= tint.r;
            color.g *= tint.g;
            color.b *= tint.b;
            color.a *= tint.a;
            _compositor_internal__blend(x, y, color);
        }
    }
}

/**
 * @brief report whether this frame uses effect surfaces
 * @return boole -> current frame's rendering mode
 */
boole compositor__active(void)
{
    return _compositor_internal__enabled;
}

/**
 * @brief select how extended scenes are drawn (renderer.h); before the first frame
 * @param gui_compositor_mode_e mode -> AUTO, FRAME or LAYERS
 * @return void
 */
void renderer__set_compositor_mode(gui_compositor_mode_e mode)
{
    _compositor_internal__requested_mode = mode;
    // an explicit AUTO consults MEEK_UI_FRAME_COMPOSITOR again
    _compositor_internal__mode_from_environment = (boole)(mode != GUI_COMPOSITOR_MODE__AUTO);
}

/**
 * @brief the requested routing, after reading MEEK_UI_FRAME_COMPOSITOR once
 * @return gui_compositor_mode_e -> AUTO, FRAME or LAYERS as requested
 */
gui_compositor_mode_e renderer__compositor_mode(void)
{
    const char* value = NULL_POINTER;

    if (_compositor_internal__mode_from_environment == FALSE)
    {
        _compositor_internal__mode_from_environment = TRUE;
        value = stdlib__getenv("MEEK_UI_FRAME_COMPOSITOR");
        if (value != NULL_POINTER && stdlib__strcmp(value, "1") == 0)
        {
            _compositor_internal__requested_mode = GUI_COMPOSITOR_MODE__FRAME;
        }
    }
    return _compositor_internal__requested_mode;
}

void renderer__submit_circle(gui_vec2_t center, float radius, gui_color_t color)
{
    gui_rect_t bounds = {center.x - radius, center.y - radius, radius * 2, radius * 2};
    if (radius > 0) renderer__submit_rect(bounds, color, radius);
}

void renderer__submit_line(gui_vec2_t from, gui_vec2_t to, float width, gui_color_t color)
{
    float dx = to.x - from.x, dy = to.y - from.y;
    float length = stdlib__sqrtf(dx * dx + dy * dy);
    gui_vec2_t a, b, c, d;
    if (width <= 0 || length <= 0) return;
    dx *= width * .5f / length;
    dy *= width * .5f / length;
    a = (gui_vec2_t){from.x - dy, from.y + dx};
    b = (gui_vec2_t){to.x - dy, to.y + dx};
    c = (gui_vec2_t){to.x + dy, to.y - dx};
    d = (gui_vec2_t){from.x + dy, from.y - dx};
    renderer__submit_triangle(a, b, c, color);
    renderer__submit_triangle(a, c, d, color);
}

void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color)
{
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    gui_rect_t clip;
    gui_vec2_t vertices[3];
    int x, y, edge;
    if (area == 0 || color.a <= 0) return;
    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__submit_triangle(_compositor_internal__gpu_point(a), _compositor_internal__gpu_point(b), _compositor_internal__gpu_point(c), color);
        return;
    }
    _compositor_internal__touch();
    if (area < 0) { gui_vec2_t swap = b; b = c; c = swap; }
    vertices[0] = a; vertices[1] = b; vertices[2] = c;
    clip.x = stdlib__fminf(a.x, stdlib__fminf(b.x, c.x));
    clip.y = stdlib__fminf(a.y, stdlib__fminf(b.y, c.y));
    clip.w = stdlib__fmaxf(a.x, stdlib__fmaxf(b.x, c.x)) - clip.x;
    clip.h = stdlib__fmaxf(a.y, stdlib__fmaxf(b.y, c.y)) - clip.y;
    clip = _compositor_internal__clip(clip);
    for (y = (int)stdlib__floorf(clip.y); y < (int)stdlib__ceilf(clip.y + clip.h); y++)
        for (x = (int)stdlib__floorf(clip.x); x < (int)stdlib__ceilf(clip.x + clip.w); x++)
        {
            for (edge = 0; edge < 3; edge++)
            {
                gui_vec2_t p = vertices[edge], q = vertices[(edge + 1) % 3];
                float dx = q.x - p.x, dy = q.y - p.y;
                float cross = dx * (y + .5f - p.y) - dy * (x + .5f - p.x);
                // Top-left ownership: adjacent triangles never double-blend.
                if (cross < 0 || (cross == 0 && !(dy < 0 || (dy == 0 && dx > 0)))) break;
            }
            if (edge == 3) _compositor_internal__blend(x, y, color);
        }
}

/**
 * @brief begin an ordinary GPU frame or a portable effect frame
 * @param int64 viewport_w -> width
 * @param int64 viewport_h -> height
 * @param gui_color_t clear -> clear color
 * @return void
 */
void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color_t clear)
{
    _compositor_internal__surface_t* surface = &_compositor_internal__groups[0].surface;
    gui_rect_t bounds;

    stdlib__null_memory(&bounds, sizeof(bounds));
    renderer_backend__begin_frame(viewport_w, viewport_h, clear);
    _compositor_internal__viewport_w = viewport_w;
    _compositor_internal__viewport_h = viewport_h;
    _compositor_internal__sse2 = cpu_features__has(CPU_FEATURE__SSE2);
    _compositor_internal__enabled = (boole)(renderer_backend__needs_compositor() || motion__needs_compositor() || widget_navigation__active(scene__root()));
    _compositor_internal__depth = 0;
    _compositor_internal__clip_count = 0;
    _compositor_internal__text_node = NULL_POINTER;
    _compositor_internal__frame++;
    if (_compositor_internal__enabled == FALSE)
    {
        return;
    }
    if (viewport_w <= 0 || viewport_h <= 0 || viewport_w > 8192 || viewport_h > 8192)
    {
        _compositor_internal__enabled = FALSE;
        return;
    }
    // layer mode: the backend draws the frame; the depth-0 surface is a
    // pixel-less viewport descriptor and the backend's clear stands
    _compositor_internal__layer_mode = (boole)(renderer_backend__needs_compositor() == FALSE && renderer__compositor_mode() != GUI_COMPOSITOR_MODE__FRAME && renderer_backend__supports_layers() == TRUE);
    if (_compositor_internal__mode_logged == FALSE)
    {
        _compositor_internal__mode_logged = TRUE;
        log_info("%s %s%s %s", "compositor mode:", (_compositor_internal__layer_mode == TRUE) ? "LAYERS" : "FRAME", ", backend layers:", (renderer_backend__supports_layers() == TRUE) ? "yes" : "no");
    }
    if (_compositor_internal__layer_mode == TRUE)
    {
        GUI_FREE(surface->pixels);
        surface->pixels = NULL_POINTER;
        surface->capacity = 0;
        surface->x = 0;
        surface->y = 0;
        surface->w = (int)viewport_w;
        surface->h = (int)viewport_h;
        return;
    }
    if (surface->pixels == NULL_POINTER || surface->w != viewport_w || surface->h != viewport_h)
    {
        GUI_FREE(surface->pixels);
        surface->pixels = GUI_CALLOC_T(viewport_w * viewport_h, 4, MM_TYPE__GENERIC);
        surface->w = (int)viewport_w;
        surface->h = (int)viewport_h;
    }
    if (surface->pixels == NULL_POINTER)
    {
        _compositor_internal__enabled = FALSE;
        return;
    }
    stdlib__null_memory(surface->pixels, (int64)surface->w * surface->h * 4);
    bounds.w = (float)viewport_w;
    bounds.h = (float)viewport_h;
    _compositor_internal__rect(bounds, clear, NULL_POINTER, 1.0f, 1.0f, 0.0f);
}

/**
 * @brief present a completed effect surface through the selected backend
 * @return void
 */
void renderer__end_frame(void)
{
    _compositor_internal__surface_t* surface = &_compositor_internal__groups[0].surface;
    void* texture = NULL_POINTER;
    gui_rect_t rect;
    gui_color_t tint;
    int64 i = 0;
    int64 count = (int64)surface->w * surface->h;
    float a = 0.0f;

    stdlib__null_memory(&rect, sizeof(rect));
    stdlib__null_memory(&tint, sizeof(tint));
    renderer_debug__draw();
    if (_compositor_internal__enabled == TRUE && surface->pixels != NULL_POINTER)
    {
        for (i = 0; i < count; i++)
        {
            a = surface->pixels[i * 4 + 3] / 255.0f;
            if (a > 0.0f && a < 1.0f)
            {
                surface->pixels[i * 4] = (ubyte)stdlib__fminf(255.0f, surface->pixels[i * 4] / a + 0.5f);
                surface->pixels[i * 4 + 1] = (ubyte)stdlib__fminf(255.0f, surface->pixels[i * 4 + 1] / a + 0.5f);
                surface->pixels[i * 4 + 2] = (ubyte)stdlib__fminf(255.0f, surface->pixels[i * 4 + 2] / a + 0.5f);
            }
        }
        texture = renderer_backend__create_frame_texture_rgba(surface->pixels, surface->w, surface->h);
        if (texture != NULL_POINTER)
        {
            rect.w = (float)surface->w;
            rect.h = (float)surface->h;
            tint.r = 1.0f;
            tint.g = 1.0f;
            tint.b = 1.0f;
            tint.a = 1.0f;
            renderer_backend__submit_image(rect, texture, tint);
            renderer_backend__flush_pending_draws();
        }
    }
    renderer_backend__end_frame();
    renderer_debug__frame_finished();
    if (_compositor_internal__layer_mode == TRUE)
    {
        _compositor_internal__stats.frames++;
        _compositor_internal__layer_textures_age(FALSE);
    }
    // D3D12 records draws until end_frame submits the command list. Destroy
    // only after submission, so its fence wait covers the texture's last use.
    if (texture != NULL_POINTER)
    {
        renderer_backend__destroy_texture(texture);
    }
}

/**
 * @brief release compositor resources before the GPU backend shuts down
 * @return void
 */
void renderer__shutdown(void)
{
    int i = 0;

    // Standalone platforms already release this cache; embedded callers also
    // need it cleared while textures and the borrowed graphics context live.
    widget_image__cache_shutdown();
    effect_acceleration__shutdown();
    renderer_debug__reset_timing();

    while (_compositor_internal__textures != NULL_POINTER)
    {
        _compositor_internal__release(_compositor_internal__textures->handle);
    }
    for (i = 0; i < COMPOSITOR_STACK_CAP; i++)
    {
        GUI_FREE(_compositor_internal__groups[i].surface.pixels);
    }
    if (_compositor_internal__stats.frames > 0)
    {
        log_info("%s %llu %s %llu %s %llu %s %llu %s %llu %s %llu %s %llu %s %llu %s %llu %s %llu %s %llu", "layer mode: frames", (unsigned long long)_compositor_internal__stats.frames, "presents", (unsigned long long)_compositor_internal__stats.presents, "texture hits", (unsigned long long)_compositor_internal__stats.texture_hits, "uploads", (unsigned long long)_compositor_internal__stats.uploads, "upload MB", (unsigned long long)(_compositor_internal__stats.upload_bytes >> 20), "implicit rasters", (unsigned long long)_compositor_internal__stats.implicit_rasters, "implicit hits", (unsigned long long)_compositor_internal__stats.implicit_hits, "readbacks", (unsigned long long)_compositor_internal__stats.readbacks, "blur layers", (unsigned long long)_compositor_internal__stats.blur_layers, "transients", (unsigned long long)_compositor_internal__stats.transients, "scissor forwards", (unsigned long long)_compositor_internal__stats.scissor_forwards);
        log_info("%s %llu %s %llu", "layer mode: GPU groups", (unsigned long long)_compositor_internal__stats.gpu_groups, "tile draws", (unsigned long long)_compositor_internal__stats.tile_draws);
    }
    _compositor_internal__layer_textures_age(TRUE);
    GUI_FREE(_compositor_internal__layer_scratch.pixels);
    stdlib__null_memory(&_compositor_internal__layer_scratch, sizeof(_compositor_internal__layer_scratch));
    _compositor_internal__layer_mode = FALSE;
    GUI_FREE(_compositor_internal__blur_scratch);
    _compositor_internal__blur_scratch = NULL_POINTER;
    _compositor_internal__blur_capacity = 0;
    for (i = 0; i < COMPOSITOR_BACKGROUND_CACHE_CAP; i++)
    {
        GUI_FREE(_compositor_internal__background_cache[i].pixels);
    }
    stdlib__null_memory(_compositor_internal__background_cache, sizeof(_compositor_internal__background_cache));
    _compositor_internal__background_bytes = 0;
    for (i = 0; i < COMPOSITOR_DECORATION_CACHE_CAP; i++)
    {
        GUI_FREE(_compositor_internal__decorations[i].pixels);
    }
    stdlib__null_memory(_compositor_internal__decorations, sizeof(_compositor_internal__decorations));
    _compositor_internal__decoration_bytes = 0;
    for (i = 0; i < COMPOSITOR_COORDINATE_CACHE_CAP; i++)
    {
        GUI_FREE(_compositor_internal__coordinates[i].pixels);
    }
    stdlib__null_memory(_compositor_internal__coordinates, sizeof(_compositor_internal__coordinates));
    _compositor_internal__coordinate_bytes = 0;
    _compositor_internal__frame = 0;
    stdlib__null_memory(_compositor_internal__groups, sizeof(_compositor_internal__groups));
    _compositor_internal__enabled = FALSE;
    renderer_backend__shutdown();
}

/**
 * @brief flush immediate GPU draws; effect surface draws are already ordered
 * @return void
 */
void renderer__flush_pending_draws(void)
{
    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__flush_pending_draws();
    }
}

/**
 * @brief submit a rounded solid rectangle
 * @param gui_rect_t r -> bounds
 * @param gui_color_t c -> color
 * @param float radius -> corner radius
 * @return void
 */
void renderer__submit_rect(gui_rect_t r, gui_color_t c, float radius)
{
    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__submit_rect(_compositor_internal__gpu_rect(r), c, radius);
        return;
    }
    _compositor_internal__rect(r, c, NULL_POINTER, 1.0f, 1.0f, radius);
}

/**
 * @brief submit a CSS gradient
 * @param gui_rect_t r -> bounds
 * @param const gui_gradient_t* g -> gradient
 * @param float scale -> logical scale
 * @param float opacity -> alpha multiplier
 * @param float radius -> corner radius
 * @return void
 */
void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    gui_color_t unused;

    stdlib__null_memory(&unused, sizeof(unused));
    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__submit_gradient(_compositor_internal__gpu_rect(r), g, scale, opacity, radius);
        return;
    }
    _compositor_internal__rect(r, unused, g, scale, opacity, radius);
}

/**
 * @brief submit the legacy two-color gradient through the shared gradient sampler
 * @param gui_rect_t r -> bounds
 * @param gui_color_t from -> starting color
 * @param gui_color_t to -> ending color
 * @param int direction -> gradient direction
 * @param float radius -> corner radius
 * @return void
 */
void renderer__submit_rect_gradient(gui_rect_t r, gui_color_t from, gui_color_t to, int direction, float radius)
{
    gui_gradient_t gradient;

    stdlib__null_memory(&gradient, sizeof(gradient));
    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__submit_rect_gradient(_compositor_internal__gpu_rect(r), from, to, direction, radius);
        return;
    }
    gradient__two_color(&gradient, from, to, (gui_gradient_dir_e)direction);
    renderer__submit_gradient(r, &gradient, 1.0f, 1.0f, radius);
}

/**
 * @brief create a glyph atlas and retain its source coverage
 * @param const ubyte* pixels -> coverage bytes
 * @param int width -> atlas width
 * @param int height -> atlas height
 * @return void* -> backend atlas
 */
void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    void* handle = renderer_backend__create_atlas_r8(pixels, width, height);

    _compositor_internal__retain(handle, pixels, width, height, 1);
    return handle;
}

/**
 * @brief release a glyph atlas and its retained coverage
 * @param void* atlas -> backend atlas
 * @return void
 */
void renderer__destroy_atlas(void* atlas)
{
    _compositor_internal__release(atlas);
    renderer_backend__destroy_atlas(atlas);
}

/**
 * @brief select the current glyph atlas
 * @param void* atlas -> backend atlas
 * @return void
 */
void renderer__set_text_atlas(void* atlas)
{
    _compositor_internal__atlas = _compositor_internal__texture(atlas);
    if (_compositor_internal__painting() == FALSE || _compositor_internal__layer_mode == TRUE)
    {
        renderer_backend__set_text_atlas(atlas);
    }
}

/**
 * @brief submit a glyph with the existing font module's metrics and UVs
 * @param gui_rect_t rect -> glyph rectangle
 * @param gui_rect_t uv -> atlas UV rectangle
 * @param gui_color_t color -> text color
 * @return void
 */
void renderer__submit_text_glyph(gui_rect_t rect, gui_rect_t uv, gui_color_t color)
{
    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__submit_text_glyph(_compositor_internal__gpu_rect(rect), uv, color);
        return;
    }
    if (_compositor_internal__text_node != NULL_POINTER)
    {
        color.r = 1.0f;
        color.g = 1.0f;
        color.b = 1.0f;
        color.a = 1.0f;
    }
    _compositor_internal__image(rect, _compositor_internal__atlas, color, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h);
}

/**
 * @brief create an image texture and retain its source pixels
 * @param const ubyte* rgba -> straight RGBA bytes
 * @param int width -> width
 * @param int height -> height
 * @return void* -> backend texture
 */
void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height)
{
    void* handle = renderer_backend__create_texture_rgba(rgba, width, height);

    _compositor_internal__retain(handle, rgba, width, height, 4);
    return handle;
}

/**
 * @brief destroy a texture and its retained source
 * @param void* tex -> backend texture
 * @return void
 */
void renderer__destroy_texture(void* tex)
{
    _compositor_internal__release(tex);
    renderer_backend__destroy_texture(tex);
}

/**
 * @brief submit a complete texture
 * @param gui_rect_t rect -> destination
 * @param void* tex -> backend texture
 * @param gui_color_t tint -> RGBA multiplier
 * @return void
 */
void renderer__submit_image(gui_rect_t rect, void* tex, gui_color_t tint)
{
    renderer__submit_image_uv(rect, tex, tint, 0.0f, 0.0f, 1.0f, 1.0f);
}

/**
 * @brief submit an image with source UV coordinates
 * @param gui_rect_t rect -> destination
 * @param void* tex -> backend texture
 * @param gui_color_t tint -> multiplier
 * @param float u0 -> left UV
 * @param float v0 -> top UV
 * @param float u1 -> right UV
 * @param float v1 -> bottom UV
 * @return void
 */
void renderer__submit_image_uv(gui_rect_t rect, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__submit_image_uv(_compositor_internal__gpu_rect(rect), tex, tint, u0, v0, u1, v1);
        return;
    }
    _compositor_internal__image(rect, _compositor_internal__texture(tex), tint, u0, v0, u1, v1);
}

/**
 * @brief push an intersecting scissor
 * @param gui_rect_t rect -> clip rectangle
 * @return void
 */
void renderer__push_scissor(gui_rect_t rect)
{
    if (_compositor_internal__enabled == FALSE)
    {
        renderer_backend__push_scissor(rect);
        // recorded as well: the backend does not report its scissor, and a
        // backdrop blur of this frame is cut to it (_compositor_internal__blur_direct)
        if (_compositor_internal__clip_count < COMPOSITOR_STACK_CAP)
        {
            if (_compositor_internal__clip_count > 0)
            {
                rect = _compositor_internal__intersection(rect, _compositor_internal__clips[_compositor_internal__clip_count - 1]);
            }
            _compositor_internal__clips[_compositor_internal__clip_count++] = rect;
        }
        return;
    }
    // the record clips the CPU rasters, cuts group surfaces and bounds the
    // backdrop-blur regions in every mode; in layer mode a depth-0 scissor
    // also clips the backend's draws and the layer quads
    if (_compositor_internal__layer_mode == TRUE && (_compositor_internal__depth == 0 || _compositor_internal__gpu_group() == TRUE))
    {
        _compositor_internal__stats.scissor_forwards++;
        renderer_backend__push_scissor(_compositor_internal__gpu_rect(rect));
    }
    if (_compositor_internal__clip_count >= COMPOSITOR_STACK_CAP)
    {
        return;
    }
    if (_compositor_internal__clip_count > 0)
    {
        rect = _compositor_internal__intersection(rect, _compositor_internal__clips[_compositor_internal__clip_count - 1]);
    }
    _compositor_internal__clips[_compositor_internal__clip_count++] = rect;
}

/**
 * @brief restore the preceding scissor
 * @return void
 */
void renderer__pop_scissor(void)
{
    if (_compositor_internal__enabled == FALSE)
    {
        renderer_backend__pop_scissor();
        if (_compositor_internal__clip_count > 0)
        {
            _compositor_internal__clip_count--;
        }
        return;
    }
    if (_compositor_internal__layer_mode == TRUE && (_compositor_internal__depth == 0 || _compositor_internal__gpu_group() == TRUE))
    {
        renderer_backend__pop_scissor();
    }
    if (_compositor_internal__clip_count > 0)
    {
        _compositor_internal__clip_count--;
    }
}

/**
 * @brief split CSS lists outside balanced function arguments
 * @param const char* p -> remaining list
 * @param char* out -> next trimmed item
 * @param int cap -> item capacity
 * @param char delimiter -> comma or space
 * @return const char* -> next item, or the terminator
 */
static const char* _compositor_internal__part(const char* p, char* out, int cap, char delimiter)
{
    int depth = 0;
    int length = 0;
    char quote = 0;

    while (*p != 0 && (*p <= ' ' || *p == delimiter))
    {
        p++;
    }
    while (*p != 0)
    {
        if (depth == 0 && quote == 0 && (*p == delimiter || (delimiter == ' ' && *p <= ' ')))
        {
            break;
        }
        if (quote != 0)
        {
            if (*p == quote && (length == 0 || out[length - 1] != '\\'))
            {
                quote = 0;
            }
        }
        else if (*p == '"' || *p == '\'')
        {
            quote = *p;
        }
        else if (*p == '(')
        {
            depth++;
        }
        else if (*p == ')')
        {
            depth--;
        }
        if (length + 1 < cap)
        {
            out[length++] = *p;
        }
        p++;
    }
    while (length > 0 && out[length - 1] <= ' ')
    {
        length--;
    }
    out[length] = 0;
    if (*p != 0)
    {
        p++;
    }
    return p;
}

/**
 * @brief prepare layered gradient images, sizes, positions and repetition
 * @param gui_node_t* node -> styled node
 * @param const char* property -> background or mask image property
 * @param _compositor_internal__layer_t* layers -> destination layers
 * @param int cap -> layer capacity
 * @param float scale -> logical scale
 * @return int -> layer count in front-to-back order
 */
static int _compositor_internal__layers(gui_node_t* node, int property, _compositor_internal__layer_t* layers, int cap, float scale)
{
    char value[2048];
    char size[1024];
    char position[1024];
    char repeat[128];
    char item[2048];
    char token[1024];
    char sx[128];
    char sy[128];
    char px[128];
    char py[128];
    char sizes[COMPOSITOR_LAYER_CAP][128];
    char positions[COMPOSITOR_LAYER_CAP][128];
    const char* p = value;
    const char* rest = NULL_POINTER;
    const char* q = NULL_POINTER;
    gui_gradient_t gradient;
    boole mask = (boole)(property == MOTION_PROP__MASK || property == MOTION_PROP__MASK_IMAGE);
    int count = 0;
    int size_count = 0;
    int pos_count = 0;
    int i = 0;
    float reference_x = node->bounds.w;
    float reference_y = node->bounds.h;

    // written before every read (a terminating writer), so one terminator
    // replaces the full clear on this per-node path
    value[0] = 0;
    size[0] = 0;
    position[0] = 0;
    repeat[0] = 0;
    item[0] = 0;
    token[0] = 0;
    sx[0] = 0;
    sy[0] = 0;
    px[0] = 0;
    py[0] = 0;
    sizes[0][0] = 0;
    positions[0][0] = 0;
    stdlib__null_memory(&gradient, sizeof(gradient));
    if (motion__value_id(node, property, value, sizeof(value)) == FALSE)
    {
        return 0;
    }
    motion__value_id(node, (mask == TRUE) ? MOTION_PROP__MASK_SIZE : MOTION_PROP__BACKGROUND_SIZE, size, sizeof(size));
    motion__value_id(node, (mask == TRUE) ? MOTION_PROP__MASK_POSITION : MOTION_PROP__BACKGROUND_POSITION, position, sizeof(position));
    motion__value_id(node, (mask == TRUE) ? MOTION_PROP__MASK_REPEAT : MOTION_PROP__BACKGROUND_REPEAT, repeat, sizeof(repeat));
    q = size;
    while (*q != 0 && size_count < cap)
    {
        q = _compositor_internal__part(q, sizes[size_count++], sizeof(sizes[0]), ',');
    }
    q = position;
    while (*q != 0 && pos_count < cap)
    {
        q = _compositor_internal__part(q, positions[pos_count++], sizeof(positions[0]), ',');
    }
    while (*p != 0 && count < cap)
    {
        p = _compositor_internal__part(p, item, sizeof(item), ',');
        rest = _compositor_internal__part(item, token, sizeof(token), ' ');
        stdlib__null_memory(&layers[count], sizeof(layers[count]));
        layers[count].w = reference_x;
        layers[count].h = reference_y;
        layers[count].repeat_x = (boole)(stdlib__strstr(repeat, "no-repeat") == NULL_POINTER && stdlib__strcmp(repeat, "repeat-y") != 0);
        layers[count].repeat_y = (boole)(stdlib__strstr(repeat, "no-repeat") == NULL_POINTER && stdlib__strcmp(repeat, "repeat-x") != 0);
        if (size_count > 0)
        {
            q = _compositor_internal__part(sizes[count % size_count], sx, sizeof(sx), ' ');
            _compositor_internal__part(q, sy, sizeof(sy), ' ');
            if (sx[0] != 0 && stdlib__strcmp(sx, "auto") != 0)
            {
                layers[count].w = motion__number(sx, reference_x / scale) * scale;
            }
            if (sy[0] != 0 && stdlib__strcmp(sy, "auto") != 0)
            {
                layers[count].h = motion__number(sy, reference_y / scale) * scale;
            }
        }
        layers[count].w = stdlib__fmaxf(0.01f, layers[count].w);
        layers[count].h = stdlib__fmaxf(0.01f, layers[count].h);
        if (pos_count > 0)
        {
            q = _compositor_internal__part(positions[count % pos_count], px, sizeof(px), ' ');
            _compositor_internal__part(q, py, sizeof(py), ' ');
            if (py[0] == 0)
            {
                stdlib__strcpy_safe(py, sizeof(py), "50%");
            }
            layers[count].x = (stdlib__strcmp(px, "center") == 0) ? (reference_x - layers[count].w) * 0.5f : ((stdlib__strcmp(px, "right") == 0) ? reference_x - layers[count].w : motion__number(px, (reference_x - layers[count].w) / scale) * scale);
            layers[count].y = (stdlib__strcmp(py, "center") == 0) ? (reference_y - layers[count].h) * 0.5f : ((stdlib__strcmp(py, "bottom") == 0) ? reference_y - layers[count].h : motion__number(py, (reference_y - layers[count].h) / scale) * scale);
        }
        if (parser_style__parse_gradient(token, &gradient) == TRUE)
        {
            layers[count].is_gradient = TRUE;
            layers[count].source = gradient;
            gradient__prepare(&gradient, layers[count].w, layers[count].h, scale, &layers[count].gradient);
        }
        else
        {
            parser_style__parse_color(token, &layers[count].color);
        }
        if (mask == TRUE && stdlib__strstr(rest, "content-box") != NULL_POINTER)
        {
            layers[count].x += node->resolved.pad_l * scale;
            layers[count].y += node->resolved.pad_t * scale;
            layers[count].w -= (node->resolved.pad_l + node->resolved.pad_r) * scale;
            layers[count].h -= (node->resolved.pad_t + node->resolved.pad_b) * scale;
            layers[count].repeat_x = FALSE;
            layers[count].repeat_y = FALSE;
        }
        count++;
    }
    for (i = count; i < cap; i++)
    {
        stdlib__null_memory(&layers[i], sizeof(layers[i]));
    }
    return count;
}

/**
 * @brief sample a repeated or positioned background layer
 * @param const _compositor_internal__layer_t* layer -> prepared image
 * @param float x -> local x
 * @param float y -> local y
 * @return gui_color_t -> straight RGBA sample
 */
static gui_color_t _compositor_internal__layer_sample(const _compositor_internal__layer_t* layer, float x, float y)
{
    gui_color_t transparent = {0.0f, 0.0f, 0.0f, 0.0f};
    x -= layer->x;
    y -= layer->y;
    if (layer->w <= 0.0f || layer->h <= 0.0f)
    {
        return transparent;
    }
    if (layer->repeat_x == TRUE)
    {
        x -= stdlib__floorf(x / layer->w) * layer->w;
    }
    if (layer->repeat_y == TRUE)
    {
        y -= stdlib__floorf(y / layer->h) * layer->h;
    }
    if (x < 0.0f || y < 0.0f || x >= layer->w || y >= layer->h)
    {
        return transparent;
    }
    return (layer->is_gradient == TRUE) ? gradient__sample(&layer->gradient, x, y) : layer->color;
}

/**
 * @brief combine two straight colors using source-over
 * @param gui_color_t front -> source
 * @param gui_color_t back -> destination
 * @return gui_color_t -> composited straight RGBA
 */
static gui_color_t _compositor_internal__over(gui_color_t front, gui_color_t back)
{
    gui_color_t result = {0.0f, 0.0f, 0.0f, 0.0f};
    float rest = back.a * (1.0f - front.a);

    if (front.a == 1.0f)
    {
        return front;
    }
    if (front.a == 0.0f)
    {
        return back;
    }
    result.a = front.a + rest;
    if (result.a > 0.0f)
    {
        result.r = (front.r * front.a + back.r * rest) / result.a;
        result.g = (front.g * front.a + back.g * rest) / result.a;
        result.b = (front.b * front.a + back.b * rest) / result.a;
    }
    return result;
}

/**
 * @brief retain exact background samples after the same paint inputs occur twice
 * @param gui_node_t* node -> owner identity, without retaining or dereferencing it later
 * @param const _compositor_internal__background_key_t* key -> fully resolved paint and sample geometry
 * @return _compositor_internal__background_cache_t* -> readable samples when valid, otherwise storage to fill, or NULL
 */
static _compositor_internal__background_cache_t* _compositor_internal__cached_background(gui_node_t* node, const _compositor_internal__background_key_t* key, boole create)
{
    _compositor_internal__background_cache_t* entry = NULL_POINTER;
    _compositor_internal__background_cache_t* oldest = NULL_POINTER;
    int64 bytes = (int64)key->w * key->h * sizeof(gui_color_t);
    int i = 0;

    if (bytes <= 0 || bytes > MEEK_UI_CPU_BACKGROUND_CACHE_BYTES)
    {
        return NULL_POINTER;
    }
    oldest = &_compositor_internal__background_cache[0];
    for (i = 0; i < COMPOSITOR_BACKGROUND_CACHE_CAP; i++)
    {
        if (_compositor_internal__background_cache[i].owner == node)
        {
            entry = &_compositor_internal__background_cache[i];
            break;
        }
        if (_compositor_internal__background_cache[i].used < oldest->used)
        {
            oldest = &_compositor_internal__background_cache[i];
        }
    }
    // A speculative fused draw must not admit/evict CPU color caches. In
    // particular, a fallback in the same frame must not count as key reuse.
    if (create == FALSE)
    {
        return (entry != NULL_POINTER && entry->valid && stdlib__memcmp(&entry->key, key, sizeof(*key)) == 0) ? entry : NULL_POINTER;
    }
    if (entry == NULL_POINTER)
    {
        entry = oldest;
        entry->owner = node;
        entry->valid = FALSE;
        // An evicted entry must observe its new key once before allocating.
        entry->used = 0;
    }
    if (entry->used == 0 || stdlib__memcmp(&entry->key, key, sizeof(*key)) != 0)
    {
        _compositor_internal__background_bytes -= entry->bytes;
        GUI_FREE(entry->pixels);
        entry->pixels = NULL_POINTER;
        entry->bytes = 0;
        entry->valid = FALSE;
        entry->key = *key;
        entry->used = _compositor_internal__frame;
        return NULL_POINTER;
    }
    entry->used = _compositor_internal__frame;
    if (entry->pixels != NULL_POINTER)
    {
        return entry;
    }
    while (_compositor_internal__background_bytes + bytes > MEEK_UI_CPU_BACKGROUND_CACHE_BYTES)
    {
        oldest = NULL_POINTER;
        for (i = 0; i < COMPOSITOR_BACKGROUND_CACHE_CAP; i++)
        {
            if (&_compositor_internal__background_cache[i] != entry && _compositor_internal__background_cache[i].pixels != NULL_POINTER && _compositor_internal__background_cache[i].used + 1 < _compositor_internal__frame && (oldest == NULL_POINTER || _compositor_internal__background_cache[i].used < oldest->used))
            {
                oldest = &_compositor_internal__background_cache[i];
            }
        }
        if (oldest == NULL_POINTER)
        {
            // Do not evict backgrounds used this frame or the last one just to admit another.
            // A page larger than the budget otherwise cycles through every allocation each frame.
            return NULL_POINTER;
        }
        _compositor_internal__background_bytes -= oldest->bytes;
        GUI_FREE(oldest->pixels);
        oldest->pixels = NULL_POINTER;
        oldest->bytes = 0;
        oldest->valid = FALSE;
    }
    entry->pixels = GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
    if (entry->pixels == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    entry->bytes = bytes;
    _compositor_internal__background_bytes += bytes;
    return entry;
}

/**
 * @brief retain a conic layer's geometry separately from its changing color ramp
 * @param const _compositor_internal__background_key_t* background -> sample rectangle and prepared layers
 * @param int layer -> conic layer index
 * @return _compositor_internal__coordinate_cache_t* -> retained angular coordinates or NULL
 */
static _compositor_internal__coordinate_cache_t* _compositor_internal__cached_coordinates(const _compositor_internal__background_key_t* background, int layer, boole create)
{
    _compositor_internal__coordinate_key_t key;
    _compositor_internal__coordinate_cache_t* entry = NULL_POINTER;
    _compositor_internal__coordinate_cache_t* oldest = NULL_POINTER;
    int64 bytes = (int64)background->w * background->h * sizeof(float);
    float cx = background->layers[layer].gradient.cx;
    float cy = background->layers[layer].gradient.cy;
    int i = 0;

    if (bytes <= 0 || bytes > (int64)MEEK_UI_CPU_COORDINATE_CACHE_BYTES)
    {
        return NULL_POINTER;
    }
    stdlib__null_memory(&key, sizeof(key));
    key.x = background->x;
    key.y = background->y;
    key.left = background->left;
    key.top = background->top;
    key.w = background->w;
    key.h = background->h;
    key.layer = background->layers[layer];
    stdlib__null_memory(&key.layer.gradient, sizeof(key.layer.gradient));
    stdlib__null_memory(&key.layer.color, sizeof(key.layer.color));
    key.layer.gradient.cx = cx;
    key.layer.gradient.cy = cy;
    for (i = 0; i < COMPOSITOR_COORDINATE_CACHE_CAP; i++)
    {
        entry = &_compositor_internal__coordinates[i];
        if (entry->pixels != NULL_POINTER && stdlib__memcmp(&entry->key, &key, sizeof(key)) == 0)
        {
            entry->used = _compositor_internal__frame;
            return entry;
        }
        if (oldest == NULL_POINTER || entry->used < oldest->used)
        {
            oldest = entry;
        }
    }
    if (create == FALSE) return NULL_POINTER;
    entry = oldest;
    if (entry->pixels != NULL_POINTER && entry->used + 1 >= _compositor_internal__frame)
    {
        return NULL_POINTER;
    }
    GUI_FREE(entry->pixels);
    _compositor_internal__coordinate_bytes -= entry->bytes;
    stdlib__null_memory(entry, sizeof(*entry));
    while (_compositor_internal__coordinate_bytes + bytes > (int64)MEEK_UI_CPU_COORDINATE_CACHE_BYTES)
    {
        oldest = NULL_POINTER;
        for (i = 0; i < COMPOSITOR_COORDINATE_CACHE_CAP; i++)
        {
            if (_compositor_internal__coordinates[i].pixels != NULL_POINTER && _compositor_internal__coordinates[i].used + 1 < _compositor_internal__frame && (oldest == NULL_POINTER || _compositor_internal__coordinates[i].used < oldest->used))
            {
                oldest = &_compositor_internal__coordinates[i];
            }
        }
        if (oldest == NULL_POINTER)
        {
            return NULL_POINTER;
        }
        GUI_FREE(oldest->pixels);
        _compositor_internal__coordinate_bytes -= oldest->bytes;
        stdlib__null_memory(oldest, sizeof(*oldest));
    }
    entry->pixels = GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
    if (entry->pixels == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    entry->bytes = bytes;
    entry->key = key;
    entry->used = _compositor_internal__frame;
    _compositor_internal__coordinate_bytes += bytes;
    return entry;
}

/**
 * @brief evaluate a conic layer's angle independently of its animated rotation
 * @param const _compositor_internal__layer_t* layer -> prepared conic layer
 * @param float x -> local pixel center
 * @param float y -> local pixel center
 * @return float -> angle in turns (-0.5..0.5), or 2 for a transparent tile exterior
 */
static float _compositor_internal__conic_coordinate(const _compositor_internal__layer_t* layer, float x, float y)
{
    x -= layer->x;
    y -= layer->y;
    if (layer->w <= 0.0f || layer->h <= 0.0f)
    {
        return 2.0f;
    }
    if (layer->repeat_x == TRUE)
    {
        x -= stdlib__floorf(x / layer->w) * layer->w;
    }
    if (layer->repeat_y == TRUE)
    {
        y -= stdlib__floorf(y / layer->h) * layer->h;
    }
    if (x < 0.0f || y < 0.0f || x >= layer->w || y >= layer->h)
    {
        return 2.0f;
    }
    return stdlib__atan2f(x - layer->gradient.cx, -(y - layer->gradient.cy)) / (2.0f * 3.14159265358979323846f);
}

// Shared numbering for portable composition and the optional compute shaders.
static int _compositor_internal__blend_mode(gui_node_t* node)
{
    char blend[64] = {0};
    motion__value_id(node, MOTION_PROP__MIX_BLEND_MODE, blend, sizeof(blend));
    if (stdlib__strcmp(blend, "screen") == 0) return 1;
    if (stdlib__strcmp(blend, "multiply") == 0) return 2;
    if (stdlib__strcmp(blend, "overlay") == 0) return 3;
    if (stdlib__strcmp(blend, "difference") == 0) return 4;
    return 0;
}

// fuse may only be used for an otherwise empty, eligible leaf group. A FALSE
// return from that speculative path never paints or changes the parent pixels.
static boole _compositor_internal__background(gui_node_t* node, float scale, boole fuse)
{
    _compositor_internal__layer_t layers[COMPOSITOR_LAYER_CAP];
    gui_rect_t clip;
    gui_color_t color;
    char text_clip[64];
    int count = 0;
    int x = 0;
    int y = 0;
    int i = 0;
    int right = 0;
    int bottom = 0;
    float coverage = 0.0f;
    _compositor_internal__background_key_t key;
    _compositor_internal__background_cache_t* cache = NULL_POINTER;
    int64 index = 0;
    boole uniform = TRUE;
    _compositor_internal__coordinate_cache_t* coordinates[COMPOSITOR_LAYER_CAP];
    const float* gpu_coordinates[COMPOSITOR_LAYER_CAP] = {0};
    gui_color_t sample;
    float parameter = 0.0f;
    float inverse = 0.0f;
    float alpha = 0.0f;
    ubyte* pixel = NULL_POINTER;
    int stop = 0;
#if defined(COMPOSITOR_X86_SIMD)
    __m128 source;
#endif

    if (_compositor_internal__enabled == FALSE)
    {
        return FALSE;
    }
    clip = _compositor_internal__clip(node->bounds);
    if (clip.w <= 0.0f || clip.h <= 0.0f)
    {
        return (boole)!fuse;
    }
    // the parser fills and nulls the layer array; text_clip is written by motion__value_id
    stdlib__null_memory(&color, sizeof(color));
    text_clip[0] = 0;
    stdlib__null_memory(&key, sizeof(key));
    stdlib__null_memory(coordinates, sizeof(coordinates));
    stdlib__null_memory(&sample, sizeof(sample));
    motion__value_id(node, MOTION_PROP__BACKGROUND_CLIP, text_clip, sizeof(text_clip));
    if (text_clip[0] == 0)
    {
        motion__value_id(node, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, text_clip, sizeof(text_clip));
    }
    if (stdlib__strcmp(text_clip, "text") == 0)
    {
        return (boole)!fuse;
    }
    count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND_IMAGE, layers, COMPOSITOR_LAYER_CAP, scale);
    if (count == 0)
    {
        count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND, layers, COMPOSITOR_LAYER_CAP, scale);
    }
    // Raster background images are drawn by scene__emit_default_bg after this
    // gradient/color path. Reporting an empty background as handled here must
    // not suppress that image when the node has no background color.
    if (count == 0 && !node->resolved.has_background_image &&
        (node->resolved.has_background_color == FALSE || node->resolved.background_color.a <= 0.0f))
    {
        return (boole)!fuse;
    }
    if (count == 0)
    {
        return FALSE;
    }
    for (i = 0; i < count; i++)
    {
        if (layers[i].is_gradient == TRUE || layers[i].repeat_x == FALSE || layers[i].repeat_y == FALSE)
        {
            uniform = FALSE;
            break;
        }
    }
    if (uniform == TRUE)
    {
        if (fuse) return FALSE;
        if (node->resolved.has_background_color == TRUE)
        {
            color = node->resolved.background_color;
        }
        for (i = count - 1; i >= 0; i--)
        {
            color = _compositor_internal__over(layers[i].color, color);
        }
        _compositor_internal__rect(node->bounds, color, NULL_POINTER, scale, node->effective_opacity, node->resolved.radius * scale);
        return TRUE;
    }
    right = (int)stdlib__ceilf(clip.x + clip.w);
    bottom = (int)stdlib__ceilf(clip.y + clip.h);
    _compositor_internal__touch();
    // Keep backgrounds premultiplied from the ramp through source-over.
    // Straight-alpha conversion is unnecessary between every pair of layers.
    for (i = 0; i < count; i++)
    {
        layers[i].gradient.premultiplied = TRUE;
        for (stop = 0; stop < layers[i].gradient.count; stop++)
        {
            layers[i].gradient.color[stop].r *= layers[i].gradient.color[stop].a;
            layers[i].gradient.color[stop].g *= layers[i].gradient.color[stop].a;
            layers[i].gradient.color[stop].b *= layers[i].gradient.color[stop].a;
        }
        layers[i].color.r *= layers[i].color.a;
        layers[i].color.g *= layers[i].color.a;
        layers[i].color.b *= layers[i].color.a;
    }
    stdlib__memcpy(key.layers, layers, sizeof(layers));
    key.count = count;
    key.x = node->bounds.x;
    key.y = node->bounds.y;
    key.left = (int)stdlib__floorf(clip.x);
    key.top = (int)stdlib__floorf(clip.y);
    key.w = right - key.left;
    key.h = bottom - key.top;
    if (node->resolved.has_background_color == TRUE)
    {
        key.base = node->resolved.background_color;
        key.base.r *= key.base.a;
        key.base.g *= key.base.a;
        key.base.b *= key.base.a;
    }
    cache = _compositor_internal__cached_background(node, &key, (boole)(fuse == FALSE && _compositor_internal__no_float_cache == FALSE));
    if (cache == NULL_POINTER || cache->valid == FALSE)
    {
        for (i = 0; i < count; i++)
        {
            if (layers[i].is_gradient == TRUE && layers[i].gradient.kind == GUI_GRADIENT_KIND__CONIC)
            {
                coordinates[i] = _compositor_internal__cached_coordinates(&key, i, (boole)!fuse);
                if (coordinates[i] != NULL_POINTER && coordinates[i]->valid == TRUE)
                {
                    gpu_coordinates[i] = coordinates[i]->pixels;
                }
            }
        }
        {
            _compositor_internal__surface_t* surface = &_compositor_internal__groups[_compositor_internal__depth].surface;
            effect_surface_t output = {surface->pixels, surface->x, surface->y, surface->w, surface->h, 0};
            if (fuse)
            {
                _compositor_internal__group_t* group = &_compositor_internal__groups[_compositor_internal__depth];
                _compositor_internal__surface_t* parent = &_compositor_internal__groups[_compositor_internal__depth - 1].surface;
                int left = key.left, top = key.top;
                int end_x = key.left + key.w, end_y = key.top + key.h;
                // begin_node temporarily replaces the ancestor scissor. Keep
                // its saved bounds, the leaf scissor and the parent surface.
                if (group->parent_clipped)
                {
                    left = (int)stdlib__fmaxf((float)left, stdlib__floorf(group->clip.x));
                    top = (int)stdlib__fmaxf((float)top, stdlib__floorf(group->clip.y));
                    end_x = (int)stdlib__fminf((float)end_x, stdlib__ceilf(group->clip.x + group->clip.w));
                    end_y = (int)stdlib__fminf((float)end_y, stdlib__ceilf(group->clip.y + group->clip.h));
                }
                left = (int)stdlib__fmaxf((float)left, (float)parent->x);
                top = (int)stdlib__fmaxf((float)top, (float)parent->y);
                end_x = (int)stdlib__fminf((float)end_x, (float)(parent->x + parent->w));
                end_y = (int)stdlib__fminf((float)end_y, (float)(parent->y + parent->h));
                if (end_x <= left || end_y <= top) return FALSE;
                for (i = 0; i < count; i++)
                    if (gpu_coordinates[i]) gpu_coordinates[i] += (int64)(top - key.top) * key.w + left - key.left;
                output = (effect_surface_t){parent->pixels, parent->x, parent->y, parent->w, parent->h, 0};
                return effect_acceleration__background_compose(&output, left, top, end_x - left, end_y - top,
                    node->bounds, node->resolved.radius * scale, group->opacity, key.base, layers, count,
                    gpu_coordinates, key.w, _compositor_internal__blend_mode(node));
            }
            // a top group's background still takes the compute detour when
            // enabled: the readback plus the layer upload (about 5 ms for the
            // hero sweep) cost far less than its CPU raster (about 55 ms)
            if (_compositor_internal__no_acceleration == FALSE && effect_acceleration__background(&output, key.left, key.top, key.w, key.h, node->bounds,
                node->resolved.radius * scale, node->effective_opacity, key.base, layers, count, gpu_coordinates) == TRUE)
            {
                // Compute wrote the destination only; CPU color/coordinate caches
                // must not be marked valid without their own samples being filled.
                return TRUE;
            }
        }
    }
    if (fuse) return FALSE;
    for (y = (int)stdlib__floorf(clip.y); y < bottom; y++)
    {
        for (x = (int)stdlib__floorf(clip.x); x < right; x++)
        {
            index = (int64)(y - key.top) * key.w + x - key.left;
            if (cache != NULL_POINTER && cache->valid == TRUE)
            {
                color = cache->pixels[index];
            }
            else
            {
                color = key.base;
                for (i = count - 1; i >= 0; i--)
                {
                    if (coordinates[i] != NULL_POINTER)
                    {
                        if (coordinates[i]->valid == TRUE)
                        {
                            parameter = coordinates[i]->pixels[index];
                        }
                        else
                        {
                            parameter = _compositor_internal__conic_coordinate(&layers[i], x + 0.5f - node->bounds.x, y + 0.5f - node->bounds.y);
                            coordinates[i]->pixels[index] = parameter;
                        }
                        if (parameter == 2.0f)
                        {
                            continue;
                        }
                        parameter -= layers[i].gradient.angle;
                        parameter -= stdlib__floorf(parameter);
                        sample = _gradient_internal__sample_parameter(&layers[i].gradient, parameter);
                    }
                    else
                    {
                        sample = _compositor_internal__layer_sample(&layers[i], x + 0.5f - node->bounds.x, y + 0.5f - node->bounds.y);
                    }
                    inverse = 1.0f - sample.a;
                    color.r = sample.r + color.r * inverse;
                    color.g = sample.g + color.g * inverse;
                    color.b = sample.b + color.b * inverse;
                    color.a = sample.a + color.a * inverse;
                }
                if (cache != NULL_POINTER)
                {
                    cache->pixels[index] = color;
                }
            }
            coverage = _compositor_internal__rounded(x + 0.5f - node->bounds.x, y + 0.5f - node->bounds.y, node->bounds.w, node->bounds.h, node->resolved.radius * scale);
            coverage *= node->effective_opacity;
            alpha = color.a * coverage;
            if (alpha > 0.0f)
            {
                inverse = 1.0f - alpha;
                pixel = _compositor_internal__groups[_compositor_internal__depth].surface.pixels + ((int64)(y - _compositor_internal__groups[_compositor_internal__depth].surface.y) * _compositor_internal__groups[_compositor_internal__depth].surface.w + x - _compositor_internal__groups[_compositor_internal__depth].surface.x) * 4;
#if defined(COMPOSITOR_X86_SIMD)
                if (_compositor_internal__sse2 == TRUE)
                {
                    // every lane is min(c, a) * coverage * 255 + p * inverse + 0.5:
                    // for the alpha lane min(a, a) * coverage is the scalar alpha
                    source = _mm_mul_ps(_mm_mul_ps(_mm_min_ps(_mm_set_ps(color.a, color.b, color.g, color.r), _mm_set1_ps(color.a)), _mm_set1_ps(coverage)), _mm_set1_ps(255.0f));
                    _compositor_internal__store_rgba(pixel, _mm_add_ps(_mm_add_ps(source, _mm_mul_ps(_compositor_internal__rgba_float(pixel), _mm_set1_ps(inverse))), _mm_set1_ps(0.5f)));
                    continue;
                }
#endif
                pixel[0] = (ubyte)(stdlib__fminf(color.r, color.a) * coverage * 255.0f + pixel[0] * inverse + 0.5f);
                pixel[1] = (ubyte)(stdlib__fminf(color.g, color.a) * coverage * 255.0f + pixel[1] * inverse + 0.5f);
                pixel[2] = (ubyte)(stdlib__fminf(color.b, color.a) * coverage * 255.0f + pixel[2] * inverse + 0.5f);
                pixel[3] = (ubyte)(alpha * 255.0f + pixel[3] * inverse + 0.5f);
            }
        }
    }
    if (cache != NULL_POINTER)
    {
        cache->valid = TRUE;
    }
    for (i = 0; i < count; i++)
    {
        if (coordinates[i] != NULL_POINTER)
        {
            coordinates[i]->valid = TRUE;
        }
    }
    return TRUE;
}

boole compositor__background(gui_node_t* node, float scale)
{
    if (_compositor_internal__layer_mode == TRUE && _compositor_internal__depth == 0)
    {
        return _compositor_internal__background_layer(node, scale);
    }
    if (_compositor_internal__gpu_group() == TRUE)
    {
        return _compositor_internal__background_gpu(node, scale);
    }
    return _compositor_internal__background(node, scale, FALSE);
}

/**
 * @brief the radius of one box pass: three passes of width 2 * radius + 1 have the variance of a Gaussian with this sigma
 * @param float sigma -> standard deviation in pixels
 * @return int -> 1 to 256; the CPU kernel and the backend's blur are given the same value
 */
static int _compositor_internal__blur_radius(float sigma)
{
    return (int)stdlib__fminf(256.0f, stdlib__fmaxf(1.0f, stdlib__sqrtf(sigma * sigma * 4.0f + 1.0f) * 0.5f - 0.5f));
}

/**
 * @brief approximate a Gaussian with three separable box passes in premultiplied space
 * @param _compositor_internal__surface_t* surface -> pixels to blur
 * @param float sigma -> standard deviation in pixels
 * @return void
 */
static void _compositor_internal__blur(_compositor_internal__surface_t* surface, float sigma)
{
    ubyte* scratch = NULL_POINTER;
    ubyte* source = NULL_POINTER;
    ubyte* target = NULL_POINTER;
    int sum[4];
    int columns[64 * 4];
    int tile = 0;
    int channels = 0;
    int pass = 0;
    int axis = 0;
    int line = 0;
    int step = 0;
    int k = 0;
    int c = 0;
    int lines = 0;
    int length = 0;
    int radius = 0;
    int width = 0;
    int index = 0;
    int remove = 0;
    int add = 0;
    int64 stride = 0;
    int64 base = 0;
    uint64 reciprocal = 0;
    int64 bytes = (int64)surface->w * surface->h * 4;

    stdlib__null_memory(sum, sizeof(sum));
    if (surface->pixels == NULL_POINTER || sigma <= 0.1f || surface->w <= 0 || surface->h <= 0)
    {
        return;
    }
    radius = _compositor_internal__blur_radius(sigma);
    width = radius * 2 + 1;
    // The rounded numerator is at most 131071 and width is at most 513.
    // The upward-rounded 32-bit reciprocal gives the exact integer quotient in this range.
    reciprocal = stdlib__udiv64(((uint64)1 << 32) + width - 1, width);
    if (bytes > _compositor_internal__blur_capacity)
    {
        scratch = GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
        if (scratch == NULL_POINTER)
        {
            return;
        }
        GUI_FREE(_compositor_internal__blur_scratch);
        _compositor_internal__blur_scratch = scratch;
        _compositor_internal__blur_capacity = bytes;
    }
    scratch = _compositor_internal__blur_scratch;
    if (scratch == NULL_POINTER)
    {
        return;
    }
    for (pass = 0; pass < 3; pass++)
    {
        for (axis = 0; axis < 2; axis++)
        {
            source = (axis == 0) ? surface->pixels : scratch;
            target = (axis == 0) ? scratch : surface->pixels;
            lines = (axis == 0) ? surface->h : surface->w;
            length = (axis == 0) ? surface->w : surface->h;
            stride = (axis == 0) ? 4 : surface->w * 4;
            if (axis == 1)
            {
                // Sweep short contiguous spans instead of striding a whole
                // image column. Three active rows and the running sums stay
                // near each other in cache, and channels can be vectorized.
                for (tile = 0; tile < surface->w; tile += 64)
                {
                    channels = ((surface->w - tile < 64) ? surface->w - tile : 64) * 4;
                    stdlib__null_memory(columns, sizeof(columns));
                    for (k = -radius; k <= radius; k++)
                    {
                        index = (k < 0) ? 0 : ((k >= length) ? length - 1 : k);
                        base = (int64)index * stride + tile * 4;
                        for (c = 0; c < channels; c++)
                        {
                            columns[c] += source[base + c];
                        }
                    }
                    for (step = 0; step < length; step++)
                    {
                        remove = (step - radius < 0) ? 0 : step - radius;
                        add = (step + radius + 1 >= length) ? length - 1 : step + radius + 1;
                        base = (int64)step * stride + tile * 4;
                        for (c = 0; c < channels; c++)
                        {
                            target[base + c] = (ubyte)(((uint64)(columns[c] + width / 2) * reciprocal) >> 32);
                            columns[c] += source[(int64)add * stride + tile * 4 + c] - source[(int64)remove * stride + tile * 4 + c];
                        }
                    }
                }
                continue;
            }
            for (line = 0; line < lines; line++)
            {
                base = (axis == 0) ? (int64)line * surface->w * 4 : line * 4;
                stdlib__null_memory(sum, sizeof(sum));
                for (k = -radius; k <= radius; k++)
                {
                    index = (k < 0) ? 0 : ((k >= length) ? length - 1 : k);
                    for (c = 0; c < 4; c++)
                    {
                        sum[c] += source[base + index * stride + c];
                    }
                }
                for (step = 0; step < length; step++)
                {
                    remove = (step - radius < 0) ? 0 : step - radius;
                    add = (step + radius + 1 >= length) ? length - 1 : step + radius + 1;
                    for (c = 0; c < 4; c++)
                    {
                        target[base + step * stride + c] = (ubyte)(((uint64)(sum[c] + width / 2) * reciprocal) >> 32);
                        sum[c] += source[base + add * stride + c] - source[base + remove * stride + c];
                    }
                }
            }
        }
    }
}

/**
 * @brief blur pixels already behind a decorated node
 * @param gui_rect_t rect -> backdrop bounds
 * @param float sigma_px -> blur deviation
 * @return void
 */
void renderer__blur_region(gui_rect_t rect, float sigma_px)
{
    _compositor_internal__surface_t temporary;
    _compositor_internal__surface_t* source = &_compositor_internal__groups[_compositor_internal__depth].surface;
    gui_rect_t clip;
    int row = 0;

    stdlib__null_memory(&temporary, sizeof(temporary));
    // A depth-0 backdrop blur in layer mode uses the backend's GPU filter,
    // or reads the back buffer and blurs
    // it with the compositor's kernel (the reference); tested before the
    // painting gate, which is FALSE at depth 0
    if (_compositor_internal__enabled == TRUE && _compositor_internal__layer_mode == TRUE && _compositor_internal__depth == 0)
    {
        _compositor_internal__blur_layer(rect, sigma_px);
        return;
    }
    // the compositor is off: the backend draws this frame itself
    if (_compositor_internal__enabled == FALSE)
    {
        _compositor_internal__blur_direct(rect, sigma_px);
        return;
    }
    if (_compositor_internal__painting() == FALSE)
    {
        _compositor_internal__blurs_cpu++;
        renderer_backend__blur_region(rect, sigma_px);
        return;
    }
    _compositor_internal__touch();
    _compositor_internal__blurs_cpu++;
    clip = _compositor_internal__clip(rect);
    temporary.x = (int)stdlib__ceilf(clip.x);
    temporary.y = (int)stdlib__ceilf(clip.y);
    temporary.w = (int)stdlib__floorf(clip.w);
    temporary.h = (int)stdlib__floorf(clip.h);
    if (temporary.w <= 0 || temporary.h <= 0)
    {
        return;
    }
    temporary.pixels = GUI_MALLOC_T((int64)temporary.w * temporary.h * 4, MM_TYPE__GENERIC);
    if (temporary.pixels == NULL_POINTER)
    {
        return;
    }
    for (row = 0; row < temporary.h; row++)
    {
        stdlib__memcpy(temporary.pixels + (int64)row * temporary.w * 4, source->pixels + ((int64)(temporary.y + row - source->y) * source->w + temporary.x - source->x) * 4, temporary.w * 4);
    }
    _compositor_internal__blur(&temporary, sigma_px);
    for (row = 0; row < temporary.h; row++)
    {
        stdlib__memcpy(source->pixels + ((int64)(temporary.y + row - source->y) * source->w + temporary.x - source->x) * 4, temporary.pixels + (int64)row * temporary.w * 4, temporary.w * 4);
    }
    GUI_FREE(temporary.pixels);
}

/**
 * @brief draw a soft rounded shadow behind a node
 * @param gui_rect_t rect -> shadow bounds
 * @param gui_color_t c -> shadow color
 * @param float radius -> corner radius
 * @param float blur -> blur radius
 * @return void
 */
void renderer__submit_shadow(gui_rect_t rect, gui_color_t c, float radius, float blur)
{
    gui_rect_t clip;
    gui_color_t color = c;
    float dx = 0.0f;
    float dy = 0.0f;
    float d = 0.0f;
    float sigma = stdlib__fmaxf(0.5f, blur * 0.5f);
    int x = 0;
    int y = 0;
    int right = 0;
    int bottom = 0;

    if (_compositor_internal__painting() == FALSE)
    {
        renderer_backend__submit_shadow(_compositor_internal__gpu_rect(rect), c, radius, blur);
        return;
    }
    _compositor_internal__touch();
    clip = rect;
    clip.x -= blur * 3.0f;
    clip.y -= blur * 3.0f;
    clip.w += blur * 6.0f;
    clip.h += blur * 6.0f;
    clip = _compositor_internal__clip(clip);
    right = (int)stdlib__ceilf(clip.x + clip.w);
    bottom = (int)stdlib__ceilf(clip.y + clip.h);
    for (y = (int)stdlib__floorf(clip.y); y < bottom; y++)
    {
        for (x = (int)stdlib__floorf(clip.x); x < right; x++)
        {
            dx = stdlib__fabsf(x + 0.5f - rect.x - rect.w * 0.5f) - rect.w * 0.5f + radius;
            dy = stdlib__fabsf(y + 0.5f - rect.y - rect.h * 0.5f) - rect.h * 0.5f + radius;
            d = stdlib__fminf(stdlib__fmaxf(dx, dy), 0.0f) + stdlib__sqrtf(stdlib__fmaxf(dx, 0.0f) * stdlib__fmaxf(dx, 0.0f) + stdlib__fmaxf(dy, 0.0f) * stdlib__fmaxf(dy, 0.0f)) - radius;
            color.a = c.a * (1.0f / (1.0f + stdlib__expf(1.7f * d / sigma)));
            _compositor_internal__blend(x, y, color);
        }
    }
}

/**
 * @brief build a planar homography from CSS transform functions in declared order
 * @param gui_node_t* node -> transform owner
 * @param float scale -> logical scale
 * @param float* matrix -> nine row-major coefficients in device coordinates
 * @return void
 */
static void _compositor_internal__matrix(gui_node_t* node, float scale, float* matrix)
{
    char value[1024];
    char token[512];
    char name[64];
    char argument[128];
    char origin[128];
    float m[16];
    float t[16];
    float product[16];
    float a[6];
    float ox = node->bounds.x + node->bounds.w * 0.5f;
    float oy = node->bounds.y + node->bounds.h * 0.5f;
    float angle = 0.0f;
    float sine = 0.0f;
    float cosine = 1.0f;
    const char* p = value;
    const char* q = NULL_POINTER;
    char* close = NULL_POINTER;
    int count = 0;
    int i = 0;
    int j = 0;
    int k = 0;
    int length = 0;

    stdlib__null_memory(value, sizeof(value));
    stdlib__null_memory(token, sizeof(token));
    stdlib__null_memory(name, sizeof(name));
    stdlib__null_memory(argument, sizeof(argument));
    stdlib__null_memory(origin, sizeof(origin));
    stdlib__null_memory(m, sizeof(m));
    stdlib__null_memory(t, sizeof(t));
    stdlib__null_memory(product, sizeof(product));
    stdlib__null_memory(a, sizeof(a));
    stdlib__null_memory(matrix, sizeof(float) * 9);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
    motion__value_id(node, MOTION_PROP__TRANSFORM_ORIGIN, origin, sizeof(origin));
    if (origin[0] != 0)
    {
        q = _compositor_internal__part(origin, argument, sizeof(argument), ' ');
        ox = node->bounds.x + ((stdlib__strcmp(argument, "center") == 0) ? node->bounds.w * 0.5f : ((stdlib__strcmp(argument, "right") == 0) ? node->bounds.w : motion__number(argument, node->bounds.w / scale) * scale));
        _compositor_internal__part(q, argument, sizeof(argument), ' ');
        oy = node->bounds.y + ((argument[0] == 0 || stdlib__strcmp(argument, "center") == 0) ? node->bounds.h * 0.5f : ((stdlib__strcmp(argument, "bottom") == 0) ? node->bounds.h : motion__number(argument, node->bounds.h / scale) * scale));
    }
    motion__value_id(node, MOTION_PROP__TRANSFORM, value, sizeof(value));
    while (*p != 0)
    {
        p = _compositor_internal__part(p, token, sizeof(token), ' ');
        q = stdlib__strchr(token, '(');
        close = token + stdlib__strlen(token);
        while (close > token && *close != ')')
        {
            close--;
        }
        if (*close != ')')
        {
            close = NULL_POINTER;
        }
        if (q == NULL_POINTER || close == NULL_POINTER)
        {
            continue;
        }
        length = (int)(q - token);
        if (length >= (int)sizeof(name))
        {
            continue;
        }
        stdlib__memcpy(name, token, length);
        name[length] = 0;
        *close = 0;
        q++;
        count = 0;
        stdlib__null_memory(a, sizeof(a));
        while (*q != 0 && count < 6)
        {
            q = _compositor_internal__part(q, argument, sizeof(argument), (stdlib__strchr(q, ',') != NULL_POINTER) ? ',' : ' ');
            a[count] = motion__number(argument, ((count == 1) ? node->bounds.h : node->bounds.w) / scale);
            count++;
        }
        stdlib__null_memory(t, sizeof(t));
        t[0] = 1.0f;
        t[5] = 1.0f;
        t[10] = 1.0f;
        t[15] = 1.0f;
        if (stdlib__strncmp(name, "translate", 9) == 0)
        {
            if (stdlib__strcmp(name, "translateY") == 0)
            {
                t[13] = motion__number(argument, node->bounds.h / scale) * scale;
            }
            else if (stdlib__strcmp(name, "translateZ") == 0)
            {
                t[14] = a[0] * scale;
            }
            else
            {
                t[12] = a[0] * scale;
                t[13] = a[1] * scale;
                t[14] = a[2] * scale;
            }
        }
        else if (stdlib__strncmp(name, "scale", 5) == 0)
        {
            t[0] = (stdlib__strcmp(name, "scaleY") == 0) ? 1.0f : a[0];
            t[5] = (stdlib__strcmp(name, "scaleX") == 0) ? 1.0f : ((count > 1) ? a[1] : a[0]);
            if (stdlib__strcmp(name, "scale3d") == 0)
            {
                t[10] = a[2];
            }
        }
        else if (stdlib__strncmp(name, "rotate", 6) == 0)
        {
            angle = a[0] * 0.0174532925199433f;
            sine = stdlib__sinf(angle);
            cosine = stdlib__cosf(angle);
            if (stdlib__strcmp(name, "rotateX") == 0)
            {
                t[5] = cosine;
                t[6] = sine;
                t[9] = -sine;
                t[10] = cosine;
            }
            else if (stdlib__strcmp(name, "rotateY") == 0)
            {
                t[0] = cosine;
                t[2] = -sine;
                t[8] = sine;
                t[10] = cosine;
            }
            else
            {
                t[0] = cosine;
                t[1] = sine;
                t[4] = -sine;
                t[5] = cosine;
            }
        }
        else if (stdlib__strcmp(name, "perspective") == 0 && a[0] > 0.0f)
        {
            t[11] = -1.0f / (a[0] * scale);
        }
        else if (stdlib__strncmp(name, "skew", 4) == 0)
        {
            angle = a[0] * 0.0174532925199433f;
            if (stdlib__strcmp(name, "skewY") == 0)
            {
                t[1] = stdlib__sinf(angle) / stdlib__cosf(angle);
            }
            else
            {
                t[4] = stdlib__sinf(angle) / stdlib__cosf(angle);
            }
        }
        else if (stdlib__strcmp(name, "matrix") == 0 && count == 6)
        {
            t[0] = a[0];
            t[1] = a[1];
            t[4] = a[2];
            t[5] = a[3];
            t[12] = a[4] * scale;
            t[13] = a[5] * scale;
        }
        for (i = 0; i < 4; i++)
        {
            for (j = 0; j < 4; j++)
            {
                product[j * 4 + i] = 0.0f;
                for (k = 0; k < 4; k++)
                {
                    product[j * 4 + i] += m[k * 4 + i] * t[j * 4 + k];
                }
            }
        }
        stdlib__memcpy(m, product, sizeof(m));
    }
    matrix[0] = m[0] + ox * m[3];
    matrix[1] = m[4] + ox * m[7];
    matrix[2] = m[12] + ox * m[15] - matrix[0] * ox - matrix[1] * oy;
    matrix[3] = m[1] + oy * m[3];
    matrix[4] = m[5] + oy * m[7];
    matrix[5] = m[13] + oy * m[15] - matrix[3] * ox - matrix[4] * oy;
    matrix[6] = m[3];
    matrix[7] = m[7];
    matrix[8] = m[15] - m[3] * ox - m[7] * oy;
    // Compose navigation translation after the page's own CSS transform.
    matrix[0] += node->navigation_x * matrix[6];
    matrix[1] += node->navigation_x * matrix[7];
    matrix[2] += node->navigation_x * matrix[8];
    matrix[3] += node->navigation_y * matrix[6];
    matrix[4] += node->navigation_y * matrix[7];
    matrix[5] += node->navigation_y * matrix[8];
}

/**
 * @brief project a point with a planar homography
 * @param const float* matrix -> row-major homography
 * @param float x -> source x
 * @param float y -> source y
 * @param float* px -> destination x
 * @param float* py -> destination y
 * @return void
 */
static void _compositor_internal__project(const float* matrix, float x, float y, float* px, float* py)
{
    float w = matrix[6] * x + matrix[7] * y + matrix[8];

    if (stdlib__fabsf(w) < 0.00001f)
    {
        w = 0.00001f;
    }
    *px = (matrix[0] * x + matrix[1] * y + matrix[2]) / w;
    *py = (matrix[3] * x + matrix[4] * y + matrix[5]) / w;
}

/**
 * @brief invert a planar homography, rejecting collapsed transforms
 * @param const float* matrix -> source matrix
 * @param float* inverse -> destination matrix
 * @return boole -> FALSE for singular matrices
 */
static boole _compositor_internal__inverse(const float* matrix, float* inverse)
{
    float determinant = 0.0f;
    int i = 0;

    inverse[0] = matrix[4] * matrix[8] - matrix[5] * matrix[7];
    inverse[1] = matrix[2] * matrix[7] - matrix[1] * matrix[8];
    inverse[2] = matrix[1] * matrix[5] - matrix[2] * matrix[4];
    inverse[3] = matrix[5] * matrix[6] - matrix[3] * matrix[8];
    inverse[4] = matrix[0] * matrix[8] - matrix[2] * matrix[6];
    inverse[5] = matrix[2] * matrix[3] - matrix[0] * matrix[5];
    inverse[6] = matrix[3] * matrix[7] - matrix[4] * matrix[6];
    inverse[7] = matrix[1] * matrix[6] - matrix[0] * matrix[7];
    inverse[8] = matrix[0] * matrix[4] - matrix[1] * matrix[3];
    determinant = matrix[0] * inverse[0] + matrix[1] * inverse[3] + matrix[2] * inverse[6];
    if (stdlib__fabsf(determinant) < 0.000001f)
    {
        return FALSE;
    }
    for (i = 0; i < 9; i++)
    {
        inverse[i] /= determinant;
    }
    return TRUE;
}

/**
 * @brief map input coordinates back through one node's CSS transform
 * @param gui_node_t* node -> transform owner
 * @param float* x -> device x, replaced by local untransformed x
 * @param float* y -> device y, replaced by local untransformed y
 * @return void
 */
void compositor__map_point(gui_node_t* node, float* x, float* y)
{
    float matrix[9];
    float inverse[9];

    stdlib__null_memory(matrix, sizeof(matrix));
    stdlib__null_memory(inverse, sizeof(inverse));
    if (motion__has_id(node, MOTION_PROP__TRANSFORM) == FALSE && node->navigation_effect == FALSE)
    {
        return;
    }
    _compositor_internal__matrix(node, scene__scale(), matrix);
    if (_compositor_internal__inverse(matrix, inverse) == TRUE)
    {
        _compositor_internal__project(inverse, *x, *y, x, y);
    }
}

/**
 * @brief begin a node's isolated transform, mask, filter or text effect surface
 * @param gui_node_t* node -> current node
 * @param float scale -> logical scale
 * @return boole -> TRUE when end_node must be called
 */
boole compositor__begin_node(gui_node_t* node, float scale)
{
    _compositor_internal__group_t* group = NULL_POINTER;
    gui_rect_t area;
    gui_rect_t reach;
    char value[1024];
    const char* blur = NULL_POINTER;
    float padding = 4.0f;
    float margin = 0.0f;
    float radius = 0.0f;
    int64 bytes = 0;
    boole text = FALSE;
    boole needed = FALSE;
    gui_rect_t visible;

    stdlib__null_memory(&area, sizeof(area));
    stdlib__null_memory(&reach, sizeof(reach));
    stdlib__null_memory(&visible, sizeof(visible));
    value[0] = 0;
    if (_compositor_internal__enabled == FALSE || _compositor_internal__depth + 1 >= COMPOSITOR_STACK_CAP)
    {
        return FALSE;
    }
    if (motion__value_id(node, MOTION_PROP__BACKDROP_FILTER, value, sizeof(value)) == TRUE)
    {
        blur = stdlib__strstr(value, "blur(");
        if (blur != NULL_POINTER)
        {
            renderer__blur_region(node->bounds, motion__number(blur + 5, 0.0f) * scale);
        }
    }
    text = (boole)(node->text[0] != 0 && (motion__has_id(node, MOTION_PROP__BACKGROUND_CLIP) == TRUE || motion__has_id(node, MOTION_PROP__WEBKIT_BACKGROUND_CLIP) == TRUE || motion__has_id(node, MOTION_PROP__TEXT_SHADOW) == TRUE || motion__has_id(node, MOTION_PROP__WEBKIT_TEXT_STROKE) == TRUE));
    needed = (boole)(text == TRUE || motion__has_id(node, MOTION_PROP__TRANSFORM) == TRUE || motion__has_id(node, MOTION_PROP__FILTER) == TRUE || motion__has_id(node, MOTION_PROP__MASK) == TRUE || motion__has_id(node, MOTION_PROP__MASK_IMAGE) == TRUE || motion__has_id(node, MOTION_PROP__CLIP_PATH) == TRUE || motion__has_id(node, MOTION_PROP__BORDER_RADIUS) == TRUE || motion__has_id(node, MOTION_PROP__MIX_BLEND_MODE) == TRUE || (node->resolved.has_opacity == TRUE && node->resolved.opacity < 1.0f));
    needed = (boole)(needed || node->navigation_effect);
    if (needed == FALSE)
    {
        return FALSE;
    }
    // Avoid rasterizing effect groups wholly outside an ancestor's viewport.
    // The generous margin includes the source page's translated/blurred layers.
    visible = node->bounds;
    visible.x -= 256.0f * scale;
    visible.y -= 256.0f * scale;
    visible.w += 512.0f * scale;
    visible.h += 512.0f * scale;
    visible = _compositor_internal__clip(visible);
    if (visible.w <= 0.0f || visible.h <= 0.0f)
    {
        return FALSE;
    }
    motion__value_id(node, MOTION_PROP__FILTER, value, sizeof(value));
    blur = stdlib__strstr(value, "blur(");
    if (blur != NULL_POINTER)
    {
        padding += stdlib__fminf(256.0f, motion__number(blur + 5, 0.0f) * scale * 3.0f);
    }
    if (text == TRUE)
    {
        padding = stdlib__fmaxf(padding, 80.0f * scale);
    }
    area = node->bounds;
    area.x -= padding;
    area.y -= padding;
    area.w += padding * 2.0f;
    area.h += padding * 2.0f;
    // A node without a transform composes at identity, so only the part
    // of its padded area inside the parent's visible rect (the parent
    // surface and the scissor above it) can reach a visible pixel. Cut
    // the surface to that, keeping a margin for every neighbourhood
    // read: the node's own filter blur and text effects (the padding
    // above) and the largest backdrop blur of any descendant (recorded
    // by the layout pass), whose edge replication then stays outside the
    // visible rect. A transformed or navigating node keeps the full area:
    // its cut would follow the animation and defeat the decoration
    // cache, and a perspective inverse can put clip corners behind the
    // eye. Every raster addresses the surface by its origin, so the
    // pixels that remain are the same bytes as before.
    if (motion__has_id(node, MOTION_PROP__TRANSFORM) == FALSE && node->navigation_effect == FALSE)
    {
        reach = _compositor_internal__clip(area);
        if (reach.w <= 0.0f || reach.h <= 0.0f)
        {
            return FALSE;
        }
        margin = padding + 1.0f;
        if (node->subtree_backdrop_px > 0.0f)
        {
            radius = stdlib__fminf(256.0f, stdlib__fmaxf(1.0f, stdlib__sqrtf(node->subtree_backdrop_px * node->subtree_backdrop_px * 4.0f + 1.0f) * 0.5f - 0.5f));
            margin += radius * 3.0f + 2.0f;
        }
        reach.x -= margin;
        reach.y -= margin;
        reach.w += margin * 2.0f;
        reach.h += margin * 2.0f;
        area = _compositor_internal__intersection(area, reach);
    }
    if (area.w <= 0.0f || area.h <= 0.0f || area.w > 8192.0f || area.h > 8192.0f)
    {
        return FALSE;
    }
    group = &_compositor_internal__groups[_compositor_internal__depth + 1];
    group->surface.x = (int)stdlib__floorf(area.x);
    group->surface.y = (int)stdlib__floorf(area.y);
    area.w = stdlib__ceilf(area.x + area.w) - group->surface.x;
    area.h = stdlib__ceilf(area.y + area.h) - group->surface.y;
    bytes = (int64)area.w * (int64)area.h * 4;
    group->surface.w = (int)area.w;
    group->surface.h = (int)area.h;
    // a leaf the backend can draw renders into a backend layer: no CPU
    // surface, no upload
    group->gpu = FALSE;
    if (_compositor_internal__gpu_group_possible(node, scale) == TRUE && renderer_backend__begin_layer((int)area.w, (int)area.h) == TRUE)
    {
        group->gpu = TRUE;
    }
    else
    {
        if (group->surface.capacity < bytes)
        {
            GUI_FREE(group->surface.pixels);
            group->surface.pixels = GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
            group->surface.capacity = (group->surface.pixels == NULL_POINTER) ? 0 : bytes;
        }
        if (group->surface.pixels == NULL_POINTER)
        {
            return FALSE;
        }
    }
    group->node = node;
    group->decoration = 0;
    group->content_version = 0;
    group->reused = FALSE;
    group->fused = FALSE;
    group->painted = FALSE;
    group->texture_cached = FALSE;
    group->cleared = FALSE;
    // a top group in layer mode clears on first use: reuse_decoration may
    // find the finished texture in the layer cache, in which case the
    // pixels are never read
    if (group->gpu == FALSE && (_compositor_internal__layer_mode == FALSE || _compositor_internal__depth != 0))
    {
        stdlib__null_memory(group->surface.pixels, bytes);
        group->cleared = TRUE;
    }
    if (group->gpu == TRUE)
    {
        group->cleared = TRUE;
    }
    group->parent_clipped = (boole)(_compositor_internal__clip_count > 0);
    group->opacity = node->effective_opacity;
    group->text_node = _compositor_internal__text_node;
    if (_compositor_internal__clip_count > 0)
    {
        group->clip = _compositor_internal__clips[_compositor_internal__clip_count - 1];
        _compositor_internal__clips[_compositor_internal__clip_count - 1] = area;
    }
    _compositor_internal__depth++;
    _compositor_internal__text_node = (text == TRUE) ? node : NULL_POINTER;
    node->effective_opacity = 1.0f;
    return TRUE;
}

/**
 * @brief reuse a built-in leaf div's filtered decoration, preserving its live transform
 * @param gui_node_t* node -> leaf currently being painted inside its own scissor
 * @param float scale -> logical scale
 * @return boole -> TRUE when the normal decoration draws can be skipped
 */
boole compositor__reuse_decoration(gui_node_t* node, float scale)
{
    static const int properties[COMPOSITOR_DECORATION_PROPERTIES] = {
        MOTION_PROP__BACKGROUND, MOTION_PROP__BACKGROUND_IMAGE, MOTION_PROP__BACKGROUND_SIZE, MOTION_PROP__BACKGROUND_POSITION,
        MOTION_PROP__BACKGROUND_REPEAT, MOTION_PROP__BACKGROUND_CLIP, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, MOTION_PROP__FILTER,
        MOTION_PROP__MASK, MOTION_PROP__MASK_IMAGE, MOTION_PROP__MASK_SIZE, MOTION_PROP__MASK_POSITION, MOTION_PROP__MASK_REPEAT,
        MOTION_PROP__MASK_COMPOSITE, MOTION_PROP__CLIP_PATH, MOTION_PROP__BORDER_RADIUS, MOTION_PROP__TEXT_SHADOW, MOTION_PROP__WEBKIT_TEXT_STROKE
    };
    _compositor_internal__group_t* group = &_compositor_internal__groups[_compositor_internal__depth];
    _compositor_internal__decoration_key_t key;
    _compositor_internal__decoration_cache_t* entry = NULL_POINTER;
    _compositor_internal__decoration_cache_t* oldest = NULL_POINTER;
    int64 bytes = (int64)group->surface.w * group->surface.h * 4;
    int i = 0;

    // The caller is the built-in div painter, so custom vtables cannot opt in
    // accidentally. Images and text have external resource lifetimes; leave
    // those and child/scrollbar content on the ordinary path.
    if (_compositor_internal__enabled == FALSE || _compositor_internal__depth <= 0 || group->node != node || group->gpu == TRUE || node->first_child != NULL_POINTER || node->text[0] != 0 || node->resolved.has_background_image == TRUE || bytes <= 0 || bytes > (int64)MEEK_UI_CPU_DECORATION_CACHE_BYTES)
    {
        return FALSE;
    }
    if (!node->resolved.has_shadow && node->resolved.border_width <= 0.0f &&
        node->resolved.blur_px <= 0.0f && !motion__has_id(node, MOTION_PROP__TRANSFORM))
    {
        static const int excluded[] = {MOTION_PROP__FILTER, MOTION_PROP__MASK, MOTION_PROP__MASK_IMAGE, MOTION_PROP__CLIP_PATH, MOTION_PROP__BORDER_RADIUS,
            MOTION_PROP__BACKGROUND_CLIP, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, MOTION_PROP__TEXT_SHADOW, MOTION_PROP__WEBKIT_TEXT_STROKE};
        for (i = 0; i < (int)(sizeof(excluded) / sizeof(excluded[0])); i++)
            if (motion__has_id(node, excluded[i])) break;
        if (i == (int)(sizeof(excluded) / sizeof(excluded[0])))
        {
            // This built-in leaf has no transform, filter, mask, shadow or
            // child that could bring an outside source pixel into view. Keep
            // its original layout/gradient coordinates, but paint only source
            // pixels that can reach the parent. Round out just like compose.
            _compositor_internal__surface_t* parent = &_compositor_internal__groups[_compositor_internal__depth - 1].surface;
            gui_rect_t visible = {parent->x, parent->y, parent->w, parent->h};
            int left = group->surface.x;
            int top = group->surface.y;
            int right = left + group->surface.w;
            int bottom = top + group->surface.h;
            if (group->parent_clipped)
            {
                visible = _compositor_internal__intersection(visible, group->clip);
            }
            if (visible.w <= 0.0f || visible.h <= 0.0f)
            {
                group->fused = TRUE; // No pixel can reach the parent.
                return TRUE;
            }
            left = (int)stdlib__fmaxf(left, stdlib__floorf(visible.x));
            top = (int)stdlib__fmaxf(top, stdlib__floorf(visible.y));
            right = (int)stdlib__fminf(right, stdlib__ceilf(visible.x + visible.w));
            bottom = (int)stdlib__fminf(bottom, stdlib__ceilf(visible.y + visible.h));
            if (right <= left || bottom <= top)
            {
                group->fused = TRUE;
                return TRUE;
            }
            group->surface.x = left;
            group->surface.y = top;
            group->surface.w = right - left;
            group->surface.h = bottom - top;
            bytes = (int64)group->surface.w * group->surface.h * 4;
            // a top group in layer mode has no parent pixels to fuse into
            if (effect_acceleration__enabled() && !_compositor_internal__groups[_compositor_internal__depth - 1].gpu && parent->pixels != NULL_POINTER && _compositor_internal__background(node, scale, TRUE))
            {
                group->fused = TRUE;
                return TRUE;
            }
        }
    }
    stdlib__null_memory(&key, sizeof(key));
    key.style = node->resolved;
    key.style.opacity = 1.0f;
    key.style.has_opacity = FALSE;
    key.bounds = node->bounds;
    key.scale = scale;
    key.x = group->surface.x;
    key.y = group->surface.y;
    key.w = group->surface.w;
    key.h = group->surface.h;
    if (_compositor_internal__clip_count > 0)
    {
        key.clip = _compositor_internal__clips[_compositor_internal__clip_count - 1];
    }
    for (i = 0; i < COMPOSITOR_DECORATION_PROPERTIES; i++)
    {
        motion__value_id(node, properties[i], key.values[i], sizeof(key.values[i]));
    }
    oldest = &_compositor_internal__decorations[0];
    for (i = 0; i < COMPOSITOR_DECORATION_CACHE_CAP; i++)
    {
        if (_compositor_internal__decorations[i].owner == node)
        {
            entry = &_compositor_internal__decorations[i];
            break;
        }
        if (_compositor_internal__decorations[i].used < oldest->used)
        {
            oldest = &_compositor_internal__decorations[i];
        }
    }
    if (entry == NULL_POINTER)
    {
        entry = oldest;
    }
    if (entry->owner != node || stdlib__memcmp(&entry->key, &key, sizeof(key)) != 0)
    {
        GUI_FREE(entry->pixels);
        _compositor_internal__decoration_bytes -= entry->bytes;
        stdlib__null_memory(entry, sizeof(*entry));
        entry->owner = node;
        entry->key = key;
        entry->used = _compositor_internal__frame;
        return FALSE;
    }
    entry->used = _compositor_internal__frame;
    if (entry->valid == TRUE)
    {
        group->reused = TRUE;
        group->content_version = entry->content_version;
        // a top group whose texture the layer cache holds needs no pixels
        if ((_compositor_internal__depth == 1 || _compositor_internal__groups[_compositor_internal__depth - 1].gpu) && _compositor_internal__layer_texture_cached(node, entry->content_version) == TRUE)
        {
            group->texture_cached = TRUE;
            return TRUE;
        }
        stdlib__memcpy(group->surface.pixels, entry->pixels, bytes);
        group->cleared = TRUE;
        return TRUE;
    }
    while (_compositor_internal__decoration_bytes + bytes > (int64)MEEK_UI_CPU_DECORATION_CACHE_BYTES)
    {
        oldest = NULL_POINTER;
        for (i = 0; i < COMPOSITOR_DECORATION_CACHE_CAP; i++)
        {
            if (_compositor_internal__decorations[i].pixels != NULL_POINTER && _compositor_internal__decorations[i].used + 1 < _compositor_internal__frame && (oldest == NULL_POINTER || _compositor_internal__decorations[i].used < oldest->used))
            {
                oldest = &_compositor_internal__decorations[i];
            }
        }
        if (oldest == NULL_POINTER)
        {
            return FALSE;
        }
        GUI_FREE(oldest->pixels);
        _compositor_internal__decoration_bytes -= oldest->bytes;
        oldest->pixels = NULL_POINTER;
        oldest->bytes = 0;
        oldest->valid = FALSE;
    }
    entry->pixels = GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
    if (entry->pixels != NULL_POINTER)
    {
        entry->bytes = bytes;
        _compositor_internal__decoration_bytes += bytes;
        group->decoration = (int)(entry - _compositor_internal__decorations) + 1;
    }
    return FALSE;
}

/**
 * @brief blend clipped rows for a normal, integer-translated effect surface
 * @return boole -> TRUE when completed; FALSE selects the general pixel loop
 */
static boole _compositor_internal__compose_spans(_compositor_internal__surface_t* destination,
    const _compositor_internal__surface_t* surface, gui_rect_t bounds, const float* inverse, float opacity)
{
    float source_byte[256];
    float inverse_alpha[256];
    const ubyte* texel = NULL_POINTER;
    ubyte* pixel = NULL_POINTER;
    int left = (int)stdlib__floorf(bounds.x);
    int top = (int)stdlib__floorf(bounds.y);
    int right = (int)stdlib__ceilf(bounds.x + bounds.w);
    int bottom = (int)stdlib__ceilf(bounds.y + bounds.h);
    int dx = 0;
    int dy = 0;
    int x = 0;
    int y = 0;
    int i = 0;

    // Keep unusually large coordinates on the original float sampling path.
    // In this range integer subtraction matches its exact float coordinates.
    if (stdlib__fabsf(inverse[2]) > 1000000.0f || stdlib__fabsf(inverse[5]) > 1000000.0f ||
        surface->x < -1000000 || surface->x > 1000000 || surface->y < -1000000 || surface->y > 1000000 ||
        destination->x < -1000000 || destination->x > 1000000 || destination->y < -1000000 || destination->y > 1000000)
    {
        return FALSE;
    }
    dx = surface->x - (int)inverse[2];
    dy = surface->y - (int)inverse[5];
    if (left < dx) left = dx;
    if (top < dy) top = dy;
    if (right > dx + surface->w) right = dx + surface->w;
    if (bottom > dy + surface->h) bottom = dy + surface->h;
    if (left >= right || top >= bottom) return TRUE;
    // Amortize the two small tables over a sufficiently large visible area.
    if ((int64)(right - left) * (bottom - top) < 1024) return FALSE;
    for (i = 0; i < 256; i++)
    {
        float normalized = (float)i / 255.0f;
        // Preserve the general loop's float operation order and byte rounding.
        source_byte[i] = normalized * opacity * 255.0f;
        inverse_alpha[i] = 1.0f - normalized * opacity;
    }
    for (y = top; y < bottom; y++)
    {
        texel = surface->pixels + ((int64)(y - dy) * surface->w + left - dx) * 4;
        pixel = destination->pixels + ((int64)(y - destination->y) * destination->w + left - destination->x) * 4;
        for (x = left; x < right; x++, texel += 4, pixel += 4)
        {
            unsigned a = texel[3];
            float remaining = inverse_alpha[a];
            if (a == 0) continue;
            // Filter rounding can leave RGB above alpha; retain its clamp.
            pixel[0] = (ubyte)(source_byte[texel[0] < a ? texel[0] : a] + pixel[0] * remaining + 0.5f);
            pixel[1] = (ubyte)(source_byte[texel[1] < a ? texel[1] : a] + pixel[1] * remaining + 0.5f);
            pixel[2] = (ubyte)(source_byte[texel[2] < a ? texel[2] : a] + pixel[2] * remaining + 0.5f);
            pixel[3] = (ubyte)(source_byte[a] + pixel[3] * remaining + 0.5f);
        }
    }
    return TRUE;
}

#if defined(COMPOSITOR_X86_SIMD)
// Sample and blend four channels together, preserving the scalar operation
// order (including rounding and clamped edge texels). Only normal blending
// enters here; integer translations already use the cheaper span path.
static void _compositor_internal__compose_rgba(_compositor_internal__surface_t* destination,
    const _compositor_internal__surface_t* surface, gui_rect_t bounds, const float* inverse, float opacity)
{
    int left = (int)stdlib__floorf(bounds.x);
    int top = (int)stdlib__floorf(bounds.y);
    int right = (int)stdlib__ceilf(bounds.x + bounds.w);
    int bottom = (int)stdlib__ceilf(bounds.y + bounds.h);
    boole affine = (boole)(inverse[6] == 0.0f && inverse[7] == 0.0f && inverse[8] == 1.0f);
    __m128 opacity4 = _mm_set1_ps(opacity);
    __m128 byte_scale = _mm_set1_ps(255.0f);
    __m128 one = _mm_set1_ps(1.0f);
    __m128 half = _mm_set1_ps(0.5f);
    for (int y = top; y < bottom; y++)
    {
        ubyte* target = destination->pixels + ((int64)(y - destination->y) * destination->w + left - destination->x) * 4;
        for (int x = left; x < right; x++, target += 4)
        {
            float sx, sy;
            if (affine)
            {
                sx = inverse[0] * (x + 0.5f) + inverse[1] * (y + 0.5f) + inverse[2];
                sy = inverse[3] * (x + 0.5f) + inverse[4] * (y + 0.5f) + inverse[5];
            }
            else
            {
                _compositor_internal__project(inverse, x + 0.5f, y + 0.5f, &sx, &sy);
            }
            sx -= surface->x;
            sy -= surface->y;
            // The positive range test also rejects non-finite coordinates
            // before converting them to integer texel addresses.
            if (!(sx >= 0.0f && sy >= 0.0f && sx < surface->w && sy < surface->h))
            {
                continue;
            }
            sx -= 0.5f;
            sy -= 0.5f;
            // After the checked source bounds and half-pixel offset these
            // coordinates are in [-0.5, dimension). Truncation only needs a
            // correction for the negative half-pixel along the top/left edge.
            int ix = (int)sx - (sx < 0.0f);
            int iy = (int)sy - (sy < 0.0f);
            float fx = sx - ix;
            float fy = sy - iy;
            int x0 = ix < 0 ? 0 : ix;
            int y0 = iy < 0 ? 0 : iy;
            int x1 = ix + 1 >= surface->w ? surface->w - 1 : ix + 1;
            int y1 = iy + 1 >= surface->h ? surface->h - 1 : iy + 1;
            const ubyte* row0 = surface->pixels + (int64)y0 * surface->w * 4;
            const ubyte* row1 = surface->pixels + (int64)y1 * surface->w * 4;
            __m128 color = _mm_mul_ps(_compositor_internal__rgba_float(row0 + x0 * 4), _mm_set1_ps((1.0f - fx) * (1.0f - fy) / 255.0f));
            color = _mm_add_ps(color, _mm_mul_ps(_compositor_internal__rgba_float(row0 + x1 * 4), _mm_set1_ps(fx * (1.0f - fy) / 255.0f)));
            color = _mm_add_ps(color, _mm_mul_ps(_compositor_internal__rgba_float(row1 + x0 * 4), _mm_set1_ps((1.0f - fx) * fy / 255.0f)));
            color = _mm_add_ps(color, _mm_mul_ps(_compositor_internal__rgba_float(row1 + x1 * 4), _mm_set1_ps(fx * fy / 255.0f)));
            __m128 alpha = _mm_shuffle_ps(color, color, _MM_SHUFFLE(3, 3, 3, 3));
            if (_mm_cvtss_f32(alpha) <= 0.0f)
            {
                continue;
            }
            __m128 source = _mm_mul_ps(_mm_mul_ps(_mm_min_ps(color, alpha), opacity4), byte_scale);
            __m128 remaining = _mm_sub_ps(one, _mm_mul_ps(alpha, opacity4));
            __m128 result = _mm_add_ps(_mm_add_ps(source, _mm_mul_ps(_compositor_internal__rgba_float(target), remaining)), half);
            __m128i bytes = _mm_cvttps_epi32(result);
            bytes = _mm_packs_epi32(bytes, bytes);
            bytes = _mm_packus_epi16(bytes, bytes);
            uint packed = (uint)_mm_cvtsi128_si32(bytes);
            target[0]=(ubyte)packed; target[1]=(ubyte)(packed>>8); target[2]=(ubyte)(packed>>16); target[3]=(ubyte)(packed>>24);
        }
    }
}
#endif

#if defined(COMPOSITOR_X86_SIMD)
/**
 * @brief the general loop's blend-mode branch for an integer-translated surface, four channels at a time
 * @param _compositor_internal__surface_t* destination -> parent surface
 * @param const _compositor_internal__surface_t* surface -> premultiplied source
 * @param gui_rect_t bounds -> clipped destination area
 * @param const float* inverse -> identity with an integer translation
 * @param float opacity -> group opacity
 * @param int blend_mode -> 1 screen, 2 multiply, 3 overlay, 4 difference
 * @return void
 */
static void _compositor_internal__compose_blend_aligned(_compositor_internal__surface_t* destination, const _compositor_internal__surface_t* surface, gui_rect_t bounds, const float* inverse, float opacity, int blend_mode)
{
    const ubyte* texel = NULL_POINTER;
    ubyte* pixel = NULL_POINTER;
    int left = (int)stdlib__floorf(bounds.x);
    int top = (int)stdlib__floorf(bounds.y);
    int right = (int)stdlib__ceilf(bounds.x + bounds.w);
    int bottom = (int)stdlib__ceilf(bounds.y + bounds.h);
    int x = 0;
    int y = 0;
    float sx = 0.0f;
    float sy = 0.0f;
    __m128 byte_scale = _mm_set1_ps(255.0f);
    __m128 one = _mm_set1_ps(1.0f);
    __m128 two = _mm_set1_ps(2.0f);
    __m128 half = _mm_set1_ps(0.5f);
    __m128 zero = _mm_setzero_ps();
    __m128 opacity4 = _mm_set1_ps(opacity);
    __m128 magnitude = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    __m128 color;
    __m128 color_alpha;
    __m128 alpha;
    __m128 remaining;
    __m128 source;
    __m128 backdrop;
    __m128 backdrop_alpha;
    __m128 mixed;
    __m128 select;
    __m128 channels;
    __m128 result;

    for (y = top; y < bottom; y++)
    {
        sy = (float)(y - surface->y) + inverse[5];
        if (sy < 0.0f || sy >= surface->h)
        {
            continue;
        }
        for (x = left; x < right; x++)
        {
            sx = (float)(x - surface->x) + inverse[2];
            if (sx < 0.0f || sx >= surface->w)
            {
                continue;
            }
            texel = surface->pixels + ((int64)(int)sy * surface->w + (int)sx) * 4;
            // texel / 255 per channel, a division as in the scalar loop
            color = _mm_div_ps(_compositor_internal__rgba_float(texel), byte_scale);
            color_alpha = _mm_shuffle_ps(color, color, _MM_SHUFFLE(3, 3, 3, 3));
            if (_mm_cvtss_f32(color_alpha) <= 0.0f)
            {
                continue;
            }
            pixel = destination->pixels + ((int64)(y - destination->y) * destination->w + x - destination->x) * 4;
            alpha = _mm_mul_ps(color_alpha, opacity4);
            remaining = _mm_sub_ps(one, alpha);
            // straight source channels and straight backdrop channels (zero
            // where the backdrop alpha is zero, as the scalar test does)
            source = _mm_div_ps(color, color_alpha);
            backdrop = _compositor_internal__rgba_float(pixel);
            backdrop_alpha = _mm_shuffle_ps(backdrop, backdrop, _MM_SHUFFLE(3, 3, 3, 3));
            backdrop = _mm_and_ps(_mm_cmpgt_ps(backdrop_alpha, zero), _mm_div_ps(backdrop, backdrop_alpha));
            if (blend_mode == 1)
            {
                mixed = _mm_sub_ps(one, _mm_mul_ps(_mm_sub_ps(one, source), _mm_sub_ps(one, backdrop)));
            }
            else if (blend_mode == 2)
            {
                mixed = _mm_mul_ps(source, backdrop);
            }
            else if (blend_mode == 3)
            {
                select = _mm_cmple_ps(backdrop, half);
                mixed = _mm_or_ps(_mm_and_ps(select, _mm_mul_ps(_mm_mul_ps(two, source), backdrop)), _mm_andnot_ps(select, _mm_sub_ps(one, _mm_mul_ps(_mm_mul_ps(two, _mm_sub_ps(one, source)), _mm_sub_ps(one, backdrop)))));
            }
            else
            {
                mixed = _mm_and_ps(magnitude, _mm_sub_ps(source, backdrop));
            }
            // source + (mixed - source) * backdrop byte alpha / 255
            channels = _mm_add_ps(source, _mm_div_ps(_mm_mul_ps(_mm_sub_ps(mixed, source), backdrop_alpha), byte_scale));
            channels = _mm_min_ps(_mm_max_ps(channels, zero), one);
            // rgb lanes: clamp(channel) * alpha * 255; alpha lane: alpha * 255
            result = _mm_mul_ps(_mm_mul_ps(_compositor_internal__with_alpha(channels, alpha), _compositor_internal__with_alpha(alpha, one)), byte_scale);
            result = _mm_add_ps(_mm_add_ps(result, _mm_mul_ps(_compositor_internal__rgba_float(pixel), remaining)), half);
            _compositor_internal__store_rgba(pixel, result);
        }
    }
}
#endif

/**
 * @brief composite an effect surface through its inverse transform and blend mode
 * @param gui_node_t* node -> transform owner
 * @param _compositor_internal__surface_t* surface -> premultiplied source
 * @param float scale -> logical scale
 * @return void
 */
static void _compositor_internal__compose(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, uint64 content_version)
{
    _compositor_internal__texture_t texture;
    _compositor_internal__surface_t* destination = &_compositor_internal__groups[_compositor_internal__depth].surface;
    gui_rect_t bounds;
    gui_color_t color;
    float matrix[9];
    float inverse[9];
    float px = 0.0f;
    float py = 0.0f;
    float sx = 0.0f;
    float sy = 0.0f;
    float right = -1000000.0f;
    float bottom = -1000000.0f;
    float backdrop = 0.0f;
    float source = 0.0f;
    float mixed = 0.0f;
    float opacity = node->effective_opacity;
    float alpha = 0.0f;
    float inv_alpha = 0.0f;
    float channels[3];
    ubyte* pixel = NULL_POINTER;
    const ubyte* texel = NULL_POINTER;
    boole aligned = FALSE;
    int blend_mode = 0;
    int x = 0;
    int y = 0;
    int i = 0;
    int c = 0;

    stdlib__null_memory(&texture, sizeof(texture));
    stdlib__null_memory(&bounds, sizeof(bounds));
    stdlib__null_memory(&color, sizeof(color));
    stdlib__null_memory(matrix, sizeof(matrix));
    stdlib__null_memory(inverse, sizeof(inverse));
    stdlib__null_memory(channels, sizeof(channels));
    _compositor_internal__matrix(node, scale, matrix);
    if (_compositor_internal__inverse(matrix, inverse) == FALSE)
    {
        return;
    }
    bounds.x = 1000000.0f;
    bounds.y = 1000000.0f;
    for (i = 0; i < 4; i++)
    {
        _compositor_internal__project(matrix, surface->x + ((i & 1) != 0 ? surface->w : 0), surface->y + ((i & 2) != 0 ? surface->h : 0), &px, &py);
        bounds.x = stdlib__fminf(bounds.x, px);
        bounds.y = stdlib__fminf(bounds.y, py);
        right = stdlib__fmaxf(right, px);
        bottom = stdlib__fmaxf(bottom, py);
    }
    bounds.w = right - bounds.x;
    bounds.h = bottom - bounds.y;
    bounds = _compositor_internal__clip(bounds);
    // Rounding an empty fractional clip outward can otherwise visit a pixel
    // outside the destination surface.
    if (bounds.w <= 0.0f || bounds.h <= 0.0f)
    {
        return;
    }
    right = stdlib__ceilf(bounds.x + bounds.w);
    bottom = stdlib__ceilf(bounds.y + bounds.h);
    texture.pixels = surface->pixels;
    texture.w = surface->w;
    texture.h = surface->h;
    texture.channels = 4;
    blend_mode = _compositor_internal__blend_mode(node);
    aligned = (boole)(inverse[0] == 1.0f && inverse[1] == 0.0f && inverse[3] == 0.0f && inverse[4] == 1.0f && inverse[6] == 0.0f && inverse[7] == 0.0f && inverse[8] == 1.0f && inverse[2] == stdlib__floorf(inverse[2]) && inverse[5] == stdlib__floorf(inverse[5]));
    // The compute engine uses its own context and returns completed CPU pixels.
    // On failure no destination byte is changed, so this loop remains the fallback.
    if (_compositor_internal__no_acceleration == FALSE && (aligned == FALSE || blend_mode != 0))
    {
        effect_surface_t input = {surface->pixels, surface->x, surface->y, surface->w, surface->h, content_version};
        effect_surface_t output = {destination->pixels, destination->x, destination->y, destination->w, destination->h, 0};
        int left = (int)stdlib__floorf(bounds.x);
        int top = (int)stdlib__floorf(bounds.y);
        if (effect_acceleration__compose(&input, &output, left, top, (int)right - left, (int)bottom - top, inverse, opacity, blend_mode) == TRUE)
        {
            return;
        }
    }
    if (aligned == TRUE && blend_mode == 0 &&
        _compositor_internal__compose_spans(destination, surface, bounds, inverse, opacity) == TRUE)
    {
        return;
    }
#if defined(COMPOSITOR_X86_SIMD)
    if (_compositor_internal__sse2 == TRUE && aligned == FALSE && blend_mode == 0 && bounds.w * bounds.h >= 1024.0f)
    {
        _compositor_internal__compose_rgba(destination, surface, bounds, inverse, opacity);
        return;
    }
    if (_compositor_internal__sse2 == TRUE && aligned == TRUE && blend_mode >= 1 && blend_mode <= 4)
    {
        _compositor_internal__compose_blend_aligned(destination, surface, bounds, inverse, opacity, blend_mode);
        return;
    }
#endif
    for (y = (int)stdlib__floorf(bounds.y); y < (int)bottom; y++)
    {
        for (x = (int)stdlib__floorf(bounds.x); x < (int)right; x++)
        {
            if (aligned == TRUE)
            {
                sx = (float)(x - surface->x) + inverse[2];
                sy = (float)(y - surface->y) + inverse[5];
                if (sx < 0.0f || sy < 0.0f || sx >= surface->w || sy >= surface->h)
                {
                    continue;
                }
                texel = surface->pixels + ((int64)(int)sy * surface->w + (int)sx) * 4;
                color.r = texel[0] / 255.0f;
                color.g = texel[1] / 255.0f;
                color.b = texel[2] / 255.0f;
                color.a = texel[3] / 255.0f;
            }
            else
            {
                _compositor_internal__project(inverse, x + 0.5f, y + 0.5f, &sx, &sy);
                sx -= surface->x;
                sy -= surface->y;
                if (sx < 0.0f || sy < 0.0f || sx >= surface->w || sy >= surface->h)
                {
                    continue;
                }
                color = _compositor_internal__texel(&texture, sx - 0.5f, sy - 0.5f);
            }
            if (color.a <= 0.0f)
            {
                continue;
            }
            pixel = destination->pixels + ((int64)(y - destination->y) * destination->w + x - destination->x) * 4;
            alpha = color.a * opacity;
            inv_alpha = 1.0f - alpha;
            if (blend_mode != 0)
            {
                channels[0] = color.r / color.a;
                channels[1] = color.g / color.a;
                channels[2] = color.b / color.a;
                for (c = 0; c < 3; c++)
                {
                    backdrop = (pixel[3] > 0) ? (float)pixel[c] / pixel[3] : 0.0f;
                    source = channels[c];
                    mixed = source;
                    if (blend_mode == 1)
                    {
                        mixed = 1.0f - (1.0f - source) * (1.0f - backdrop);
                    }
                    else if (blend_mode == 2)
                    {
                        mixed = source * backdrop;
                    }
                    else if (blend_mode == 3)
                    {
                        mixed = (backdrop <= 0.5f) ? 2.0f * source * backdrop : 1.0f - 2.0f * (1.0f - source) * (1.0f - backdrop);
                    }
                    else if (blend_mode == 4)
                    {
                        mixed = stdlib__fabsf(source - backdrop);
                    }
                    channels[c] = source + (mixed - source) * pixel[3] / 255.0f;
                }
                pixel[0] = (ubyte)(_compositor_internal__clamp(channels[0]) * alpha * 255.0f + pixel[0] * inv_alpha + 0.5f);
                pixel[1] = (ubyte)(_compositor_internal__clamp(channels[1]) * alpha * 255.0f + pixel[1] * inv_alpha + 0.5f);
                pixel[2] = (ubyte)(_compositor_internal__clamp(channels[2]) * alpha * 255.0f + pixel[2] * inv_alpha + 0.5f);
            }
            else
            {
                // Keep the premultiplied source premultiplied. Rounding in
                // earlier filters can put RGB above alpha, hence the min.
                pixel[0] = (ubyte)(stdlib__fminf(color.r, color.a) * opacity * 255.0f + pixel[0] * inv_alpha + 0.5f);
                pixel[1] = (ubyte)(stdlib__fminf(color.g, color.a) * opacity * 255.0f + pixel[1] * inv_alpha + 0.5f);
                pixel[2] = (ubyte)(stdlib__fminf(color.b, color.a) * opacity * 255.0f + pixel[2] * inv_alpha + 0.5f);
            }
            pixel[3] = (ubyte)(alpha * 255.0f + pixel[3] * inv_alpha + 0.5f);
        }
    }
}

// ============================================================================
// LAYER MODE
// ============================================================================

/**
 * @brief whether submissions currently rasterize on the CPU
 * @return boole -> TRUE in frame mode, or inside a group in layer mode
 */
static inline boole _compositor_internal__painting(void)
{
    return (boole)(_compositor_internal__enabled == TRUE && (_compositor_internal__layer_mode == FALSE || (_compositor_internal__depth > 0 && _compositor_internal__groups[_compositor_internal__depth].gpu == FALSE)));
}

/**
 * @brief whether submissions currently draw into a backend layer (forwarded in layer coordinates)
 * @return boole -> TRUE inside a GPU group
 */
static inline boole _compositor_internal__gpu_group(void)
{
    return (boole)(_compositor_internal__layer_mode == TRUE && _compositor_internal__depth > 0 && _compositor_internal__groups[_compositor_internal__depth].gpu == TRUE);
}

/**
 * @brief a rect in the coordinates the backend expects: layer-relative inside a GPU group
 * @param gui_rect_t r -> device rect
 * @return gui_rect_t -> translated rect
 */
static inline gui_rect_t _compositor_internal__gpu_rect(gui_rect_t r)
{
    if (_compositor_internal__gpu_group() == TRUE)
    {
        r.x -= (float)_compositor_internal__groups[_compositor_internal__depth].surface.x;
        r.y -= (float)_compositor_internal__groups[_compositor_internal__depth].surface.y;
    }
    return r;
}

/**
 * @brief a point in the coordinates the backend expects: layer-relative inside a GPU group
 * @param gui_vec2_t p -> device point
 * @return gui_vec2_t -> translated point
 */
static inline gui_vec2_t _compositor_internal__gpu_point(gui_vec2_t p)
{
    if (_compositor_internal__gpu_group() == TRUE)
    {
        p.x -= (float)_compositor_internal__groups[_compositor_internal__depth].surface.x;
        p.y -= (float)_compositor_internal__groups[_compositor_internal__depth].surface.y;
    }
    return p;
}

// A navigation page can be a live GPU layer. Nested effects still rasterize
// into small CPU surfaces and are uploaded into that layer, instead of forcing
// every label, background and child into a page-sized CPU raster. Keep pages
// needing backdrop reads, non-normal blending or perspective on the existing
// CPU path: those operations require the parent's pixels, not just a texture.
static boole _compositor_internal__gpu_page_node_possible(gui_node_t* node, float scale)
{
    _compositor_internal__layer_t layers[COMPOSITOR_LAYER_CAP];
    gui_rect_t tile;
    float matrix[9];
    int columns = 0;
    int rows = 0;
    int count = 0;
    int i = 0;
    if (node->resolved.blur_px > 0.0f || node->subtree_backdrop_px > 0.0f ||
        motion__has_id(node, MOTION_PROP__BACKDROP_FILTER) || _compositor_internal__blend_mode(node) != 0)
    {
        return FALSE;
    }
    _compositor_internal__matrix(node, scale, matrix);
    if (matrix[6] != 0.0f || matrix[7] != 0.0f || matrix[8] <= 0.0f) return FALSE;
    count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND_IMAGE, layers, COMPOSITOR_LAYER_CAP, scale);
    if (!count) count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND, layers, COMPOSITOR_LAYER_CAP, scale);
    for (i = 0; i < count; i++)
    {
        if (!_compositor_internal__tile_grid(&layers[i], node->bounds, node->bounds, &tile, &columns, &rows)) return FALSE;
    }
    return TRUE;
}

static boole _compositor_internal__gpu_page_content_possible(gui_node_t* root, float scale)
{
    gui_node_t* node = root;
    // Walk once without allocating a gradient parse buffer per tree level.
    while (node != NULL_POINTER)
    {
        if (!node->navigation_inactive)
        {
            if (!_compositor_internal__gpu_page_node_possible(node, scale)) return FALSE;
            if (node->first_child != NULL_POINTER)
            {
                node = node->first_child;
                continue;
            }
        }
        while (node != root && node->next_sibling == NULL_POINTER) node = node->parent;
        if (node == root) break;
        node = node->next_sibling;
    }
    return TRUE;
}

/**
 * @brief admit a native leaf layer, or an opted-in live navigation page whose descendants can finish without reading the GPU parent
 * @param gui_node_t* node -> the group node, at depth 0
 * @param float scale -> logical scale
 * @return boole -> TRUE when renderer_backend__begin_layer should draw it
 */
static boole _compositor_internal__gpu_group_possible(gui_node_t* node, float scale)
{
    static const int excluded[] = {MOTION_PROP__FILTER, MOTION_PROP__BACKDROP_FILTER, MOTION_PROP__MASK, MOTION_PROP__MASK_IMAGE, MOTION_PROP__CLIP_PATH, MOTION_PROP__BORDER_RADIUS, MOTION_PROP__BACKGROUND_CLIP, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, MOTION_PROP__TEXT_SHADOW, MOTION_PROP__WEBKIT_TEXT_STROKE};
    _compositor_internal__layer_t layers[COMPOSITOR_LAYER_CAP];
    gui_rect_t tile;
    int count = 0;
    int columns = 0;
    int rows = 0;
    int i = 0;

    stdlib__null_memory(&tile, sizeof(tile));
    if (_compositor_internal__layer_mode == FALSE || _compositor_internal__depth != 0 || renderer_backend__supports_layers() == FALSE)
    {
        return FALSE;
    }
    if (node->text[0] != 0 || node->resolved.blur_px > 0.0f || node->subtree_backdrop_px > 0.0f)
    {
        return FALSE;
    }
    for (i = 0; i < (int)(sizeof(excluded) / sizeof(excluded[0])); i++)
    {
        if (motion__has_id(node, excluded[i]) == TRUE)
        {
            return FALSE;
        }
    }
    if (node->navigation_effect)
    {
        return (boole)(effect_acceleration__native_enabled() && _compositor_internal__gpu_page_content_possible(node, scale));
    }
    if (node->first_child != NULL_POINTER) return FALSE;
    // overlay and difference compose on a read-back of CPU pixels
    if (_compositor_internal__blend_mode(node) >= 3)
    {
        return FALSE;
    }
    count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND_IMAGE, layers, COMPOSITOR_LAYER_CAP, scale);
    if (count == 0)
    {
        count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND, layers, COMPOSITOR_LAYER_CAP, scale);
    }
    // every tracked layer is a solid or a gradient the backend tiles
    for (i = 0; i < count; i++)
    {
        if (_compositor_internal__tile_grid(&layers[i], node->bounds, node->bounds, &tile, &columns, &rows) == FALSE)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief draw a GPU group's tracked background layers with the backend's pipelines (the CPU sampler's order: color, then the layers last to first)
 * @param gui_node_t* node -> the group node
 * @param float scale -> logical scale
 * @return boole -> TRUE when handled, FALSE when the caller draws the plain color and gradient
 */
static boole _compositor_internal__background_gpu(gui_node_t* node, float scale)
{
    _compositor_internal__layer_t layers[COMPOSITOR_LAYER_CAP];
    gui_rect_t rect = _compositor_internal__gpu_rect(node->bounds);
    gui_rect_t clip = _compositor_internal__gpu_rect(_compositor_internal__clip(node->bounds));
    char text_clip[64];
    int count = 0;
    int i = 0;
    boole uniform = TRUE;

    text_clip[0] = 0;
    motion__value_id(node, MOTION_PROP__BACKGROUND_CLIP, text_clip, sizeof(text_clip));
    if (text_clip[0] == 0)
    {
        motion__value_id(node, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, text_clip, sizeof(text_clip));
    }
    if (stdlib__strcmp(text_clip, "text") == 0)
    {
        return TRUE;
    }
    count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND_IMAGE, layers, COMPOSITOR_LAYER_CAP, scale);
    if (count == 0)
    {
        count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND, layers, COMPOSITOR_LAYER_CAP, scale);
    }
    if (count == 0 && !node->resolved.has_background_image && (node->resolved.has_background_color == FALSE || node->resolved.background_color.a <= 0.0f))
    {
        return TRUE;
    }
    if (count == 0)
    {
        return FALSE;
    }
    for (i = 0; i < count; i++)
    {
        if (layers[i].is_gradient || !layers[i].repeat_x || !layers[i].repeat_y)
        {
            uniform = FALSE;
            break;
        }
    }
    if (uniform)
    {
        // As at frame depth, constant repeating layers collapse to one solid
        // rectangle. Keep the native rectangle's exact pixel ownership too.
        gui_color_t color = {0};
        if (node->resolved.has_background_color) color = node->resolved.background_color;
        for (i = count - 1; i >= 0; i--) color = _compositor_internal__over(layers[i].color, color);
        color.a *= node->effective_opacity;
        renderer_backend__submit_rect(rect, color, node->resolved.radius * scale);
        return TRUE;
    }
    // the group's opacity is applied when the layer is drawn (effective_opacity is one inside)
    return _compositor_internal__background_tiles(node, layers, count, rect, clip, scale, node->effective_opacity);
}

/**
 * @brief draw a finished layer texture as the group's quad through the node's transform
 * @param gui_node_t* node -> group owner
 * @param _compositor_internal__surface_t* surface -> the layer's device rect
 * @param float scale -> logical scale
 * @param void* texture -> layer texture
 * @param int blend_mode -> compositor blend mode (0 normal, 1 screen, 2 multiply)
 * @return void
 */
static void _compositor_internal__present_texture(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, void* texture, int blend_mode)
{
    float matrix[9];
    float corners[12];
    float sx = 0.0f;
    float sy = 0.0f;
    int i = 0;

    stdlib__null_memory(matrix, sizeof(matrix));
    stdlib__null_memory(corners, sizeof(corners));
    if (texture == NULL_POINTER)
    {
        return;
    }
    _compositor_internal__matrix(node, scale, matrix);
    for (i = 0; i < 4; i++)
    {
        sx = (float)surface->x + (((i == 1 || i == 2)) ? (float)surface->w : 0.0f);
        sy = (float)surface->y + (((i == 2 || i == 3)) ? (float)surface->h : 0.0f);
        corners[i * 3 + 0] = matrix[0] * sx + matrix[1] * sy + matrix[2];
        corners[i * 3 + 1] = matrix[3] * sx + matrix[4] * sy + matrix[5];
        corners[i * 3 + 2] = matrix[6] * sx + matrix[7] * sy + matrix[8];
        // a corner behind the eye has no CPU pixels to fall back on: skip
        if (corners[i * 3 + 2] <= 0.00001f)
        {
            return;
        }
    }
    _compositor_internal__stats.presents++;
    _compositor_internal__stats.gpu_groups++;
    renderer_backend__submit_layer(texture, corners, node->effective_opacity, (blend_mode == 2) ? 3 : blend_mode);
}

/**
 * @brief give a group surface defined pixels once (a top group in layer mode defers its clear past the texture cache lookup)
 * @param _compositor_internal__group_t* group -> the group
 * @return void
 */
static void _compositor_internal__ensure_cleared(_compositor_internal__group_t* group)
{
    if (group->cleared == FALSE && group->surface.pixels != NULL_POINTER)
    {
        stdlib__null_memory(group->surface.pixels, (int64)group->surface.w * group->surface.h * 4);
    }
    group->cleared = TRUE;
}

/**
 * @brief note that a draw reaches the current group surface (clearing it first when that is still pending)
 * @return void
 */
static inline void _compositor_internal__touch(void)
{
    _compositor_internal__group_t* group = &_compositor_internal__groups[_compositor_internal__depth];

    if (group->cleared == FALSE)
    {
        _compositor_internal__ensure_cleared(group);
    }
    group->painted = TRUE;
}

/**
 * @brief whether the layer texture cache already holds this decoration content for this node
 * @param gui_node_t* owner -> group node
 * @param uint64 content_version -> decoration content version
 * @return boole -> TRUE when present_layer will reuse the texture without reading the surface
 */
static boole _compositor_internal__layer_texture_cached(gui_node_t* owner, uint64 content_version)
{
    int i = 0;

    if (_compositor_internal__layer_mode == FALSE || content_version == 0)
    {
        return FALSE;
    }
    for (i = 0; i < COMPOSITOR_LAYER_TEXTURE_CAP; i++)
    {
        if (_compositor_internal__layer_textures[i].owner == owner && _compositor_internal__layer_textures[i].texture != NULL_POINTER && _compositor_internal__layer_textures[i].content_version == content_version)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief the one scratch surface layer mode rasters into (implicit backgrounds, read-back regions)
 * @param int x -> device origin
 * @param int y -> device origin
 * @param int w -> width
 * @param int h -> height
 * @param boole clear -> zero the pixels
 * @return _compositor_internal__surface_t* -> the surface, or NULL when it cannot be allocated
 */
static _compositor_internal__surface_t* _compositor_internal__scratch(int x, int y, int w, int h, boole clear)
{
    _compositor_internal__surface_t* surface = &_compositor_internal__layer_scratch;
    int64 bytes = (int64)w * h * 4;

    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
    {
        return NULL_POINTER;
    }
    if (surface->capacity < bytes)
    {
        GUI_FREE(surface->pixels);
        surface->pixels = GUI_MALLOC_T(bytes, MM_TYPE__GENERIC);
        surface->capacity = (surface->pixels == NULL_POINTER) ? 0 : bytes;
    }
    if (surface->pixels == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    surface->x = x;
    surface->y = y;
    surface->w = w;
    surface->h = h;
    if (clear == TRUE)
    {
        stdlib__null_memory(surface->pixels, bytes);
    }
    return surface;
}

/**
 * @brief FNV-1a over a byte range
 * @param uint64 h -> running hash
 * @param const void* bytes -> data
 * @param int64 length -> byte count
 * @return uint64 -> updated hash
 */
static uint64 _compositor_internal__hash(uint64 h, const void* bytes, int64 length)
{
    const ubyte* p = (const ubyte*)bytes;
    int64 i = 0;

    for (i = 0; i < length; i++)
    {
        h = (h ^ (uint64)p[i]) * 1099511628211ULL;
    }
    return h;
}

/**
 * @brief the backend texture for a layer, uploaded only when its content changed
 * @param gui_node_t* owner -> cache identity (a node, or a private address for transient layers)
 * @param uint64 content_version -> decoration content version, zero when unversioned
 * @param uint64 key -> implicit background key, zero when none
 * @param const ubyte* pixels -> premultiplied surface pixels
 * @param int w -> width
 * @param int h -> height
 * @param boole* reused -> set TRUE when no upload was needed
 * @return void* -> backend texture, or NULL
 */
static void* _compositor_internal__layer_texture(gui_node_t* owner, uint64 content_version, uint64 key, const ubyte* pixels, int w, int h, boole* reused)
{
    _compositor_internal__layer_texture_t* entry = NULL_POINTER;
    _compositor_internal__layer_texture_t* oldest = NULL_POINTER;
    void* texture = NULL_POINTER;
    int64 bytes = (int64)w * h * 4;
    int start = (int)((((nuint)owner) >> 4) % COMPOSITOR_LAYER_TEXTURE_CAP);
    int i = 0;
    int slot = 0;

    *reused = FALSE;
    // the owner's slot is found from its address; the probe wraps once
    for (i = 0; i < COMPOSITOR_LAYER_TEXTURE_CAP; i++)
    {
        slot = (start + i) % COMPOSITOR_LAYER_TEXTURE_CAP;
        if (_compositor_internal__layer_textures[slot].owner == owner)
        {
            entry = &_compositor_internal__layer_textures[slot];
            break;
        }
        if (_compositor_internal__layer_textures[slot].used != _compositor_internal__frame && (oldest == NULL_POINTER || _compositor_internal__layer_textures[slot].used < oldest->used))
        {
            oldest = &_compositor_internal__layer_textures[slot];
        }
    }
    if (entry == NULL_POINTER)
    {
        // every slot served this frame, or the layer is over budget: draw it
        // once through a transient texture and keep the cache as it is
        if (oldest == NULL_POINTER || bytes > MEEK_UI_LAYER_TEXTURE_CACHE_BYTES || _compositor_internal__layer_transient_count >= COMPOSITOR_LAYER_TRANSIENT_CAP)
        {
            if (_compositor_internal__layer_transient_count >= COMPOSITOR_LAYER_TRANSIENT_CAP)
            {
                return NULL_POINTER;
            }
            texture = renderer_backend__create_layer_texture(pixels, w, h);
            _compositor_internal__stats.transients++;
            _compositor_internal__stats.uploads++;
            _compositor_internal__stats.upload_bytes += (uint64)bytes;
            if (texture != NULL_POINTER)
            {
                _compositor_internal__layer_transients[_compositor_internal__layer_transient_count++] = texture;
            }
            return texture;
        }
        entry = oldest;
        if (entry->texture != NULL_POINTER)
        {
            renderer_backend__destroy_texture(entry->texture);
            _compositor_internal__layer_texture_bytes -= (int64)entry->w * entry->h * 4;
        }
        stdlib__null_memory(entry, sizeof(*entry));
        entry->owner = owner;
    }
    if (entry->texture != NULL_POINTER && entry->w == w && entry->h == h)
    {
        if ((content_version != 0 && entry->content_version == content_version) || (key != 0 && entry->key == key))
        {
            *reused = TRUE;
            entry->used = _compositor_internal__frame;
            _compositor_internal__stats.texture_hits++;
            return entry->texture;
        }
        _compositor_internal__stats.uploads++;
        _compositor_internal__stats.upload_bytes += (uint64)bytes;
        if (renderer_backend__update_layer_texture(entry->texture, pixels, w, h) == FALSE)
        {
            renderer_backend__destroy_texture(entry->texture);
            _compositor_internal__layer_texture_bytes -= (int64)entry->w * entry->h * 4;
            entry->texture = NULL_POINTER;
        }
    }
    else if (entry->texture != NULL_POINTER)
    {
        renderer_backend__destroy_texture(entry->texture);
        _compositor_internal__layer_texture_bytes -= (int64)entry->w * entry->h * 4;
        entry->texture = NULL_POINTER;
    }
    if (entry->texture == NULL_POINTER)
    {
        // make room: entries unused this frame go first, oldest first
        while (_compositor_internal__layer_texture_bytes + bytes > MEEK_UI_LAYER_TEXTURE_CACHE_BYTES)
        {
            oldest = NULL_POINTER;
            for (i = 0; i < COMPOSITOR_LAYER_TEXTURE_CAP; i++)
            {
                if (&_compositor_internal__layer_textures[i] != entry && _compositor_internal__layer_textures[i].texture != NULL_POINTER && _compositor_internal__layer_textures[i].used != _compositor_internal__frame && (oldest == NULL_POINTER || _compositor_internal__layer_textures[i].used < oldest->used))
                {
                    oldest = &_compositor_internal__layer_textures[i];
                }
            }
            if (oldest == NULL_POINTER)
            {
                break;
            }
            renderer_backend__destroy_texture(oldest->texture);
            _compositor_internal__layer_texture_bytes -= (int64)oldest->w * oldest->h * 4;
            stdlib__null_memory(oldest, sizeof(*oldest));
        }
        if (_compositor_internal__layer_texture_bytes + bytes > MEEK_UI_LAYER_TEXTURE_CACHE_BYTES)
        {
            stdlib__null_memory(entry, sizeof(*entry));
            if (_compositor_internal__layer_transient_count >= COMPOSITOR_LAYER_TRANSIENT_CAP)
            {
                return NULL_POINTER;
            }
            texture = renderer_backend__create_layer_texture(pixels, w, h);
            _compositor_internal__stats.transients++;
            _compositor_internal__stats.uploads++;
            _compositor_internal__stats.upload_bytes += (uint64)bytes;
            if (texture != NULL_POINTER)
            {
                _compositor_internal__layer_transients[_compositor_internal__layer_transient_count++] = texture;
            }
            return texture;
        }
        entry->texture = renderer_backend__create_layer_texture(pixels, w, h);
        entry->w = w;
        entry->h = h;
        _compositor_internal__stats.uploads++;
        _compositor_internal__stats.upload_bytes += (uint64)bytes;
        if (entry->texture != NULL_POINTER)
        {
            _compositor_internal__layer_texture_bytes += bytes;
        }
    }
    entry->content_version = content_version;
    entry->key = key;
    entry->used = _compositor_internal__frame;
    return entry->texture;
}

/**
 * @brief destroy layer textures unused for two frames, or all of them
 * @param boole all -> TRUE at shutdown
 * @return void
 */
static void _compositor_internal__layer_textures_age(boole all)
{
    int i = 0;

    // the backend finished the frame: transient textures are free to go
    for (i = 0; i < _compositor_internal__layer_transient_count; i++)
    {
        renderer_backend__destroy_texture(_compositor_internal__layer_transients[i]);
    }
    _compositor_internal__layer_transient_count = 0;
    for (i = 0; i < COMPOSITOR_LAYER_TEXTURE_CAP; i++)
    {
        if (_compositor_internal__layer_textures[i].texture != NULL_POINTER && (all == TRUE || _compositor_internal__layer_textures[i].used + 2 < _compositor_internal__frame))
        {
            renderer_backend__destroy_texture(_compositor_internal__layer_textures[i].texture);
            _compositor_internal__layer_texture_bytes -= (int64)_compositor_internal__layer_textures[i].w * _compositor_internal__layer_textures[i].h * 4;
            stdlib__null_memory(&_compositor_internal__layer_textures[i], sizeof(_compositor_internal__layer_textures[i]));
        }
    }
    if (all == TRUE)
    {
        _compositor_internal__layer_texture_bytes = 0;
    }
}

/**
 * @brief draw a finished top-level group surface as one textured quad through the node's transform
 * @param gui_node_t* node -> group owner
 * @param _compositor_internal__surface_t* surface -> premultiplied group pixels
 * @param float scale -> logical scale
 * @param uint64 content_version -> decoration content version, zero when uncached
 * @return void
 */
static void _compositor_internal__present_layer(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, uint64 content_version)
{
    float matrix[9];
    float corners[12];
    float sx = 0.0f;
    float sy = 0.0f;
    boole behind = FALSE;
    boole reused = FALSE;
    void* texture = NULL_POINTER;
    int blend_mode = 0;
    int i = 0;

    stdlib__null_memory(matrix, sizeof(matrix));
    stdlib__null_memory(corners, sizeof(corners));
    if (surface->pixels == NULL_POINTER || surface->w <= 0 || surface->h <= 0)
    {
        return;
    }
    blend_mode = _compositor_internal__blend_mode(node);
    _compositor_internal__matrix(node, scale, matrix);
    // the surface corners through the homography, kept homogeneous so the
    // backend interpolates a perspective quad correctly
    for (i = 0; i < 4; i++)
    {
        sx = (float)surface->x + (((i == 1 || i == 2)) ? (float)surface->w : 0.0f);
        sy = (float)surface->y + (((i == 2 || i == 3)) ? (float)surface->h : 0.0f);
        corners[i * 3 + 0] = matrix[0] * sx + matrix[1] * sy + matrix[2];
        corners[i * 3 + 1] = matrix[3] * sx + matrix[4] * sy + matrix[5];
        corners[i * 3 + 2] = matrix[6] * sx + matrix[7] * sy + matrix[8];
        if (corners[i * 3 + 2] <= 0.00001f)
        {
            behind = TRUE;
        }
        // A CPU child can finish into a live GPU page. Preserve homogeneous
        // coordinates while moving the quad to that render target's origin.
        if (_compositor_internal__gpu_group())
        {
            corners[i * 3 + 0] -= _compositor_internal__groups[_compositor_internal__depth].surface.x * corners[i * 3 + 2];
            corners[i * 3 + 1] -= _compositor_internal__groups[_compositor_internal__depth].surface.y * corners[i * 3 + 2];
        }
    }
    // overlay and difference need the backdrop; a corner behind the eye
    // needs the CPU's clamped mapping: both compose on a read-back.
    // multiply over an opaque backdrop is the fixed-function state
    // (DEST_COLOR, INV_SRC_ALPHA), backend blend 3
    if (blend_mode >= 3 || behind == TRUE)
    {
        _compositor_internal__present_readback(node, surface, scale, content_version);
        return;
    }
    if (blend_mode == 2)
    {
        blend_mode = 3;
    }
    texture = _compositor_internal__layer_texture(node, content_version, 0, surface->pixels, surface->w, surface->h, &reused);
    if (texture == NULL_POINTER)
    {
        return;
    }
    _compositor_internal__stats.presents++;
    renderer_backend__submit_layer(texture, corners, node->effective_opacity, blend_mode);
}

/**
 * @brief compose a group over a read-back of the back buffer and write the region back
 * @param gui_node_t* node -> group owner
 * @param _compositor_internal__surface_t* surface -> premultiplied group pixels
 * @param float scale -> logical scale
 * @param uint64 content_version -> decoration content version
 * @return void
 */
static void _compositor_internal__present_readback(gui_node_t* node, _compositor_internal__surface_t* surface, float scale, uint64 content_version)
{
    _compositor_internal__surface_t saved;
    _compositor_internal__surface_t* region = NULL_POINTER;
    float matrix[9];
    float corners[12];
    float px = 0.0f;
    float py = 0.0f;
    gui_rect_t bounds;
    float right = -1000000.0f;
    float bottom = -1000000.0f;
    boole reused = FALSE;
    void* texture = NULL_POINTER;
    int left = 0;
    int top = 0;
    int w = 0;
    int h = 0;
    int i = 0;

    stdlib__null_memory(&saved, sizeof(saved));
    stdlib__null_memory(matrix, sizeof(matrix));
    stdlib__null_memory(corners, sizeof(corners));
    stdlib__null_memory(&bounds, sizeof(bounds));
    _compositor_internal__matrix(node, scale, matrix);
    bounds.x = 1000000.0f;
    bounds.y = 1000000.0f;
    for (i = 0; i < 4; i++)
    {
        _compositor_internal__project(matrix, surface->x + ((i & 1) != 0 ? surface->w : 0), surface->y + ((i & 2) != 0 ? surface->h : 0), &px, &py);
        bounds.x = stdlib__fminf(bounds.x, px);
        bounds.y = stdlib__fminf(bounds.y, py);
        right = stdlib__fmaxf(right, px);
        bottom = stdlib__fmaxf(bottom, py);
    }
    bounds.w = right - bounds.x;
    bounds.h = bottom - bounds.y;
    bounds = _compositor_internal__clip(bounds);
    if (bounds.w <= 0.0f || bounds.h <= 0.0f)
    {
        return;
    }
    left = (int)stdlib__floorf(bounds.x);
    top = (int)stdlib__floorf(bounds.y);
    w = (int)stdlib__ceilf(bounds.x + bounds.w) - left;
    h = (int)stdlib__ceilf(bounds.y + bounds.h) - top;
    region = _compositor_internal__scratch(left, top, w, h, FALSE);
    if (region == NULL_POINTER)
    {
        return;
    }
    if (renderer_backend__read_region(left, top, w, h, region->pixels) == FALSE)
    {
        return;
    }
    _compositor_internal__stats.readbacks++;
    _compositor_internal__stats.presents++;
    // the region stands in for the depth-0 surface while the general
    // compose (any transform, any blend mode) runs into it
    saved = _compositor_internal__groups[0].surface;
    _compositor_internal__groups[0].surface = *region;
    _compositor_internal__no_acceleration = TRUE;
    _compositor_internal__compose(node, surface, scale, content_version);
    _compositor_internal__no_acceleration = FALSE;
    _compositor_internal__groups[0].surface = saved;
    texture = _compositor_internal__layer_texture(node, 0, 0, region->pixels, w, h, &reused);
    if (texture == NULL_POINTER)
    {
        return;
    }
    corners[0] = (float)left;
    corners[1] = (float)top;
    corners[2] = 1.0f;
    corners[3] = (float)(left + w);
    corners[4] = (float)top;
    corners[5] = 1.0f;
    corners[6] = (float)(left + w);
    corners[7] = (float)(top + h);
    corners[8] = 1.0f;
    corners[9] = (float)left;
    corners[10] = (float)(top + h);
    corners[11] = 1.0f;
    renderer_backend__submit_layer(texture, corners, 1.0f, 2);
}

/**
 * @brief the tile grid of one background layer the backend draws, covering the visible clip
 * @param const _compositor_internal__layer_t* layer -> tracked layer (device pixels relative to the box)
 * @param gui_rect_t box -> the node's box in the backend's coordinates
 * @param gui_rect_t clip -> the visible part of the box in the same coordinates
 * @param gui_rect_t* tile -> receives the first tile
 * @param int* columns -> receives the tiles per row (zero when nothing is visible)
 * @param int* rows -> receives the tile rows
 * @return boole -> FALSE when the layer keeps the CPU sampler (a tiny tile, a repeating radial, more cycles or tiles than the tessellator and the cap allow)
 */
static boole _compositor_internal__tile_grid(const _compositor_internal__layer_t* layer, gui_rect_t box, gui_rect_t clip, gui_rect_t* tile, int* columns, int* rows)
{
    float period = 0.0f;
    float cycles = 0.0f;
    float k = 0.0f;

    *columns = 0;
    *rows = 0;
    stdlib__null_memory(tile, sizeof(*tile));
    if (layer->w < 0.5f || layer->h < 0.5f)
    {
        return FALSE;
    }
    if (layer->is_gradient == TRUE && layer->source.repeating == TRUE)
    {
        // the tessellator repeats linear and conic ramps within its
        // boundary room; a repeating radial keeps the CPU sampler
        if (layer->source.kind == GUI_GRADIENT_KIND__RADIAL || layer->gradient.count < 2)
        {
            return FALSE;
        }
        period = layer->gradient.pos[layer->gradient.count - 1] - layer->gradient.pos[0];
        if (period <= 0.00001f)
        {
            return FALSE;
        }
        cycles = stdlib__ceilf(1.0f / period) + 3.0f;
        if (cycles * (float)layer->gradient.count > 600.0f)
        {
            return FALSE;
        }
    }
    tile->x = box.x + layer->x;
    tile->y = box.y + layer->y;
    tile->w = layer->w;
    tile->h = layer->h;
    // a repeated axis starts at the last tile edge before the clip and
    // covers it; an unrepeated one is the single tile, when visible
    if (layer->repeat_x == TRUE)
    {
        k = stdlib__floorf((clip.x - tile->x) / tile->w);
        tile->x += k * tile->w;
        *columns = (int)stdlib__ceilf((clip.x + clip.w - tile->x) / tile->w);
    }
    else if (tile->x < clip.x + clip.w && tile->x + tile->w > clip.x)
    {
        *columns = 1;
    }
    if (layer->repeat_y == TRUE)
    {
        k = stdlib__floorf((clip.y - tile->y) / tile->h);
        tile->y += k * tile->h;
        *rows = (int)stdlib__ceilf((clip.y + clip.h - tile->y) / tile->h);
    }
    else if (tile->y < clip.y + clip.h && tile->y + tile->h > clip.y)
    {
        *rows = 1;
    }
    if (*columns <= 0 || *rows <= 0)
    {
        *columns = 0;
        *rows = 0;
        return TRUE;
    }
    if ((int64)*columns * (int64)*rows > COMPOSITOR_TILE_CAP)
    {
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief draw a node's tracked background layers with the backend as tiles: the color first, then the layers last to first (the CPU sampler's order)
 * @param gui_node_t* node -> the node
 * @param const _compositor_internal__layer_t* layers -> tracked layers
 * @param int count -> layer count
 * @param gui_rect_t box -> the node's box in the backend's coordinates
 * @param gui_rect_t clip -> the visible part of the box in the same coordinates
 * @param float scale -> logical scale
 * @param float opacity -> multiplies every draw
 * @return boole -> FALSE when a layer keeps the CPU sampler (nothing was drawn)
 */
static boole _compositor_internal__background_tiles(gui_node_t* node, const _compositor_internal__layer_t* layers, int count, gui_rect_t box, gui_rect_t clip, float scale, float opacity)
{
    gui_rect_t tiles[COMPOSITOR_LAYER_CAP];
    gui_color_t color;
    float radius = node->resolved.radius * scale;
    int columns[COMPOSITOR_LAYER_CAP];
    int rows[COMPOSITOR_LAYER_CAP];
    int i = 0;

    // the grids are filled for the first count layers before they are read
    stdlib__null_memory(&color, sizeof(color));
    if (count <= 0 || count > COMPOSITOR_LAYER_CAP || renderer_backend__supports_layers() == FALSE)
    {
        return FALSE;
    }
    for (i = 0; i < count; i++)
    {
        if (_compositor_internal__tile_grid(&layers[i], box, clip, &tiles[i], &columns[i], &rows[i]) == FALSE)
        {
            return FALSE;
        }
    }
    if (node->resolved.has_background_color == TRUE && node->resolved.background_color.a > 0.0f)
    {
        color = node->resolved.background_color;
        color.a *= opacity;
        renderer_backend__submit_rect(box, color, radius);
    }
    for (i = count - 1; i >= 0; i--)
    {
        renderer_backend__submit_tiles(box, radius, tiles[i], columns[i], rows[i], (layers[i].is_gradient == TRUE) ? &layers[i].source : NULL_POINTER, layers[i].color, scale, opacity);
    }
    _compositor_internal__stats.tile_draws++;
    return TRUE;
}

/**
 * @brief the extended background of a plain node at depth 0: a solid one goes to the backend, the rest becomes an implicit layer cached by its inputs
 * @param gui_node_t* node -> node whose background is painted
 * @param float scale -> logical scale
 * @return boole -> TRUE when the background is handled (the caller draws no plain color or gradient)
 */
static boole _compositor_internal__background_layer(gui_node_t* node, float scale)
{
    static const int properties[COMPOSITOR_DECORATION_PROPERTIES] = {
        MOTION_PROP__BACKGROUND, MOTION_PROP__BACKGROUND_IMAGE, MOTION_PROP__BACKGROUND_SIZE, MOTION_PROP__BACKGROUND_POSITION,
        MOTION_PROP__BACKGROUND_REPEAT, MOTION_PROP__BACKGROUND_CLIP, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, MOTION_PROP__FILTER,
        MOTION_PROP__MASK, MOTION_PROP__MASK_IMAGE, MOTION_PROP__MASK_SIZE, MOTION_PROP__MASK_POSITION, MOTION_PROP__MASK_REPEAT,
        MOTION_PROP__MASK_COMPOSITE, MOTION_PROP__CLIP_PATH, MOTION_PROP__BORDER_RADIUS, MOTION_PROP__TEXT_SHADOW, MOTION_PROP__WEBKIT_TEXT_STROKE
    };
    _compositor_internal__layer_t layers[COMPOSITOR_LAYER_CAP];
    _compositor_internal__surface_t saved;
    _compositor_internal__surface_t* surface = NULL_POINTER;
    _compositor_internal__layer_texture_t* entry = NULL_POINTER;
    gui_rect_t clip;
    gui_color_t color;
    char text_clip[64];
    char value[2048];
    float corners[12];
    float radius = node->resolved.radius * scale;
    uint64 key = 14695981039346656037ULL;
    void* texture = NULL_POINTER;
    boole uniform = TRUE;
    boole reused = FALSE;
    boole handled = FALSE;
    int count = 0;
    int left = 0;
    int top = 0;
    int w = 0;
    int h = 0;
    int i = 0;
    int start = (int)((((nuint)node) >> 4) % COMPOSITOR_LAYER_TEXTURE_CAP);

    // _compositor_internal__layers fills count entries and nulls the rest;
    // the strings are filled by motion__value_id before they are read
    stdlib__null_memory(&saved, sizeof(saved));
    stdlib__null_memory(&clip, sizeof(clip));
    stdlib__null_memory(&color, sizeof(color));
    text_clip[0] = 0;
    value[0] = 0;
    stdlib__null_memory(corners, sizeof(corners));
    if (_compositor_internal__enabled == FALSE)
    {
        return FALSE;
    }
    // the same admission as _compositor_internal__background: nothing
    // visible, a text-clipped background (its text group paints it) and no
    // tracked layers are decided here without a raster
    clip = _compositor_internal__clip(node->bounds);
    if (clip.w <= 0.0f || clip.h <= 0.0f)
    {
        return TRUE;
    }
    motion__value_id(node, MOTION_PROP__BACKGROUND_CLIP, text_clip, sizeof(text_clip));
    if (text_clip[0] == 0)
    {
        motion__value_id(node, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, text_clip, sizeof(text_clip));
    }
    if (stdlib__strcmp(text_clip, "text") == 0)
    {
        return TRUE;
    }
    count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND_IMAGE, layers, COMPOSITOR_LAYER_CAP, scale);
    if (count == 0)
    {
        count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND, layers, COMPOSITOR_LAYER_CAP, scale);
    }
    if (count == 0 && !node->resolved.has_background_image && (node->resolved.has_background_color == FALSE || node->resolved.background_color.a <= 0.0f))
    {
        return TRUE;
    }
    if (count == 0)
    {
        return FALSE;
    }
    for (i = 0; i < count; i++)
    {
        if (layers[i].is_gradient == TRUE || layers[i].repeat_x == FALSE || layers[i].repeat_y == FALSE)
        {
            uniform = FALSE;
            break;
        }
    }
    if (uniform == TRUE)
    {
        // solid layers over the background color: one backend rect
        if (node->resolved.has_background_color == TRUE)
        {
            color = node->resolved.background_color;
        }
        for (i = count - 1; i >= 0; i--)
        {
            color = _compositor_internal__over(layers[i].color, color);
        }
        color.a *= node->effective_opacity;
        renderer_backend__submit_rect(node->bounds, color, radius);
        return TRUE;
    }
    // gradients and solids the backend can tile are drawn straight into the
    // frame: no raster and no texture, whatever animates
    if (_compositor_internal__background_tiles(node, layers, count, node->bounds, clip, scale, node->effective_opacity) == TRUE)
    {
        return TRUE;
    }
    // the implicit layer covers the visible part of the box; its raster is a
    // pure function of these inputs, so an unchanged key reuses the texture
    left = (int)stdlib__floorf(clip.x);
    top = (int)stdlib__floorf(clip.y);
    w = (int)stdlib__ceilf(clip.x + clip.w) - left;
    h = (int)stdlib__ceilf(clip.y + clip.h) - top;
    for (i = 0; i < COMPOSITOR_DECORATION_PROPERTIES; i++)
    {
        motion__value_id(node, properties[i], value, sizeof(value));
        key = _compositor_internal__hash(key, value, (int64)stdlib__strlen(value) + 1);
    }
    key = _compositor_internal__hash(key, &node->bounds, sizeof(node->bounds));
    key = _compositor_internal__hash(key, &clip, sizeof(clip));
    key = _compositor_internal__hash(key, &scale, sizeof(scale));
    key = _compositor_internal__hash(key, &node->resolved.background_color, sizeof(node->resolved.background_color));
    key = _compositor_internal__hash(key, &node->resolved.has_background_color, sizeof(node->resolved.has_background_color));
    key = _compositor_internal__hash(key, &radius, sizeof(radius));
    key = _compositor_internal__hash(key, &node->effective_opacity, sizeof(node->effective_opacity));
    if (key == 0)
    {
        key = 1;
    }
    corners[0] = (float)left;
    corners[1] = (float)top;
    corners[2] = 1.0f;
    corners[3] = (float)(left + w);
    corners[4] = (float)top;
    corners[5] = 1.0f;
    corners[6] = (float)(left + w);
    corners[7] = (float)(top + h);
    corners[8] = 1.0f;
    corners[9] = (float)left;
    corners[10] = (float)(top + h);
    corners[11] = 1.0f;
    for (i = 0; i < COMPOSITOR_LAYER_TEXTURE_CAP; i++)
    {
        entry = &_compositor_internal__layer_textures[(start + i) % COMPOSITOR_LAYER_TEXTURE_CAP];
        if (entry->owner == node)
        {
            if (entry->texture != NULL_POINTER && entry->key == key && entry->w == w && entry->h == h)
            {
                entry->used = _compositor_internal__frame;
                _compositor_internal__stats.implicit_hits++;
                renderer_backend__submit_layer(entry->texture, corners, 1.0f, 0);
                return TRUE;
            }
            break;
        }
    }
    entry = NULL_POINTER;
    surface = _compositor_internal__scratch(left, top, w, h, TRUE);
    if (surface == NULL_POINTER)
    {
        return FALSE;
    }
    // the scratch stands in as the depth-1 surface while the existing
    // rasterizer (float sample cache, conic coordinate cache, compute
    // fusion) paints the background into it
    saved = _compositor_internal__groups[1].surface;
    _compositor_internal__groups[1].surface = *surface;
    _compositor_internal__depth = 1;
    _compositor_internal__no_acceleration = TRUE;
    _compositor_internal__no_float_cache = TRUE;
    _compositor_internal__stats.implicit_rasters++;
    handled = _compositor_internal__background(node, scale, FALSE);
    _compositor_internal__no_float_cache = FALSE;
    _compositor_internal__no_acceleration = FALSE;
    _compositor_internal__depth = 0;
    _compositor_internal__groups[1].surface = saved;
    if (handled == FALSE)
    {
        return FALSE;
    }
    texture = _compositor_internal__layer_texture(node, 0, key, surface->pixels, w, h, &reused);
    if (texture == NULL_POINTER)
    {
        return FALSE;
    }
    renderer_backend__submit_layer(texture, corners, 1.0f, 0);
    return TRUE;
}

/**
 * @brief backdrop-filter blur at depth 0: the backend's GPU filter, else the back buffer region through the compositor's kernel, written back
 * @param gui_rect_t rect -> node bounds
 * @param float sigma -> blur sigma in device pixels
 * @return void
 */
static void _compositor_internal__blur_layer(gui_rect_t rect, float sigma)
{
    _compositor_internal__surface_t* region = NULL_POINTER;
    gui_rect_t clip;
    float corners[12];
    boole reused = FALSE;
    void* texture = NULL_POINTER;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    stdlib__null_memory(&clip, sizeof(clip));
    stdlib__null_memory(corners, sizeof(corners));
    clip = _compositor_internal__clip(rect);
    // the same rounding as the frame-mode blur
    x = (int)stdlib__ceilf(clip.x);
    y = (int)stdlib__ceilf(clip.y);
    w = (int)stdlib__floorf(clip.w);
    h = (int)stdlib__floorf(clip.h);
    if (w <= 0 || h <= 0)
    {
        return;
    }
    if (effect_acceleration__native_blur_enabled() == TRUE)
    {
        // at and below this sigma the filter changes no pixel (the first
        // frame of a blur that fades in); the read-back below would cost a
        // frame for nothing
        if (sigma <= 0.1f)
        {
            return;
        }
        if (renderer_backend__blur_layer(x, y, w, h, _compositor_internal__blur_radius(sigma)) == TRUE)
        {
            _compositor_internal__stats.blur_layers++;
            _compositor_internal__blurs_gpu++;
            return;
        }
    }
    region = _compositor_internal__scratch(x, y, w, h, FALSE);
    if (region == NULL_POINTER || renderer_backend__read_region(x, y, w, h, region->pixels) == FALSE)
    {
        return;
    }
    _compositor_internal__blurs_cpu++;
    _compositor_internal__blur(region, sigma);
    _compositor_internal__stats.readbacks++;
    _compositor_internal__stats.blur_layers++;
    // identity only, never dereferenced: one cache slot per blur rect
    texture = _compositor_internal__layer_texture((gui_node_t*)(nuint)(0x10000u + (nuint)(x & 0xFFFF) * 0x10000u + (nuint)(y & 0xFFFF)), 0, 0, region->pixels, w, h, &reused);
    if (texture == NULL_POINTER)
    {
        return;
    }
    corners[0] = (float)x;
    corners[1] = (float)y;
    corners[2] = 1.0f;
    corners[3] = (float)(x + w);
    corners[4] = (float)y;
    corners[5] = 1.0f;
    corners[6] = (float)(x + w);
    corners[7] = (float)(y + h);
    corners[8] = 1.0f;
    corners[9] = (float)x;
    corners[10] = (float)(y + h);
    corners[11] = 1.0f;
    renderer_backend__submit_layer(texture, corners, 1.0f, 2);
}

/**
 * @brief backdrop blur of a frame the backend draws itself (compositor off): the backend's GPU filter, else its CPU routine
 * @param gui_rect_t rect -> node bounds
 * @param float sigma -> blur sigma in device pixels
 * @return void
 */
static void _compositor_internal__blur_direct(gui_rect_t rect, float sigma)
{
    gui_rect_t area;
    gui_rect_t clip;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    stdlib__null_memory(&area, sizeof(area));
    stdlib__null_memory(&clip, sizeof(clip));
    // GUI_EFFECT_ACCELERATION__CPU: the backend's read-back, CPU filter and upload, unchanged
    if (effect_acceleration__native_blur_enabled() == FALSE)
    {
        _compositor_internal__blurs_cpu++;
        renderer_backend__blur_region(rect, sigma);
        return;
    }
    // at and below this sigma the filter changes no pixel (the first frame of
    // a blur that fades in); a read-back would cost a frame for nothing
    if (sigma <= 0.1f)
    {
        return;
    }
    // the filter writes the whole rectangle it is given, so the rectangle is
    // cut to the viewport and to the scissor here; the same rounding as the
    // layer-mode blur
    area.w = (float)_compositor_internal__viewport_w;
    area.h = (float)_compositor_internal__viewport_h;
    clip = _compositor_internal__intersection(rect, area);
    if (_compositor_internal__clip_count > 0)
    {
        clip = _compositor_internal__intersection(clip, _compositor_internal__clips[_compositor_internal__clip_count - 1]);
    }
    x = (int)stdlib__ceilf(clip.x);
    y = (int)stdlib__ceilf(clip.y);
    w = (int)stdlib__floorf(clip.w);
    h = (int)stdlib__floorf(clip.h);
    if (w <= 0 || h <= 0)
    {
        return;
    }
    if (renderer_backend__blur_layer(x, y, w, h, _compositor_internal__blur_radius(sigma)) == TRUE)
    {
        _compositor_internal__blurs_gpu++;
        return;
    }
    // no GPU filter in this backend, or a draw target it refuses
    _compositor_internal__blurs_cpu++;
    renderer_backend__blur_region(rect, sigma);
}

/**
 * @brief how many backdrop blurs ran since the start, by what filtered them (renderer.h)
 * @param uint64* gpu -> optional: blurs the backend's GPU filter did
 * @param uint64* cpu -> optional: blurs a CPU routine did
 * @return void
 */
void renderer__backdrop_blur_counts(uint64* gpu, uint64* cpu)
{
    if (gpu != NULL_POINTER)
    {
        *gpu = _compositor_internal__blurs_gpu;
    }
    if (cpu != NULL_POINTER)
    {
        *cpu = _compositor_internal__blurs_cpu;
    }
}

/**
 * @brief finish a node effect surface and restore its parent's draw context
 * @param gui_node_t* node -> current node
 * @param float scale -> logical scale
 * @return void
 */
void compositor__end_node(gui_node_t* node, float scale)
{
    _compositor_internal__group_t* group = &_compositor_internal__groups[_compositor_internal__depth];
    _compositor_internal__decoration_cache_t* entry = NULL_POINTER;
    int i = 0;

    if (_compositor_internal__enabled == FALSE || _compositor_internal__depth <= 0 || group->node != node)
    {
        return;
    }
    if (group->gpu == TRUE)
    {
        void* texture = renderer_backend__end_layer();
        int blend_mode = _compositor_internal__blend_mode(node);

        _compositor_internal__text_node = group->text_node;
        node->effective_opacity = group->opacity;
        if (_compositor_internal__clip_count > 0)
        {
            _compositor_internal__clips[_compositor_internal__clip_count - 1] = group->clip;
        }
        _compositor_internal__depth--;
        _compositor_internal__present_texture(node, &group->surface, scale, texture, blend_mode);
        return;
    }
    if (group->reused == FALSE && group->fused == FALSE)
    {
        _compositor_internal__ensure_cleared(group);
        _compositor_internal__effects(node, &group->surface, scale);
        if (group->decoration > 0)
        {
            entry = &_compositor_internal__decorations[group->decoration - 1];
            stdlib__memcpy(entry->pixels, group->surface.pixels, entry->bytes);
            entry->valid = TRUE;
            // At counter exhaustion disable new GPU cache entries rather than
            // reusing an identity. Existing cached pixels remain valid.
            entry->content_version = _compositor_internal__content_version == (uint64)-1 ? 0 : ++_compositor_internal__content_version;
            group->content_version = entry->content_version;
            // A finished decoration supersedes this node's much larger float
            // background cache. Let other visible backgrounds use that budget.
            for (i = 0; i < COMPOSITOR_BACKGROUND_CACHE_CAP; i++)
            {
                if (_compositor_internal__background_cache[i].owner == node)
                {
                    _compositor_internal__background_bytes -= _compositor_internal__background_cache[i].bytes;
                    GUI_FREE(_compositor_internal__background_cache[i].pixels);
                    stdlib__null_memory(&_compositor_internal__background_cache[i], sizeof(_compositor_internal__background_cache[i]));
                }
            }
        }
    }
    _compositor_internal__text_node = group->text_node;
    node->effective_opacity = group->opacity;
    if (_compositor_internal__clip_count > 0)
    {
        _compositor_internal__clips[_compositor_internal__clip_count - 1] = group->clip;
    }
    _compositor_internal__depth--;
    if (group->fused == FALSE)
    {
        if (_compositor_internal__layer_mode == TRUE && (_compositor_internal__depth == 0 || _compositor_internal__gpu_group()))
        {
            // an untouched top group has nothing to draw (a fused or
            // texture-cached one is drawn from the cache)
            if (group->painted == TRUE || group->reused == TRUE || group->texture_cached == TRUE)
            {
                _compositor_internal__present_layer(node, &group->surface, scale, group->content_version);
            }
        }
        else
        {
            _compositor_internal__touch();
            _compositor_internal__compose(node, &group->surface, scale, group->content_version);
        }
    }
}

/**
 * @brief expand CSS one-to-four length notation
 * @param const char* value -> length list
 * @param float* result -> four side or corner values
 * @param float reference -> percentage reference in device pixels
 * @param float scale -> logical scale
 * @return void
 */
static void _compositor_internal__four(const char* value, float* result, float reference, float scale)
{
    const char* p = value;
    char token[128];
    int count = 0;

    stdlib__null_memory(token, sizeof(token));
    stdlib__null_memory(result, sizeof(float) * 4);
    while (*p != 0 && count < 4)
    {
        p = _compositor_internal__part(p, token, sizeof(token), ' ');
        result[count++] = motion__number(token, reference / scale) * scale;
    }
    if (count == 1)
    {
        result[1] = result[0];
        result[2] = result[0];
        result[3] = result[0];
    }
    else if (count == 2)
    {
        result[2] = result[0];
        result[3] = result[1];
    }
    else if (count == 3)
    {
        result[3] = result[1];
    }
}

/**
 * @brief apply CSS color matrices and blur in function-list order
 * @param _compositor_internal__surface_t* surface -> premultiplied pixels
 * @param const char* filter -> resolved filter list
 * @param float scale -> logical scale
 * @return void
 */
static void _compositor_internal__filter(_compositor_internal__surface_t* surface, const char* filter, float scale)
{
    const char* p = filter;
    const char* argument = NULL_POINTER;
    char token[256];
    float value = 0.0f;
    float a = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float nr = 0.0f;
    float ng = 0.0f;
    float nb = 0.0f;
    float luma = 0.0f;
    float sine = 0.0f;
    float cosine = 0.0f;
    int64 i = 0;
    int64 count = (int64)surface->w * surface->h;
    ubyte* pixel = NULL_POINTER;

    stdlib__null_memory(token, sizeof(token));
    while (*p != 0)
    {
        p = _compositor_internal__part(p, token, sizeof(token), ' ');
        argument = stdlib__strchr(token, '(');
        if (argument == NULL_POINTER)
        {
            continue;
        }
        value = motion__number(argument + 1, 1.0f);
        if (stdlib__strncmp(token, "blur(", 5) == 0)
        {
            _compositor_internal__blur(surface, value * scale);
            continue;
        }
        sine = stdlib__sinf(value * 0.0174532925199433f);
        cosine = stdlib__cosf(value * 0.0174532925199433f);
        for (i = 0; i < count; i++)
        {
            pixel = surface->pixels + i * 4;
            if (pixel[3] == 0)
            {
                continue;
            }
            a = pixel[3] / 255.0f;
            r = (float)pixel[0] / pixel[3];
            g = (float)pixel[1] / pixel[3];
            b = (float)pixel[2] / pixel[3];
            nr = r;
            ng = g;
            nb = b;
            luma = r * 0.213f + g * 0.715f + b * 0.072f;
            if (stdlib__strncmp(token, "hue-rotate(", 11) == 0)
            {
                nr = r * (0.213f + cosine * 0.787f - sine * 0.213f) + g * (0.715f - cosine * 0.715f - sine * 0.715f) + b * (0.072f - cosine * 0.072f + sine * 0.928f);
                ng = r * (0.213f - cosine * 0.213f + sine * 0.143f) + g * (0.715f + cosine * 0.285f + sine * 0.140f) + b * (0.072f - cosine * 0.072f - sine * 0.283f);
                nb = r * (0.213f - cosine * 0.213f - sine * 0.787f) + g * (0.715f - cosine * 0.715f + sine * 0.715f) + b * (0.072f + cosine * 0.928f + sine * 0.072f);
            }
            else if (stdlib__strncmp(token, "saturate(", 9) == 0 || stdlib__strncmp(token, "grayscale(", 10) == 0)
            {
                if (token[0] == 'g')
                {
                    value = 1.0f - _compositor_internal__clamp(value);
                }
                nr = luma + (r - luma) * value;
                ng = luma + (g - luma) * value;
                nb = luma + (b - luma) * value;
                if (token[0] == 'g')
                {
                    value = 1.0f - value;
                }
            }
            else if (stdlib__strncmp(token, "brightness(", 11) == 0)
            {
                nr *= value;
                ng *= value;
                nb *= value;
            }
            else if (stdlib__strncmp(token, "contrast(", 9) == 0)
            {
                nr = (nr - 0.5f) * value + 0.5f;
                ng = (ng - 0.5f) * value + 0.5f;
                nb = (nb - 0.5f) * value + 0.5f;
            }
            else if (stdlib__strncmp(token, "opacity(", 8) == 0)
            {
                a *= _compositor_internal__clamp(value);
            }
            else if (stdlib__strncmp(token, "invert(", 7) == 0)
            {
                nr += (1.0f - 2.0f * nr) * value;
                ng += (1.0f - 2.0f * ng) * value;
                nb += (1.0f - 2.0f * nb) * value;
            }
            pixel[0] = (ubyte)(_compositor_internal__clamp(nr) * a * 255.0f + 0.5f);
            pixel[1] = (ubyte)(_compositor_internal__clamp(ng) * a * 255.0f + 0.5f);
            pixel[2] = (ubyte)(_compositor_internal__clamp(nb) * a * 255.0f + 0.5f);
            pixel[3] = (ubyte)(a * 255.0f + 0.5f);
        }
    }
}

/**
 * @brief color glyph coverage with gradient fills, centered strokes and shadow layers
 * @param gui_node_t* node -> text owner
 * @param _compositor_internal__surface_t* surface -> glyph coverage surface
 * @param float scale -> logical scale
 * @return void
 */
static void _compositor_internal__text_effects(gui_node_t* node, _compositor_internal__surface_t* surface, float scale)
{
    _compositor_internal__surface_t shadow;
    _compositor_internal__layer_t layers[COMPOSITOR_LAYER_CAP];
    ubyte* coverage = NULL_POINTER;
    ubyte* output = NULL_POINTER;
    gui_color_t color;
    gui_color_t stroke;
    gui_color_t shadow_color;
    gui_color_t previous;
    char value[1024];
    char shadows[16][256];
    char token[256];
    const char* p = NULL_POINTER;
    const char* q = NULL_POINTER;
    float lengths[3];
    float width = 0.0f;
    float alpha = 0.0f;
    float dilation = 0.0f;
    float erosion = 0.0f;
    float a = 0.0f;
    int count = 0;
    int shadow_count = 0;
    int number = 0;
    int i = 0;
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
    int sx = 0;
    int sy = 0;
    int c = 0;
    int64 index = 0;
    int64 pixels = (int64)surface->w * surface->h;

    stdlib__null_memory(&shadow, sizeof(shadow));
    stdlib__null_memory(layers, sizeof(layers));
    stdlib__null_memory(&color, sizeof(color));
    stdlib__null_memory(&stroke, sizeof(stroke));
    stdlib__null_memory(&shadow_color, sizeof(shadow_color));
    stdlib__null_memory(&previous, sizeof(previous));
    stdlib__null_memory(value, sizeof(value));
    stdlib__null_memory(shadows, sizeof(shadows));
    stdlib__null_memory(token, sizeof(token));
    stdlib__null_memory(lengths, sizeof(lengths));
    if (node->text[0] == 0 || (motion__has_id(node, MOTION_PROP__BACKGROUND_CLIP) == FALSE && motion__has_id(node, MOTION_PROP__WEBKIT_BACKGROUND_CLIP) == FALSE && motion__has_id(node, MOTION_PROP__TEXT_SHADOW) == FALSE && motion__has_id(node, MOTION_PROP__WEBKIT_TEXT_STROKE) == FALSE))
    {
        return;
    }
    coverage = GUI_MALLOC_T(pixels, MM_TYPE__GENERIC);
    output = GUI_CALLOC_T(pixels, 4, MM_TYPE__GENERIC);
    if (coverage == NULL_POINTER || output == NULL_POINTER)
    {
        goto label_text_effects_end;
    }
    for (index = 0; index < pixels; index++)
    {
        coverage[index] = surface->pixels[index * 4 + 3];
    }
    motion__value_id(node, MOTION_PROP__TEXT_SHADOW, value, sizeof(value));
    p = value;
    while (*p != 0 && shadow_count < 16)
    {
        p = _compositor_internal__part(p, shadows[shadow_count++], sizeof(shadows[0]), ',');
    }
    shadow = *surface;
    shadow.pixels = GUI_CALLOC_T(pixels, 4, MM_TYPE__GENERIC);
    if (shadow.pixels != NULL_POINTER)
    {
        for (i = shadow_count - 1; i >= 0; i--)
        {
            stdlib__null_memory(lengths, sizeof(lengths));
            shadow_color = node->resolved.font_color;
            p = shadows[i];
            number = 0;
            while (*p != 0)
            {
                p = _compositor_internal__part(p, token, sizeof(token), ' ');
                if (parser_style__parse_color(token, &color) == TRUE)
                {
                    shadow_color = color;
                }
                else if (number < 3)
                {
                    lengths[number++] = motion__number(token, 0.0f) * scale;
                }
            }
            stdlib__null_memory(shadow.pixels, pixels * 4);
            for (y = 0; y < surface->h; y++)
            {
                sy = y - (int)lengths[1];
                for (x = 0; x < surface->w; x++)
                {
                    sx = x - (int)lengths[0];
                    if (sx < 0 || sy < 0 || sx >= surface->w || sy >= surface->h)
                    {
                        continue;
                    }
                    alpha = coverage[(int64)sy * surface->w + sx] / 255.0f * shadow_color.a;
                    index = ((int64)y * surface->w + x) * 4;
                    shadow.pixels[index] = (ubyte)(shadow_color.r * alpha * 255.0f + 0.5f);
                    shadow.pixels[index + 1] = (ubyte)(shadow_color.g * alpha * 255.0f + 0.5f);
                    shadow.pixels[index + 2] = (ubyte)(shadow_color.b * alpha * 255.0f + 0.5f);
                    shadow.pixels[index + 3] = (ubyte)(alpha * 255.0f + 0.5f);
                }
            }
            _compositor_internal__blur(&shadow, lengths[2] * 0.5f);
            for (index = 0; index < pixels * 4; index += 4)
            {
                a = shadow.pixels[index + 3] / 255.0f;
                for (c = 0; c < 4; c++)
                {
                    output[index + c] = (ubyte)(shadow.pixels[index + c] + output[index + c] * (1.0f - a) + 0.5f);
                }
            }
        }
    }
    motion__value_id(node, MOTION_PROP__BACKGROUND_CLIP, value, sizeof(value));
    if (value[0] == 0)
    {
        motion__value_id(node, MOTION_PROP__WEBKIT_BACKGROUND_CLIP, value, sizeof(value));
    }
    if (stdlib__strcmp(value, "text") == 0)
    {
        count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND_IMAGE, layers, COMPOSITOR_LAYER_CAP, scale);
        if (count == 0)
        {
            count = _compositor_internal__layers(node, MOTION_PROP__BACKGROUND, layers, COMPOSITOR_LAYER_CAP, scale);
        }
    }
    if (motion__value_id(node, MOTION_PROP__WEBKIT_TEXT_STROKE, value, sizeof(value)) == TRUE)
    {
        q = _compositor_internal__part(value, token, sizeof(token), ' ');
        width = stdlib__fminf(8.0f, motion__number(token, 0.0f) * scale * 0.5f);
        stroke = node->resolved.font_color;
        parser_style__parse_color(q, &stroke);
    }
    for (y = 0; y < surface->h; y++)
    {
        for (x = 0; x < surface->w; x++)
        {
            index = (int64)y * surface->w + x;
            alpha = coverage[index] / 255.0f;
            dilation = alpha;
            erosion = alpha;
            if (width > 0.0f)
            {
                for (dy = -(int)stdlib__ceilf(width); dy <= (int)stdlib__ceilf(width); dy++)
                {
                    sy = y + dy;
                    for (dx = -(int)stdlib__ceilf(width); dx <= (int)stdlib__ceilf(width); dx++)
                    {
                        sx = x + dx;
                        if (sx >= 0 && sy >= 0 && sx < surface->w && sy < surface->h && dx * dx + dy * dy <= (width + 0.5f) * (width + 0.5f))
                        {
                            dilation = stdlib__fmaxf(dilation, coverage[(int64)sy * surface->w + sx] / 255.0f);
                            erosion = stdlib__fminf(erosion, coverage[(int64)sy * surface->w + sx] / 255.0f);
                        }
                    }
                }
            }
            // Shadows are already in output; empty glyph/stroke coverage adds nothing.
            if (alpha == 0.0f && dilation == erosion)
            {
                continue;
            }
            color = node->resolved.font_color;
            if (count > 0)
            {
                stdlib__null_memory(&color, sizeof(color));
                for (i = count - 1; i >= 0; i--)
                {
                    color = _compositor_internal__over(_compositor_internal__layer_sample(&layers[i], surface->x + x + 0.5f - node->bounds.x, surface->y + y + 0.5f - node->bounds.y), color);
                }
            }
            color.a *= alpha;
            previous = stroke;
            previous.a *= stdlib__fmaxf(0.0f, dilation - erosion);
            color = _compositor_internal__over(color, previous);
            index *= 4;
            output[index] = (ubyte)(color.r * color.a * 255.0f + output[index] * (1.0f - color.a) + 0.5f);
            output[index + 1] = (ubyte)(color.g * color.a * 255.0f + output[index + 1] * (1.0f - color.a) + 0.5f);
            output[index + 2] = (ubyte)(color.b * color.a * 255.0f + output[index + 2] * (1.0f - color.a) + 0.5f);
            output[index + 3] = (ubyte)(color.a * 255.0f + output[index + 3] * (1.0f - color.a) + 0.5f);
        }
    }
    stdlib__memcpy(surface->pixels, output, pixels * 4);

label_text_effects_end:
    GUI_FREE(shadow.pixels);
    GUI_FREE(coverage);
    GUI_FREE(output);
}

/**
 * @brief apply a node's text, filter, mask, clipping and elliptical corner effects
 * @param gui_node_t* node -> styled owner
 * @param _compositor_internal__surface_t* surface -> isolated pixels
 * @param float scale -> logical scale
 * @return void
 */
/**
 * @brief parse the points of clip-path: polygon(): "x y, x y, ..." with px or % of the box (the closing parenthesis ends it)
 * @param const char* text -> the text after "polygon("
 * @param float* points -> receives x, y pairs in device pixels relative to the box
 * @param int cap -> the most points
 * @param float w -> box width in device pixels (the % reference of x)
 * @param float h -> box height (the % reference of y)
 * @param float scale -> logical scale for px values
 * @return int -> the number of points (fewer than three: no polygon)
 */
static int _compositor_internal__polygon(const char* text, float* points, int cap, float w, float h, float scale)
{
    char token[64];
    const char* p = text;
    int count = 0;
    int axis = 0;
    int length = 0;
    float value = 0.0f;

    token[0] = 0;
    while (*p != 0 && *p != ')' && count < cap)
    {
        while (*p == ' ' || *p == ',' || *p == '\t')
        {
            p++;
        }
        length = 0;
        while (*p != 0 && *p != ' ' && *p != ',' && *p != ')' && length < (int)sizeof(token) - 1)
        {
            token[length++] = *p++;
        }
        token[length] = 0;
        if (length == 0)
        {
            break;
        }
        value = (float)stdlib__atof(token);
        if (token[length - 1] == '%')
        {
            value = value / 100.0f * ((axis == 0) ? w : h);
        }
        else
        {
            value *= scale;
        }
        points[count * 2 + axis] = value;
        if (axis == 1)
        {
            count++;
        }
        axis = 1 - axis;
    }
    return count;
}

/**
 * @brief the coverage of a pixel by a polygon: four samples inside the pixel, even-odd fill
 * @param const float* points -> x, y pairs
 * @param int count -> point count
 * @param float x -> pixel centre x (box relative)
 * @param float y -> pixel centre y
 * @return float -> 0 to 1
 */
static float _compositor_internal__polygon_coverage(const float* points, int count, float x, float y)
{
    static const float offsets[4][2] = {{-0.25f, -0.25f}, {0.25f, -0.25f}, {-0.25f, 0.25f}, {0.25f, 0.25f}};
    float sx = 0.0f;
    float sy = 0.0f;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    int inside = 0;
    int hits = 0;
    int s = 0;
    int i = 0;
    int j = 0;

    for (s = 0; s < 4; s++)
    {
        sx = x + offsets[s][0];
        sy = y + offsets[s][1];
        inside = 0;
        for (i = 0, j = count - 1; i < count; j = i, i++)
        {
            x0 = points[j * 2];
            y0 = points[j * 2 + 1];
            x1 = points[i * 2];
            y1 = points[i * 2 + 1];
            if ((y1 > sy) != (y0 > sy) && sx < x1 + (sy - y1) * (x0 - x1) / (y0 - y1))
            {
                inside = !inside;
            }
        }
        hits += inside;
    }
    return (float)hits * 0.25f;
}

static void _compositor_internal__effects(gui_node_t* node, _compositor_internal__surface_t* surface, float scale)
{
    _compositor_internal__layer_t masks[COMPOSITOR_LAYER_CAP];
    char value[1024];
    char composite[64];
    char clip[512];
    char radius[256];
    char horizontal[128];
    char vertical[128];
    const char* slash = NULL_POINTER;
    float rx[4];
    float ry[4];
    float insets[4];
    float polygon[64];
    int polygon_count = 0;
    float factor = 1.0f;
    float coverage = 1.0f;
    float alpha = 0.0f;
    float px = 0.0f;
    float py = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float distance = 0.0f;
    int pass = 0;
    int count = 0;
    int x = 0;
    int y = 0;
    int i = 0;
    int c = 0;
    int corner = 0;
    int length = 0;
    int64 index = 0;

    stdlib__null_memory(masks, sizeof(masks));
    stdlib__null_memory(value, sizeof(value));
    stdlib__null_memory(composite, sizeof(composite));
    stdlib__null_memory(clip, sizeof(clip));
    stdlib__null_memory(radius, sizeof(radius));
    stdlib__null_memory(horizontal, sizeof(horizontal));
    stdlib__null_memory(vertical, sizeof(vertical));
    stdlib__null_memory(rx, sizeof(rx));
    stdlib__null_memory(ry, sizeof(ry));
    stdlib__null_memory(insets, sizeof(insets));
    stdlib__null_memory(polygon, sizeof(polygon));
    _compositor_internal__text_effects(node, surface, scale);
    motion__value_id(node, MOTION_PROP__FILTER, value, sizeof(value));
    count = _compositor_internal__layers(node, MOTION_PROP__MASK_IMAGE, masks, COMPOSITOR_LAYER_CAP, scale);
    if (count == 0)
    {
        count = _compositor_internal__layers(node, MOTION_PROP__MASK, masks, COMPOSITOR_LAYER_CAP, scale);
    }
    motion__value_id(node, MOTION_PROP__MASK_COMPOSITE, composite, sizeof(composite));
    motion__value_id(node, MOTION_PROP__CLIP_PATH, clip, sizeof(clip));
    motion__value_id(node, MOTION_PROP__BORDER_RADIUS, radius, sizeof(radius));
    if (stdlib__strncmp(clip, "inset(", 6) == 0)
    {
        _compositor_internal__four(clip + 6, insets, node->bounds.h, scale);
    }
    else if (stdlib__strncmp(clip, "polygon(", 8) == 0)
    {
        polygon_count = _compositor_internal__polygon(clip + 8, polygon, 32, node->bounds.w, node->bounds.h, scale);
    }
    if (radius[0] != 0)
    {
        slash = stdlib__strchr(radius, '/');
        if (slash != NULL_POINTER)
        {
            length = (int)(slash - radius);
            if (length >= (int)sizeof(horizontal))
            {
                length = sizeof(horizontal) - 1;
            }
            stdlib__memcpy(horizontal, radius, length);
            horizontal[length] = 0;
            stdlib__strcpy_safe(vertical, sizeof(vertical), slash + 1);
        }
        else
        {
            stdlib__strcpy_safe(horizontal, sizeof(horizontal), radius);
            stdlib__strcpy_safe(vertical, sizeof(vertical), radius);
        }
        _compositor_internal__four(horizontal, rx, node->bounds.w, scale);
        _compositor_internal__four(vertical, ry, node->bounds.h, scale);
        factor = stdlib__fminf(factor, node->bounds.w / stdlib__fmaxf(0.01f, rx[0] + rx[1]));
        factor = stdlib__fminf(factor, node->bounds.w / stdlib__fmaxf(0.01f, rx[2] + rx[3]));
        factor = stdlib__fminf(factor, node->bounds.h / stdlib__fmaxf(0.01f, ry[0] + ry[3]));
        factor = stdlib__fminf(factor, node->bounds.h / stdlib__fmaxf(0.01f, ry[1] + ry[2]));
        for (i = 0; i < 4; i++)
        {
            rx[i] *= factor;
            ry[i] *= factor;
        }
    }
    if (count == 0 && clip[0] == 0 && radius[0] == 0)
    {
        _compositor_internal__filter(surface, value, scale);
        return;
    }
    // Rounded geometry belongs to the source image. Blur that rounded image,
    // then apply masks and clip-path to the filtered result.
    for (pass = 0; pass < 2; pass++)
    {
        if (pass == 0 && radius[0] == 0)
        {
            continue;
        }
        if (pass == 1)
        {
            _compositor_internal__filter(surface, value, scale);
            if (count == 0 && clip[0] == 0)
            {
                break;
            }
        }
        for (y = 0; y < surface->h; y++)
        {
            for (x = 0; x < surface->w; x++)
            {
                index = ((int64)y * surface->w + x) * 4;
                if (surface->pixels[index + 3] == 0)
                {
                    continue;
                }
                px = surface->x + x + 0.5f - node->bounds.x;
                py = surface->y + y + 0.5f - node->bounds.y;
                coverage = 1.0f;
                if (pass == 1 && count > 0)
                {
                    coverage = _compositor_internal__layer_sample(&masks[count - 1], px, py).a;
                    for (i = count - 2; i >= 0; i--)
                    {
                        alpha = _compositor_internal__layer_sample(&masks[i], px, py).a;
                        if (stdlib__strcmp(composite, "exclude") == 0)
                        {
                            coverage = alpha * (1.0f - coverage) + coverage * (1.0f - alpha);
                        }
                        else if (stdlib__strcmp(composite, "intersect") == 0)
                        {
                            coverage *= alpha;
                        }
                        else if (stdlib__strcmp(composite, "subtract") == 0)
                        {
                            coverage = alpha * (1.0f - coverage);
                        }
                        else
                        {
                            coverage = alpha + coverage * (1.0f - alpha);
                        }
                    }
                }
                if (pass == 1 && stdlib__strncmp(clip, "inset(", 6) == 0 && (px < insets[3] || py < insets[0] || px >= node->bounds.w - insets[1] || py >= node->bounds.h - insets[2]))
                {
                    coverage = 0.0f;
                }
                if (pass == 1 && polygon_count >= 3)
                {
                    coverage *= _compositor_internal__polygon_coverage(polygon, polygon_count, px, py);
                }
                if (pass == 0 && radius[0] != 0)
                {
                    if (px < 0.0f || py < 0.0f || px > node->bounds.w || py > node->bounds.h)
                    {
                        coverage = 0.0f;
                    }
                    else
                    {
                        corner = (py < node->bounds.h * 0.5f) ? ((px < node->bounds.w * 0.5f) ? 0 : 1) : ((px < node->bounds.w * 0.5f) ? 3 : 2);
                        cx = (corner == 0 || corner == 3) ? px : node->bounds.w - px;
                        cy = (corner == 0 || corner == 1) ? py : node->bounds.h - py;
                        if (rx[corner] > 0.0f && ry[corner] > 0.0f && cx < rx[corner] && cy < ry[corner])
                        {
                            cx = (cx - rx[corner]) / rx[corner];
                            cy = (cy - ry[corner]) / ry[corner];
                            distance = (stdlib__sqrtf(cx * cx + cy * cy) - 1.0f) * stdlib__fminf(rx[corner], ry[corner]);
                            coverage *= _compositor_internal__clamp(0.5f - distance);
                        }
                    }
                }
                for (c = 0; c < 4; c++)
                {
                    surface->pixels[index + c] = (ubyte)(surface->pixels[index + c] * coverage + 0.5f);
                }
            }
        }
    }
}
