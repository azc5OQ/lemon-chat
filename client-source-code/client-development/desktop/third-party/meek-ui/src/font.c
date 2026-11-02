// ============================================================================
// font.c - TTF loading + rasterization + atlas + text drawing.
// ============================================================================
// PIPELINE:
// 1. register a family (TTF bytes stored, not yet rasterized).
// 2. on first request for (family, size): use stb_truetype's packer
// API to rasterize codepoints 32..255 into a 1024x1024 R8 atlas
// bitmap with 3x horizontal oversampling.
// 3. call renderer__create_atlas_r8 to upload the bitmap and get
// a backend-specific texture handle.
// 4. cache the whole rasterization under its (family, size) key.
// 5. each glyph's uv rect + advance + bearing stored in a per-font
// stbtt_packedchar array; looked up by codepoint at draw time.
// font__draw walks a utf-8 string, asks stb_truetype for each glyph's
// quad coordinates, and submits one textured quad per glyph via
// renderer__submit_text_glyph. the font's atlas texture is bound
// once before the glyph batch via renderer__set_text_atlas.
// IMGUI-LEVEL QUALITY:
// - 3x horizontal oversampling (stbtt_PackSetOversampling(3, 1)).
// - stbtt_PackFontRange handles the packer + rasterizer in one call
// (uses stb_rect_pack internally).
// - atlas sampled with LINEAR filtering in the shader (set up by
// each renderer backend).
// - advances are stored as floats; pen positions stay fractional,
// rounded only at final pixel write. this preserves subpixel
// positioning across a line.
// SIZE SEMANTICS:
// - size_px is the em size, as CSS font-size: the font's unitsPerEm
// map to size_px pixels (stbtt_ScaleForMappingEmToPixels). ascent,
// descent and line gap are rounded to whole pixels individually, so
// font__line_height is the normal line height a browser computes for
// the same font and size (Roboto at 100 px: 105 + 27 + 0 = 132).
// Earlier versions scaled the font so that ascent + descent spanned
// size_px, which drew Roboto about 24 % smaller than a browser draws
// the same font-size.

// readlink is a POSIX interface hidden by glibc in strict C11 mode unless
// requested before the first system header (used by executable-relative fonts).
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Win32 for directory enumeration (FindFirstFileW / FindNextFileW) --
// only used by the auto-scan helper, which is itself guarded by _WIN32.
// on Android the host app registers TTF bytes explicitly via
// font__register_from_memory (e.g. pulled from APK assets), so no
// directory walk is needed on that platform.
#if defined(_WIN32)
#include <windows.h>
#else
// POSIX scan (Linux DRM/X11, macOS) reads the fonts directory via
// opendir / readdir. Android skips this path entirely -- its fonts
// ship inside the APK and platform_android.c walks them through
// AAssetManager_openDir instead.
#if !defined(__ANDROID__)
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#endif
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "types.h"
#include "gui.h"
#include "fs.h"
#include "renderer.h"
#include "font.h"
#include "motion.h"
#include "scene.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"

// stb_truetype requires us to #define the implementation in exactly
// one translation unit. this is that one. we also pull in its malloc
// hooks, asserts, and a few basic stdlib headers it wants.
#include "clib/stdlib.h"
#include "clib/memory_manager.h"

// stb_truetype reaches for the C runtime through these hooks unless they
// are already defined: its heap goes to the memory manager (and so to the
// host's heap), and its maths to the project's own, so a freestanding
// build pulls in neither malloc nor libm through the font subsystem.
#define STBTT_malloc(size, user)  ((void)(user), GUI_MALLOC_T((size), MM_TYPE__FONT))
#define STBTT_free(ptr, user)     ((void)(user), GUI_FREE((ptr)))
#define STBTT_ifloor(x)           ((int)stdlib__floorf((float)(x)))
#define STBTT_iceil(x)            ((int)stdlib__ceilf((float)(x)))
#define STBTT_sqrt(x)             stdlib__sqrtf((float)(x))
#define STBTT_pow(x, y)           stdlib__powf((float)(x), (float)(y))
#define STBTT_fmod(x, y)          stdlib__fmodf((float)(x), (float)(y))
#define STBTT_cos(x)              stdlib__cosf((float)(x))
#define STBTT_acos(x)             stdlib__acosf((float)(x))
#define STBTT_fabs(x)             stdlib__fabsf((float)(x))
#define STBTT_strlen(x)           ((int)stdlib__strlen((x)))
#define STBTT_memcpy              stdlib__memcpy
#define STBTT_memset              stdlib__memset
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "clib/stdlib.h"

static gui_node_t* _font_internal__node = NULL_POINTER;
static float _font_internal__tracking = 0.0f;

/**
 * @brief scope letter spacing to the node currently being measured or drawn
 * @param gui_node_t* node -> text context, or NULL for unstyled host text
 * @return gui_node_t* -> previous context, to restore after recursive traversal
 */
gui_node_t* font__set_node(gui_node_t* node)
{
    gui_node_t* previous = _font_internal__node;
    gui_node_t* owner = node;
    char value[128];
    float em = node != NULL_POINTER ? node->resolved.font_size : 16.0f;

    value[0] = 0; // filled by motion__value_id (which terminates first) before it is read
    _font_internal__node = node;
    _font_internal__tracking = 0.0f;
    if (motion__active() == FALSE)
    {
        return previous;
    }
    while (owner != NULL_POINTER && motion__value_id(owner, MOTION_PROP__LETTER_SPACING, value, sizeof(value)) == FALSE)
    {
        owner = owner->parent;
    }
    if (owner != NULL_POINTER && stdlib__strcmp(value, "normal") != 0)
    {
        _font_internal__tracking = motion__number(value, 0.0f) * scene__scale();
        if (stdlib__strstr(value, "em") != NULL_POINTER)
        {
            _font_internal__tracking *= em;
        }
    }
    return previous;
}

// rasterized-range bounds. codepoints in the contiguous Latin-1
// range live in packed[0 .. NUM_CODEPOINTS). Beyond that, a small
// curated set of NON-contiguous "extras" (bullet, arrows, etc.) is
// packed into packed[NUM_CODEPOINTS .. NUM_CODEPOINTS+EXTRA_COUNT).
// Anything outside both sets is silently skipped by the text walkers.
#define _FONT_INTERNAL__FIRST_CODEPOINT 32
#define _FONT_INTERNAL__LAST_CODEPOINT  383
#define _FONT_INTERNAL__NUM_CODEPOINTS  (_FONT_INTERNAL__LAST_CODEPOINT - _FONT_INTERNAL__FIRST_CODEPOINT + 1)

// Non-contiguous "extras" packed alongside the Latin-1 range. Each
// entry is a real Unicode codepoint (not a byte index). The slots
// in packed[] for these live at indices NUM_CODEPOINTS + i.
// Picked for things meek-ui actually surfaces: BULLET (•) for the
// page-dots indicator, MIDDLE DOT (·) as an alt, BLACK/WHITE CIRCLE
// for future indicator styles, basic ARROWS for nav-chevron-style
// labels (when we want a real arrow instead of "<" or ">"), CHECK
// MARK + CROSS for tick / dismiss affordances.
// Keep this list short -- each entry packs into the atlas like a
// regular glyph (3x horizontal oversampling + padding). Bloating
// the list pushes the atlas to a larger power-of-two side, costing
// VRAM per font instance.
#define _FONT_INTERNAL__EXTRA_COUNT 31
static const int _font_internal__extra_codepoints[_FONT_INTERNAL__EXTRA_COUNT] =
{
    0x00B7, // MIDDLE DOT          ·
    0x2022, // BULLET              •
    0x25CB, // WHITE CIRCLE        ○
    0x25CF, // BLACK CIRCLE        ●
    0x2190, // LEFTWARDS ARROW     ←
    0x2192, // RIGHTWARDS ARROW    →
    0x2713, // CHECK MARK          ✓
    0x2715, // MULTIPLICATION X    ✕
    // the stress page (tests/visual/scenes/24-ai-hard-render): arrows,
    // the infinity sign, typographic quote and dash, box drawing for
    // the monospace block (Cousine has them; a family without a glyph
    // packs an empty one)
    0x2191, // UPWARDS ARROW       ↑
    0x2193, // DOWNWARDS ARROW     ↓
    0x221E, // INFINITY            ∞
    0x201C, // LEFT DOUBLE QUOTE   “
    0x2014, // EM DASH             —
    0x2550, // BOX DOUBLE HORIZONTAL   ═
    0x2551, // BOX DOUBLE VERTICAL     ║
    0x2554, // BOX DOUBLE DOWN+RIGHT   ╔
    0x2557, // BOX DOUBLE DOWN+LEFT    ╗
    0x255A, // BOX DOUBLE UP+RIGHT     ╚
    0x255D, // BOX DOUBLE UP+LEFT      ╝
    0x2560, // BOX DOUBLE VERT+RIGHT   ╠
    0x2563, // BOX DOUBLE VERT+LEFT    ╣
    // Chemical formulas and other scientific labels need real subscripts.
    // Keep all ten digits so longer formulas do not silently lose numbers.
    0x2080, // SUBSCRIPT ZERO
    0x2081, // SUBSCRIPT ONE
    0x2082, // SUBSCRIPT TWO
    0x2083, // SUBSCRIPT THREE
    0x2084, // SUBSCRIPT FOUR
    0x2085, // SUBSCRIPT FIVE
    0x2086, // SUBSCRIPT SIX
    0x2087, // SUBSCRIPT SEVEN
    0x2088, // SUBSCRIPT EIGHT
    0x2089, // SUBSCRIPT NINE
};
#define _FONT_INTERNAL__TOTAL_PACKED (_FONT_INTERNAL__NUM_CODEPOINTS + _FONT_INTERNAL__EXTRA_COUNT)

// Decode the next UTF-8 codepoint starting at *p (advancing *p past
// the byte sequence). Returns 0 if we hit `end`; returns 0xFFFD
// (Unicode replacement character) for invalid sequences and skips
// past the lead byte. Caller passes (utf8, utf8 + len) and loops
// while *p < end.
// Why we need this: meek-ui used to walk text byte-by-byte and
// treat each byte as a codepoint, so a multi-byte glyph like
// "•" (UTF-8: E2 80 A2) rendered as three Latin-1 glyphs
// ("â € ¢"). Proper UTF-8 walking lets non-ASCII glyphs in the
// extras table actually resolve.
// This is the standard 2/3/4-byte decode without overlong-encoding
// rejection (we don't see hostile input here; bad bytes turn into
// 0xFFFD which then misses the glyph_index lookup and gets skipped
// silently -- exactly the legacy "unsupported byte" behaviour).
static uint _font_internal__utf8_next(const ubyte** p, const ubyte* end)
{
    if (*p >= end)
    {
        return 0;
    }
    ubyte b0 = **p;
    (*p)++;
    if (b0 < 0x80)
    {
        return b0;
    }
    int  trail;
    uint cp;
    if      ((b0 & 0xE0) == 0xC0)
    {
        trail = 1;
        cp = b0 & 0x1F;
    }
    else if ((b0 & 0xF0) == 0xE0)
    {
        trail = 2;
        cp = b0 & 0x0F;
    }
    else if ((b0 & 0xF8) == 0xF0)
    {
        trail = 3;
        cp = b0 & 0x07;
    }
    else
    {
        return 0xFFFD;
    }
    for (int i = 0; i < trail; i++)
    {
        if (*p >= end || ((**p) & 0xC0) != 0x80)
        {
            return 0xFFFD;
        }
        cp = (cp << 6) | (**p & 0x3F);
        (*p)++;
    }
    return cp;
}

// Map a real Unicode codepoint to the packed[] index. Returns -1
// when the codepoint isn't in the atlas (Latin-1 range OR the
// extras table). The linear scan over extras is fine -- at 8
// entries it's a handful of comparisons per non-Latin codepoint,
// and most text is pure Latin so the early-out at the top fires
// on every glyph.
static int _font_internal__glyph_index(uint cp)
{
    if (cp >= (uint)_FONT_INTERNAL__FIRST_CODEPOINT && cp <= (uint)_FONT_INTERNAL__LAST_CODEPOINT)
    {
        return (int)cp - _FONT_INTERNAL__FIRST_CODEPOINT;
    }
    for (int i = 0; i < _FONT_INTERNAL__EXTRA_COUNT; i++)
    {
        if ((uint)_font_internal__extra_codepoints[i] == cp)
        {
            return _FONT_INTERNAL__NUM_CODEPOINTS + i;
        }
    }
    return -1;
}

// default atlas size for small fonts. 1024x1024 R8 = 1 MiB. fits a
// full Latin-1 range comfortably up to ~40 px. larger sizes scale the
// atlas dynamically via _font_internal__pick_atlas_dim below; the
// literal _ATLAS_W / _ATLAS_H constants below are kept only for the
// very-initial default. actual dimensions live on each gui_font_t entry
// (f->atlas_w / f->atlas_h) since they differ per (family, size).
// we cap atlas size at 4096x4096 -- any bigger and we're risking GPU
// texture limits on mobile and a single rasterization costs 16 MiB of
// transient CPU bitmap + 16 MiB of permanent GPU storage. a host app
// that wants to render 200 px text needs to either raise this cap or
// switch to an MSDF pipeline (deferred; see STATUS.md's font notes).
#define _FONT_INTERNAL__ATLAS_MIN_DIM   1024
#define _FONT_INTERNAL__ATLAS_MAX_DIM   4096

// pick the smallest power-of-two atlas dimension that can safely hold
// the full codepoint range at `size_px`. heuristic: each glyph needs
// roughly (size_px * oversample_x + padding) by (size_px + padding)
// pixels; 224 glyphs at that area, multiplied by 1.5 for packing
// overhead, gives the required total area; take sqrt to get the square
// side, round up to next power of two.
static int _font_internal__pick_atlas_dim(float size_px);

// size-adaptive oversample factors, matching imgui 1.92's
// ImFontAtlasBuildGetOversampleFactors:
// - below 36 px: 2x horizontal (subpixel positioning helps)
// - 36 px and above: 1x horizontal (atlas samples already big
// enough that GPU LINEAR filtering does the job; the 2x pays
// for no visible benefit and just wastes atlas area)
// - vertical always 1x (unless a caller overrides in the future)
// - pixel_snap_h families force 1x regardless of size so glyphs
// sit flush on integer columns, matching what bitmap fonts need
// previously we used a fixed (3, 1) factor regardless of size, which
// made large-atlas rasterizations of big display text ~50 % larger
// than necessary without any measurable sharpness benefit past
// roughly 20 px. the new picker mirrors imgui's approach.
static void _font_internal__pick_oversample(float size_px, boole pixel_snap_h, int* out_h, int* out_v)
{
    if (pixel_snap_h || size_px > 36.0f)
    {
        *out_h = 1;
    }
    else
    {
        *out_h = 2;
    }
    *out_v = 1;
}

// caps for the in-process tables. each registered family takes one
// slot; each (family, size) request takes one slot in the per-font
// cache.
// Bumped from 16 to 64. Projects shipping a reasonable font
// library (Roboto family + Karla + Droid + a few decoratives)
// easily hit 16, and because the auto-scan in platform init adds
// in alphabetical order, families late in the alphabet (Roboto,
// TimesRoman, Verdana) would be SILENTLY dropped. Lookups for
// those family names then fell back to whatever was registered
// first, giving "my text renders in the wrong font" surprises.
// The visual-test harness caught one of these during a popover
// scene showing a script font instead of Roboto. Atlas memory
// per family is tiny (just the tables); 64 is safe.
#define _FONT_INTERNAL__MAX_FAMILIES 64
// cache size. each (family, integer-pixel-size) pair rasterizes into
// one entry. the scale slider drags font sizes through every integer
// from ~min_size*0.5 to ~max_size*2.5, so a typical UI with 3 style
// rules (label, body, heading) and 2 fonts can land on:
// 3 style_sizes * 2 families * ~30 scale-driven sizes = 180 entries.
// 256 bounds the number of resident atlases and per-font glyph tables.
#define _FONT_INTERNAL__MAX_FONTS    256
#define _FONT_INTERNAL__MEASURE_CACHE_CAP 512
#define _FONT_INTERNAL__MEASURE_CACHE_TEXT 256
#define _FONT_INTERNAL__KERN_CACHE_CAP 4096

typedef struct _font_internal__measure_entry_t
{
    uint64 generation;
    uint hash;
    int length;
    float tracking;
    float width;
    char text[_FONT_INTERNAL__MEASURE_CACHE_TEXT];
} _font_internal__measure_entry_t;

typedef struct _font_internal__kern_entry_t
{
    uint64 generation;
    uint previous, current;
    float adjustment;
} _font_internal__kern_entry_t;

// family: a registered TTF file (or TTF bytes). rasterizations at
// various sizes hang off it lazily.
// the TTF bytes for a family can come from three sources:
// FROM_MAPPED_FILE (default path for font__register_from_file):
// the bytes are backed by fs__map_file -- a read-only memory-mapped
// view of the .ttf on disk. any other process that maps the same
// file shares the same physical pages via the kernel page cache,
// so you pay for each font exactly once across the whole device.
// this is the path that makes multi-process font sharing work on
// mobile (Android + iOS) for free. `mapping` holds the handles to
// unmap on shutdown.
// FROM_HEAP (fallback when fs__map_file fails):
// register_from_file malloc+ReadFile-copies the bytes. kept as a
// fallback if the filesystem doesn't support mapping. source ==
// _FONT_BYTES_HEAP => we free() the buffer on shutdown.
// FROM_EXTERNAL_MEMORY (font__register_from_memory):
// the caller hands us a pointer + size and guarantees the bytes
// live longer than the font subsystem. we don't free, unmap, or
// touch the backing storage. this is the embed-in-DLL mode.
typedef enum
{
    _FONT_BYTES_EXTERNAL = 0, // caller owns; do nothing on shutdown.
    _FONT_BYTES_HEAP     = 1, // we malloc'd; free on shutdown.
    _FONT_BYTES_MAPPED   = 2, // we mmap'd via fs__map_file; unmap on shutdown.
} _font_bytes_source_e;

typedef struct _font_internal__family_t
{
    char               name[64];
    const ubyte*       ttf_bytes;
    int64              ttf_size;
    _font_bytes_source_e source; // drives cleanup behavior.
    fs_mapped_file_t     mapping; // populated only when source == _FONT_BYTES_MAPPED.
    boole              active;
    // pixel_snap_h: force integer x-advance and 1x horizontal
    // oversampling for this family. auto-set for bitmap-designed
    // fonts (ProggyClean / ProggyTiny / kenvector_future) whose
    // glyph outlines land on an integer pixel grid at their design
    // size; pushing them through a subpixel positioning pipeline
    // blurs the very-deliberate pixel alignment. matches imgui's
    // ImFontConfig::PixelSnapH field + auto-assignment to bitmap
    // defaults.
    boole              pixel_snap_h;
} _font_internal__family_t;

// gui_font_t: a rasterization of one family at one size. holds the
// atlas pixels (freed after upload), the renderer texture handle,
// the packer output (one stbtt_packedchar per codepoint), and cached
// metrics.
struct gui_font_t
{
    int64  family_index; // index into g_families. -1 = unused slot.
    float  size_px;

    void*  atlas_tex; // renderer-specific handle (cast from GLuint / ID3D11SRV*/ IDirect3DTexture9*).
    ubyte* atlas_pixels; // kept null after upload -- renderer owns GPU copy.

    // per-entry atlas dimensions. picked at rasterize time by
    // _font_internal__pick_atlas_dim based on size_px, so a small
    // font still uses a 1024^2 sheet while a 73 px one gets 4096^2.
    // stbtt_GetPackedQuad needs these to compute correct UVs, so
    // every call site that used to pass _FONT_INTERNAL__ATLAS_W/H
    // now passes f->atlas_w / f->atlas_h.
    int    atlas_w;
    int    atlas_h;

    // cache sentinel: TRUE when rasterization failed permanently for
    // this (family, size). font__at returns NULL for cached entries
    // in this state so callers fall back to their "no font" path
    // without us re-attempting a ~MiB allocation and a full pack run
    // every frame. without it, a scene that asks for a too-big font
    // size each frame rasterizes (and fails) once per text widget per
    // frame -- very visibly laggy once a few widgets are in play.
    boole  rasterize_failed;

    // packed[] holds uv + metrics per glyph slot. The first
    // NUM_CODEPOINTS entries are the contiguous Latin-1 range
    // (slots 0..223 == codepoints 32..255); the trailing
    // EXTRA_COUNT entries hold the curated non-Latin extras
    // (slots 224..231 == _extra_codepoints[0..7]). _glyph_index()
    // returns the right slot for any supported codepoint.
    stbtt_packedchar packed[_FONT_INTERNAL__TOTAL_PACKED];

    // kerning: the font's own info for stbtt_GetCodepointKernAdvance
    // (kern or GPOS pair adjustments, which browsers apply by default),
    // the em scale that turns its font units into pixels, and whether
    // there is anything to look up. Bitmap families (pixel_snap_h)
    // keep their integer advances and never kern.
    stbtt_fontinfo info;
    float          em_scale;
    boole          has_kern;

    // vertical metrics, scaled to size_px. populated at pack time.
    float ascent;
    float descent;
    float line_gap;
    float line_height; // ascent + descent + line_gap (typical line advance).

    // LRU eviction tick. Bumped on every cache hit + at rasterize
    // time. When the cache hits MAX_FONTS, the entry with the
    // smallest tick gets its atlas dropped + the slot reclaimed
    // for the new (family, size). Prevents unbounded GPU memory
    // growth from prolonged scale-slider scrubbing without
    // bouncing back through font__shutdown.
    uint64 lru_tick;

    // New on every rasterization, including LRU slot reuse. Global metric
    // caches never mistake a recycled font address for the old family/size.
    uint64 metrics_generation;
};

// file-local state.
static _font_internal__family_t _font_internal__families[_FONT_INTERNAL__MAX_FAMILIES];
static int64                  _font_internal__family_count = 0;

static gui_font_t               _font_internal__fonts[_FONT_INTERNAL__MAX_FONTS];
static int64                  _font_internal__font_count = 0;

static boole                  _font_internal__initialized = FALSE;

// Monotonic counter feeding gui_font_t::lru_tick. Incremented on
// every cache hit and on rasterize, so older entries have lower
// values and are first to be evicted when the cache is full.
static uint64 _font_internal__lru_clock = 0;
static uint64 _font_internal__metrics_clock = 0;
// Fixed global budgets, independent of the number of font rasterizations.
// Measurement entries own their exact text bytes; hashes only select a slot.
static _font_internal__measure_entry_t _font_internal__measure_cache[_FONT_INTERNAL__MEASURE_CACHE_CAP];
static _font_internal__kern_entry_t _font_internal__kern_cache[_FONT_INTERNAL__KERN_CACHE_CAP];

// ============================================================================
// forward declarations
// ============================================================================

static _font_internal__family_t* _font_internal__find_family(char* name);
static int                     _font_internal__vmetrics(const stbtt_fontinfo* info, int* out_ascent, int* out_descent, int* out_line_gap);
static gui_font_t*               _font_internal__find_cached(int64 family_index, float size_px);
static gui_font_t*               _font_internal__rasterize(int64 family_index, float size_px);
static void                    _font_internal__scan_and_register_dir(char* dir_path);
static boole                   _font_internal__executable_dir(char* out, int cap);
static float                   _font_internal__kern(gui_font_t* f, uint prev_cp, uint cp);

// ============================================================================
// lifecycle
// ============================================================================

/**
 * @brief initialize the font subsystem. Must be called once after the renderer is up (font atlases become GPU textures, so the renderer must exist).
 *
 * @return boole -> TRUE on success; FALSE if something went wrong.
 */
boole font__init(void)
{
    if (_font_internal__initialized)
    {
        return TRUE;
    }
    stdlib__memset(_font_internal__families, 0, sizeof(_font_internal__families));
    stdlib__memset(_font_internal__fonts,    0, sizeof(_font_internal__fonts));
    stdlib__memset(_font_internal__measure_cache, 0, sizeof(_font_internal__measure_cache));
    stdlib__memset(_font_internal__kern_cache, 0, sizeof(_font_internal__kern_cache));
    _font_internal__metrics_clock = 0;
    _font_internal__family_count = 0;
    _font_internal__font_count   = 0;
    _font_internal__initialized  = TRUE;

    // auto-discover and register every .ttf in the library's shipped
    // fonts directory. GUI_FONTS_SOURCE_DIR is a compile-time define
    // (absolute path, baked by gui/CMakeLists.txt) so host apps don't
    // have to know any paths -- they just reference families by name
    // in their .style files (e.g. "Roboto-Regular", "Cousine-Regular").
    // the on-demand switch the user asked for: in the embedded-in-DLL
    // build mode (deferred), this call is replaced by iterating a
    // compiled-in byte-array table and calling font__register_from_memory
    // for each entry. the public API is identical either way.
#ifdef GUI_FONTS_SOURCE_DIR
    _font_internal__scan_and_register_dir(GUI_FONTS_SOURCE_DIR);
#endif

    return TRUE;
}

/**
 * @brief release every registered family + cached rasterization + renderer atlas texture. Safe to call if font__init wasn't successful.
 *
 * @return void
 */
void font__shutdown(void)
{
    // release each rasterized font's GPU atlas and (if still present)
    // its cpu bitmap.
    for (int64 i = 0; i < _font_internal__font_count; i++)
    {
        gui_font_t* f = &_font_internal__fonts[i];
        if (f->atlas_tex != NULL_POINTER)
        {
            renderer__destroy_atlas(f->atlas_tex);
            f->atlas_tex = NULL_POINTER;
        }
        if (f->atlas_pixels != NULL_POINTER)
        {
            GUI_FREE(f->atlas_pixels);
            f->atlas_pixels = NULL_POINTER;
        }
    }

    // release family TTF buffers we own, based on where the bytes came
    // from. mapped files go back through fs__unmap_file so the kernel
    // can drop the shared-page refcount; heap-owned bytes get free()'d;
    // external bytes (embed-in-DLL) we leave alone.
    for (int64 i = 0; i < _font_internal__family_count; i++)
    {
        _font_internal__family_t* fam = &_font_internal__families[i];
        switch (fam->source)
        {
            case _FONT_BYTES_MAPPED:
            {
                fs__unmap_file(&fam->mapping);
                break;
            }
            case _FONT_BYTES_HEAP:
            {
                if (fam->ttf_bytes != NULL_POINTER)
                {
                    GUI_FREE((void*)fam->ttf_bytes);
                }
                break;
            }
            case _FONT_BYTES_EXTERNAL:
            default:
            {
                // caller-owned; do nothing.
                break;
            }
        }
    }

    stdlib__memset(_font_internal__families, 0, sizeof(_font_internal__families));
    stdlib__memset(_font_internal__fonts,    0, sizeof(_font_internal__fonts));
    _font_internal__family_count = 0;
    _font_internal__font_count   = 0;
    _font_internal__initialized  = FALSE;
    stdlib__memset(_font_internal__measure_cache, 0, sizeof(_font_internal__measure_cache));
    stdlib__memset(_font_internal__kern_cache, 0, sizeof(_font_internal__kern_cache));
    _font_internal__metrics_clock = 0;
}

/**
 * @brief drop every (family, size) rasterization from the cache, including its GPU atlas texture. Registered TTF families are untouched, so the next font__at call re-rasterizes + re-uploads against whichever renderer context is current. Used on Android to recover from EGL-context loss across APP_CMD_TERM_WINDOW / APP_CMD_INIT_WINDOW; must be called while the OLD context is still current (so renderer__destroy_atlas can safely release the GPU handle).
 *
 * @return void
 */
void font__drop_gpu_cache(void)
{
    // GL-context-loss recovery. On Android, APP_CMD_TERM_WINDOW
    // destroys the EGL context and every GPU handle with it. The
    // registered font FAMILIES (TTF bytes) are pure CPU state and
    // survive -- but every cached (family, size) rasterization holds
    // an atlas_tex GPU handle that's about to become garbage. Drop
    // them here so the next font__at call after INIT_WINDOW builds
    // a fresh entry against the new context. Families are untouched.
    // Must be called while the OLD context is still current so
    // renderer__destroy_atlas's glDeleteTextures (or D3D Release) is
    // valid. platform_android__on_app_cmd orders it before
    // renderer__shutdown for that reason.
    for (int64 i = 0; i < _font_internal__font_count; i++)
    {
        gui_font_t* f = &_font_internal__fonts[i];
        if (f->atlas_tex != NULL_POINTER)
        {
            renderer__destroy_atlas(f->atlas_tex);
            f->atlas_tex = NULL_POINTER;
        }
        if (f->atlas_pixels != NULL_POINTER)
        {
            // Shouldn't happen in steady state (atlas_pixels is freed
            // right after upload) but covers the in-flight rasterize
            // case where the context died mid-pack.
            GUI_FREE(f->atlas_pixels);
            f->atlas_pixels = NULL_POINTER;
        }
    }
    stdlib__memset(_font_internal__fonts, 0, sizeof(_font_internal__fonts));
    _font_internal__font_count = 0;
    stdlib__memset(_font_internal__measure_cache, 0, sizeof(_font_internal__measure_cache));
    stdlib__memset(_font_internal__kern_cache, 0, sizeof(_font_internal__kern_cache));
    _font_internal__metrics_clock = 0;
}

// ============================================================================
// family registration
// ============================================================================

// Release rasterizations before replacing their backing TTF bytes. Otherwise
// cached glyphs describe the old font and stb kerning info can point at an
// unmapped file. Leave other families and their atlases resident.
static void _font_internal__invalidate_family(int64 family_index)
{
    int64 i;
    for (i = 0; i < _font_internal__font_count; i++)
    {
        gui_font_t* font = &_font_internal__fonts[i];
        if (font->family_index != family_index) continue;
        if (font->atlas_tex != NULL_POINTER) renderer__destroy_atlas(font->atlas_tex);
        if (font->atlas_pixels != NULL_POINTER) GUI_FREE(font->atlas_pixels);
        stdlib__memset(font, 0, sizeof(*font));
        font->family_index = -1;
    }
}

/**
 * @brief register a font family from TTF bytes already in memory. The bytes are NOT copied -- caller guarantees they remain valid for the life of the font subsystem.
 *
 * @param char* family -> The name to refer to this font by (e.g. "Inter").
 * @param const ubyte* ttf_bytes -> Pointer to the TTF file contents.
 * @param int64 ttf_size -> Size of the TTF file in bytes.
 *
 * @return boole -> TRUE on success.
 */
boole font__register_from_memory(char* family, const ubyte* ttf_bytes, int64 ttf_size)
{
    if (_font_internal__initialized == FALSE || family == NULL_POINTER || ttf_bytes == NULL_POINTER || ttf_size < 12)
    {
        return FALSE;
    }

    // sanity-check the TTF header via stb_truetype. fails early on
    // bogus bytes before they can cause weirdness later.
    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex(ttf_bytes, 0);
    if (offset < 0 || stbtt_InitFont(&info, ttf_bytes, offset) == 0)
    {
        log_error("%s %s %s", "stbtt_InitFont failed for family '", family, "'");
        return FALSE;
    }

    // already registered? release the old backing storage first (mapped
    // or heap; external bytes are caller-owned), then reuse the slot.
    _font_internal__family_t* fam = _font_internal__find_family(family);
    if (fam == NULL_POINTER)
    {
        if (_font_internal__family_count >= _FONT_INTERNAL__MAX_FAMILIES)
        {
            log_error("%s %d %s", "family table full (max", _FONT_INTERNAL__MAX_FAMILIES, ")");
            return FALSE;
        }
        fam = &_font_internal__families[_font_internal__family_count++];
    }
    else
    {
        _font_internal__invalidate_family((int64)(fam - _font_internal__families));
        if (fam->source == _FONT_BYTES_MAPPED)
        {
            fs__unmap_file(&fam->mapping);
        }
        else if (fam->source == _FONT_BYTES_HEAP && fam->ttf_bytes != NULL_POINTER)
        {
            GUI_FREE((void*)fam->ttf_bytes);
        }
    }

    size_t n = stdlib__strlen(family);
    if (n >= sizeof(fam->name))
    {
        n = sizeof(fam->name) - 1;
    }
    stdlib__memcpy(fam->name, family, n);
    fam->name[n]   = 0;
    fam->ttf_bytes = ttf_bytes;
    fam->ttf_size  = ttf_size;
    fam->source    = _FONT_BYTES_EXTERNAL;
    stdlib__memset(&fam->mapping, 0, sizeof(fam->mapping));
    fam->active    = TRUE;

    // auto-detect bitmap-designed fonts and turn on pixel_snap_h.
    // ProggyClean / ProggyTiny / kenvector_future / kenvector_future_thin
    // are the bitmap-grid faces bundled with the toolkit; at their
    // native sizes every stem lands on an integer pixel column, so
    // we want no subpixel positioning (oversample=1) and we want each
    // glyph's advance rounded to integer so consecutive chars stay
    // column-aligned. heuristic match beats plumbing a flag through
    // every registration call site; host apps with their own bitmap
    // faces can extend the check here.
    fam->pixel_snap_h = FALSE;
    {
        const char* nm = fam->name;
        if (stdlib__strncmp(nm, "Proggy", 6) == 0 || stdlib__strstr(nm, "kenvector") != NULL_POINTER)
        {
            fam->pixel_snap_h = TRUE;
        }
    }

    return TRUE;
}

/**
 * @brief register a font family by reading a TTF file from disk. The bytes are owned by the font subsystem and freed at shutdown.
 *
 * @param char* family -> The name to refer to this font by.
 * @param char* path -> UTF-8 path to the .ttf file.
 *
 * @return boole -> TRUE if the file was read and the TTF validated.
 */
boole font__register_from_file(char* family, char* path)
{
    if (_font_internal__initialized == FALSE || family == NULL_POINTER || path == NULL_POINTER)
    {
        return FALSE;
    }

    // try the memory-mapped path first. fs__map_file produces a
    // read-only view that every process mapping the same file shares
    // via the kernel page cache -- a phone with N app instances pays
    // for the fonts exactly once regardless of N. see fs.h for the
    // cross-platform story.
    // fall back to fs__read_entire_file (heap copy) only if mapping
    // fails, which on Windows typically means the file is on a weird
    // filesystem that doesn't support mappings.
    fs_mapped_file_t mapping;
    if (fs__map_file(path, &mapping))
    {
        // plug the mapped view into register_from_memory (it treats the
        // bytes as external). then upgrade the family record so we know
        // to unmap on shutdown instead of free().
        if (font__register_from_memory(family, mapping.data, mapping.size) == FALSE)
        {
            fs__unmap_file(&mapping);
            return FALSE;
        }
        _font_internal__family_t* fam = _font_internal__find_family(family);
        if (fam != NULL_POINTER)
        {
            fam->source  = _FONT_BYTES_MAPPED;
            fam->mapping = mapping;
        }
        return TRUE;
    }

    // mapping failed; try the heap-copy fallback. if that also fails,
    // we bail.
    int64 size = 0;
    char* buf  = fs__read_entire_file(path, &size);
    if (buf == NULL_POINTER)
    {
        log_error("%s %s %s", "failed to read '", path, "'");
        return FALSE;
    }

    if (font__register_from_memory(family, (const ubyte*)buf, size) == FALSE)
    {
        GUI_FREE(buf);
        return FALSE;
    }

    // mark the family as heap-owned so shutdown frees the buffer.
    _font_internal__family_t* fam = _font_internal__find_family(family);
    if (fam != NULL_POINTER)
    {
        fam->source = _FONT_BYTES_HEAP;
    }
    return TRUE;
}

/** @brief register temporary font bytes using a tracked copy owned until replacement or shutdown. */
boole font__register_from_memory_copy(char* family, const ubyte* ttf_bytes, int64 ttf_size)
{
    ubyte* copy;
    _font_internal__family_t* registered;
    if (!ttf_bytes || ttf_size < 12 || ttf_size > 32 * 1024 * 1024) { return FALSE; }
    copy = GUI_MALLOC_T((size_t)ttf_size, MM_TYPE__FONT);
    if (!copy) { return FALSE; }
    stdlib__memcpy(copy, ttf_bytes, (size_t)ttf_size);
    if (!font__register_from_memory(family, copy, ttf_size)) { GUI_FREE(copy); return FALSE; }
    registered = _font_internal__find_family(family);
    registered->source = _FONT_BYTES_HEAP;
    return TRUE;
}

/**
 * @brief registers every .ttf file found directly inside dir_path as a font family named after the file stem (Roboto-Regular.ttf becomes the family "Roboto-Regular"); this is the host-driven replacement for the compile-time GUI_FONTS_SOURCE_DIR scan
 *
 * @param char* dir_path -> directory to scan; not recursive
 *
 * @attention font__init must have run first (platform__init does that); on Android the scan is a no-op because APK assets are not on a POSIX filesystem, use font__register_from_memory there
 *
 * @return boole -> TRUE when the scan ran, FALSE when the font subsystem is not initialized or dir_path is NULL_POINTER
 */
/**
 * @brief the directory of the running executable, without a trailing separator
 * @param char* out -> receives the path
 * @param int cap -> capacity of out
 * @return boole -> FALSE when the platform gives no answer or the path does not fit
 */
static boole _font_internal__executable_dir(char* out, int cap)
{
    int64 n = 0;
#if defined(_WIN32)
    DWORD length = 0;

    length = GetModuleFileNameA(NULL_POINTER, out, (DWORD)cap);
    if (length == 0 || (int)length >= cap - 1)
    {
        return FALSE;
    }
    n = (int64)length;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;

    if (_NSGetExecutablePath(out, &size) != 0)
    {
        return FALSE;
    }
    n = (int64)stdlib__strlen(out);
#elif defined(__linux__) || defined(__ANDROID__)
    ssize_t length = 0;

    length = readlink("/proc/self/exe", out, (size_t)cap - 1);
    if (length <= 0)
    {
        return FALSE;
    }
    out[length] = 0;
    n = (int64)length;
#else
    (void)out;
    (void)cap;
    return FALSE;
#endif
    while (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\')
    {
        n--;
    }
    if (n <= 1)
    {
        return FALSE;
    }
    out[n - 1] = 0;
    return TRUE;
}

boole font__register_dir(char* dir_path)
{
    // an executable built on one machine runs on another (a different
    // drive letter, a copied folder): when the compile-time directory
    // registers nothing, MEEK_UI_FONTS, then a fonts folder next to the
    // executable or up to three levels above it (hosts live in build
    // folders of the repository) are tried
    static const char* relative[4] = {"fonts", "../fonts", "../../fonts", "../../../fonts"};
    char base[1024];
    char candidate[1024];
    const char* env = NULL_POINTER;
    int64 before = 0;
    int i = 0;

    base[0] = 0;
    candidate[0] = 0;
    if (_font_internal__initialized == FALSE || dir_path == NULL_POINTER)
    {
        return FALSE;
    }
    before = _font_internal__family_count;
    _font_internal__scan_and_register_dir(dir_path);
    if (_font_internal__family_count > before)
    {
        return TRUE;
    }
    env = stdlib__getenv("MEEK_UI_FONTS");
    if (env != NULL_POINTER && env[0] != 0)
    {
        _font_internal__scan_and_register_dir((char*)env);
        if (_font_internal__family_count > before)
        {
            log_info("%s %s %s %s", "fonts: nothing in '", dir_path, "', registered MEEK_UI_FONTS", env);
            return TRUE;
        }
    }
    if (_font_internal__executable_dir(base, (int)sizeof(base)) == FALSE)
    {
        return TRUE;
    }
    // a relative directory ("fonts") was just tried against the working
    // directory; it means "next to the executable" as well
    if (dir_path[0] != '/' && dir_path[0] != '\\' && !(dir_path[0] != 0 && dir_path[1] == ':'))
    {
        stdlib__strcpy_safe(candidate, sizeof(candidate), base);
        stdlib__strcat_safe(candidate, sizeof(candidate), "/");
        stdlib__strcat_safe(candidate, sizeof(candidate), dir_path);
        _font_internal__scan_and_register_dir(candidate);
        if (_font_internal__family_count > before)
        {
            log_info("%s %s %s %s", "fonts: nothing in '", dir_path, "', registered", candidate);
            return TRUE;
        }
    }
    for (i = 0; i < 4; i++)
    {
        stdlib__strcpy_safe(candidate, sizeof(candidate), base);
        stdlib__strcat_safe(candidate, sizeof(candidate), "/");
        stdlib__strcat_safe(candidate, sizeof(candidate), relative[i]);
        _font_internal__scan_and_register_dir(candidate);
        if (_font_internal__family_count > before)
        {
            log_info("%s %s %s %s", "fonts: nothing in '", dir_path, "', registered", candidate);
            return TRUE;
        }
    }
    log_warn("%s %s %s %s", "fonts: nothing in '", dir_path, "', MEEK_UI_FONTS or a fonts folder next to", base);
    return TRUE;
}

// ============================================================================
// query
// ============================================================================

/**
 * @brief get (or lazily build) a font at a specific pixel size. First call for a given (family, size) rasterizes the full ASCII+Latin-1 range into an atlas and uploads the texture; subsequent calls return the cache.
 *
 * @param char* family -> Registered family name. NULL or "" = default (first registered).
 * @param float size_px -> Requested size in pixels. <= 0 falls back to a 14 px default.
 *
 * @return gui_font_t* -> Cached font handle, or NULL if no families are registered.
 */
gui_font_t* font__at(char* family, float size_px)
{
    if (_font_internal__initialized == FALSE || _font_internal__family_count == 0)
    {
        return NULL_POINTER;
    }
    if (size_px <= 0.0f)
    {
        size_px = 14.0f;
    }

    // snap to a quarter pixel before caching.
    // why: callers routinely pass (style_size * scene_scale), which from
    // the demo's scale slider lands on endless fractional values (14.02,
    // 14.07, 14.13, ...). without snapping, every tiny slider wiggle
    // rasterizes a brand-new atlas and the cache fills in seconds.
    // a quarter pixel keeps the common DPI scales exact (14 px at 125 %
    // is 17.5 px, and a browser rasterizes exactly that) while bounding
    // cache growth to four entries per (family, integer size) pair.
    // clamp to [1, 256] as a safety rail -- stbtt's packer can't render
    // glyphs below ~6 px usefully, and anything over ~110 would overflow
    // the atlas. host apps that need extreme sizes can revisit.
    int quarter = (int)(size_px * 4.0f + 0.5f);
    if (quarter < 4)    quarter = 4;
    if (quarter > 1024) quarter = 1024;
    size_px = (float)quarter / 4.0f;

    int64 family_index = -1;
    if (family != NULL_POINTER && family[0] != 0)
    {
        for (int64 i = 0; i < _font_internal__family_count; i++)
        {
            if (stdlib__strcmp(_font_internal__families[i].name, family) == 0)
            {
                family_index = i;
                break;
            }
        }
    }
    if (family_index < 0)
    {
        // fallback to the first registered family. matches the "no font
        // family specified" or "requested family not found" cases.
        family_index = 0;
    }

    gui_font_t* cached = _font_internal__find_cached(family_index, size_px);
    if (cached != NULL_POINTER)
    {
        // cached failure -- return NULL so callers fall back to their
        // "no font" path instead of our rasterize attempting a huge
        // allocation + PackFontRange call every frame.
        if (cached->rasterize_failed)
        {
            return NULL_POINTER;
        }
        return cached;
    }
    return _font_internal__rasterize(family_index, size_px);
}

void font__prewarm_tree(gui_node_t* root, float scale)
{
    gui_node_t* node = root;
    if (root == NULL_POINTER || scale <= 0.0f) return;
    while (node != NULL_POINTER)
    {
        // Controls can draw values even when node->text is empty. Preparing
        // their inherited font too only repeats a cheap cache lookup.
        font__at(node->resolved.font_family, (node->resolved.font_size > 0.0f ? node->resolved.font_size : 14.0f) * scale);
        if (node->first_child != NULL_POINTER)
        {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next_sibling == NULL_POINTER) node = node->parent;
        if (node == root) break;
        node = node->next_sibling;
    }
}

/**
 * @brief get per-codepoint glyph info. Returns FALSE for codepoints outside the rasterized range (currently 32..255).
 *
 * @param gui_font_t* f -> Font handle from font__at.
 * @param uint codepoint -> Unicode codepoint.
 * @param gui_glyph_t* out -> Filled on success.
 *
 * @return boole -> TRUE if the glyph exists in the atlas.
 */
boole font__glyph(gui_font_t* f, uint codepoint, gui_glyph_t* out)
{
    if (f == NULL_POINTER || out == NULL_POINTER)
    {
        return FALSE;
    }
    int index = _font_internal__glyph_index(codepoint);
    if (index < 0)
    {
        return FALSE;
    }

    // stbtt_GetPackedQuad computes the quad (screen rect + uv rect)
    // given a pen position. we ask at (0, 0) and pick off the raw
    // metrics; caller positions with its own pen.
    stbtt_aligned_quad q;
    float x = 0.0f;
    float y = 0.0f;
    stbtt_GetPackedQuad(f->packed, f->atlas_w, f->atlas_h, index, &x, &y, &q, 1);

    out->advance  = f->packed[index].xadvance + _font_internal__tracking;
    out->offset_x = q.x0; // bearing left relative to the pen origin we passed (0,0).
    out->offset_y = q.y0; // bearing top (negative for most glyphs, going "up" from baseline).
    out->width    = q.x1 - q.x0;
    out->height   = q.y1 - q.y0;
    out->uv_x0    = q.s0;
    out->uv_y0    = q.t0;
    out->uv_x1    = q.s1;
    out->uv_y1    = q.t1;
    return TRUE;
}

// Share measurements between layout/draw and equal text at different addresses.
// Every hit compares all bytes, the exact font generation, length and tracking.
// Long strings still use the original UTF-8 walker and the kerning cache.
static float _font_internal__measure(gui_font_t* f, const char* utf8, int64 length)
{
    _font_internal__measure_entry_t* entry = NULL_POINTER;
    uint hash = 2166136261u;
    uint tracking_bits = 0;
    float x = 0.0f;
    uint previous = 0;
    const ubyte* p = (const ubyte*)utf8;
    const ubyte* end = p + length;
    int64 i;
    if (length <= _FONT_INTERNAL__MEASURE_CACHE_TEXT)
    {
        for (i = 0; i < length; i++) hash = (hash ^ (ubyte)utf8[i]) * 16777619u;
        stdlib__memcpy(&tracking_bits, &_font_internal__tracking, sizeof(tracking_bits));
        hash ^= (uint)f->metrics_generation * 2654435761u ^ (uint)(f->metrics_generation >> 32) ^ tracking_bits;
        hash ^= hash >> 16;
        entry = &_font_internal__measure_cache[hash & (_FONT_INTERNAL__MEASURE_CACHE_CAP - 1)];
        if (entry->generation == f->metrics_generation && entry->hash == hash && entry->length == length &&
            entry->tracking == _font_internal__tracking && stdlib__memcmp(entry->text, utf8, length) == 0)
        {
            return entry->width;
        }
    }
    while (p < end)
    {
        uint cp = _font_internal__utf8_next(&p, end);
        int index = _font_internal__glyph_index(cp);
        if (index < 0) continue;
        x += _font_internal__kern(f, previous, cp) + f->packed[index].xadvance + _font_internal__tracking;
        previous = cp;
    }
    if (entry != NULL_POINTER)
    {
        entry->generation = f->metrics_generation;
        entry->hash = hash;
        entry->length = (int)length;
        entry->tracking = _font_internal__tracking;
        entry->width = x;
        stdlib__memcpy(entry->text, utf8, length);
    }
    return x;
}

/**
 * @brief measure the pixel width of a utf-8 string at this font's size. Ignores codepoints outside the rasterized range.
 *
 * @param gui_font_t* f -> Font handle.
 * @param char* utf8 -> Null-terminated string.
 *
 * @return float -> Total advance width in pixels.
 */
float font__measure(gui_font_t* f, char* utf8)
{
    if (f == NULL_POINTER || utf8 == NULL_POINTER)
    {
        return 0.0f;
    }
    return _font_internal__measure(f, utf8, (int64)stdlib__strlen(utf8));
}

// fixed-length variant for word-wrap probing: measures utf8[0..n) in
// pixels without needing a trailing null byte. widget_text.c calls
// this with every candidate "word fits on this line?" test, which
// avoids mutating the source string just to terminate a substring.
/**
 * @brief measure the pixel width of the first `n` bytes of a utf-8 string. Used by word-wrap logic (widget_text) to probe candidate line breaks without having to null-terminate substrings. Ignores codepoints outside the rasterized range.
 *
 * @param gui_font_t* f -> Font handle.
 * @param char* utf8 -> String (not necessarily null-terminated).
 * @param int64 n -> Number of bytes to measure.
 *
 * @return float -> Total advance width of those n bytes in pixels.
 */
float font__measure_n(gui_font_t* f, char* utf8, int64 n)
{
    if (f == NULL_POINTER || utf8 == NULL_POINTER || n <= 0)
    {
        return 0.0f;
    }
    return _font_internal__measure(f, utf8, n);
}

/**
 * @brief line height in pixels (ascent + descent + line gap).
 *
 * @param gui_font_t* f -> Font handle.
 *
 * @return float -> Line height.
 */
float font__line_height(gui_font_t* f)
{
    return f != NULL_POINTER ? f->line_height : 0.0f;
}

/**
 * @brief ascent in pixels (baseline to top of cap height, roughly).
 *
 * @param gui_font_t* f -> Font handle.
 *
 * @return float -> Ascent.
 */
float font__ascent(gui_font_t* f)
{
    return f != NULL_POINTER ? f->ascent : 0.0f;
}

/**
 * @brief descent in pixels, negative: the distance from the baseline down to the bottom of the font's descenders, rounded like the ascent
 *
 * @param gui_font_t* f -> Font handle.
 *
 * @return float -> Descent, zero or negative.
 */
float font__descent(gui_font_t* f)
{
    return f != NULL_POINTER ? f->descent : 0.0f;
}

/**
 * @brief kerning adjustment in pixels between two consecutive codepoints, what font__measure adds before the second glyph's advance; a text walker that accumulates single-glyph advances adds this to land on the same width
 *
 * @param gui_font_t* f -> Font handle.
 * @param uint prev_cp -> the previous codepoint, 0 at the start of a run
 * @param uint cp -> the codepoint about to be placed
 *
 * @return float -> Adjustment, usually zero or negative.
 */
float font__kern(gui_font_t* f, uint prev_cp, uint cp)
{
    if (f == NULL_POINTER)
    {
        return 0.0f;
    }

    return _font_internal__kern(f, prev_cp, cp);
}

/**
 * @brief the line box height a style gives text drawn with this font: line-height as a multiplier of the font size, as absolute pixels, or the font's normal line height when the style sets neither
 *
 * @param gui_font_t* f -> Font handle.
 * @param const gui_style_t* s -> resolved style of the node being drawn
 * @param float scale -> global UI scale
 *
 * @return float -> Line height in pixels.
 */
float font__line_height_for(gui_font_t* f, const gui_style_t* s, float scale)
{
    float font_px = 0.0f;

    if (s != NULL_POINTER && s->line_height_px > 0.0f)
    {
        return s->line_height_px * scale;
    }

    if (s != NULL_POINTER && s->line_height > 0.0f)
    {
        font_px = (s->font_size > 0.0f ? s->font_size : 14.0f) * scale;
        return s->line_height * font_px;
    }

    return font__line_height(f);
}

/**
 * @brief distance from the top of a line box to the baseline: the CSS half-leading, (line height - ascent - descent) / 2, plus the ascent
 *
 * @param gui_font_t* f -> Font handle.
 * @param const gui_style_t* s -> resolved style of the node being drawn
 * @param float scale -> global UI scale
 *
 * @return float -> Baseline offset in pixels.
 */
float font__baseline_for(gui_font_t* f, const gui_style_t* s, float scale)
{
    float line_h = font__line_height_for(f, s, scale);
    float content = font__ascent(f) - font__descent(f);

    return (line_h - content) * 0.5f + font__ascent(f);
}

/**
 * @brief opaque atlas texture handle (void* cast from each backend's native texture type). The renderer uses this with renderer__set_text_atlas.
 *
 * @param gui_font_t* f -> Font handle.
 *
 * @return void* -> Renderer-specific texture handle (GLuint, ID3D11SRV*, IDirect3DTexture9*).
 */
void* font__atlas_tex(gui_font_t* f)
{
    return f != NULL_POINTER ? f->atlas_tex : NULL_POINTER;
}

// ============================================================================
// drawing
// ============================================================================

/**
 * @brief walk a utf-8 string and emit one textured quad per glyph via renderer__submit_text_glyph. Also binds the font's atlas texture via renderer__set_text_atlas before emitting.
 *
 * @param gui_font_t* f -> Font to draw with.
 * @param float x -> Pen x position (left edge of first glyph, in pixels).
 * @param float y -> Pen y position on the baseline.
 * @param gui_color_t color -> Text color (multiplied against the glyph's alpha).
 * @param char* utf8 -> Null-terminated string.
 *
 * @return void
 */
void font__draw(gui_font_t* f, float x, float y, gui_color_t color, char* utf8)
{
    if (utf8 == NULL_POINTER)
    {
        return;
    }
    // delegate to the length-bounded variant so the two functions share
    // one glyph-emit implementation. measuring strlen here costs a loop
    // we'd be doing implicitly in font__draw_n anyway.
    int64 n = 0;
    while (utf8[n] != 0)
    {
        n++;
    }
    font__draw_n(f, x, y, color, utf8, n);
}

/**
 * @brief length-bounded variant of font__draw. Draws utf8[0..n) instead of reading until a null terminator. Used by widget_text's word-wrap path to draw one line-slice at a time without having to build null-terminated substrings.
 *
 * @param gui_font_t* f -> Font to draw with.
 * @param float x -> Pen x position.
 * @param float y -> Pen y position (baseline).
 * @param gui_color_t color -> Text color.
 * @param char* utf8 -> String (not necessarily null-terminated).
 * @param int64 n -> Number of bytes to draw.
 *
 * @return void
 */
static void _font_internal__draw_n(gui_font_t* f, float x, float y, gui_color_t color, char* utf8, int64 n, const gui_rect_t* clip)
{
    if (f == NULL_POINTER || utf8 == NULL_POINTER || n <= 0)
    {
        return;
    }

    // bind the atlas so subsequent submit_text_glyph calls sample from
    // it. different fonts have different textures; the renderer's text
    // batch only handles one atlas at a time, so if the caller mixes
    // fonts within a frame they need to call font__draw per font run.
    renderer__set_text_atlas(f->atlas_tex);

    // pixel-truncate the text origin before emitting glyphs. callers
    // pass bounds.x + bearing + ascent and such, which are routinely
    // fractional once the scene's scale slider is at something other
    // than 1.0 -- and every glyph's stbtt_GetPackedQuad align_to_integer
    // step anchors relative to this origin, so a fractional pen_y
    // slides horizontal stems across two adjacent rows of pixels,
    // producing the classic "baseline drift softness" even though
    // each individual glyph is pixel-aligned. matches imgui's
    // ImFont::RenderText prologue:
    // // Align to be pixel perfect
    // float x = IM_TRUNC(pos.x);
    // float y = IM_TRUNC(pos.y);
    // truncating (floor toward zero) instead of rounding matches
    // imgui's IM_TRUNC so the behavior is the same.
    float pen_x = (float)(int)x;
    float pen_y = (float)(int)y;
    uint  prev_cp = 0;

    // UTF-8 walker: see font__measure for rationale. Codepoints
    // outside the atlas (neither Latin-1 nor in the extras list)
    // get silently skipped, same as the legacy byte-by-byte path
    // did for non-Latin bytes.
    const ubyte* p   = (const ubyte*)utf8;
    const ubyte* end = p + n;
    while (p < end)
    {
        uint cp = _font_internal__utf8_next(&p, end);
        int index = _font_internal__glyph_index(cp);
        if (index < 0)
        {
            continue;
        }

        // the pair adjustment moves the pen before the glyph is placed, the
        // same amount font__measure counted
        pen_x += _font_internal__kern(f, prev_cp, cp);
        prev_cp = cp;

        // stbtt_GetPackedQuad bakes the oversampling adjustment into the
        // returned quad and advances the pen position by xadvance. this
        // is the "right" way to consume PackedChars -- doing it manually
        // is error-prone (easy to be off by half a pixel).
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(f->packed, f->atlas_w, f->atlas_h, index, &pen_x, &pen_y, &q, 1);
        pen_x += _font_internal__tracking;

        gui_rect_t rect;
        rect.x = q.x0;
        rect.y = q.y0;
        rect.w = q.x1 - q.x0;
        rect.h = q.y1 - q.y0;

        gui_rect_t uv;
        uv.x = q.s0;
        uv.y = q.t0;
        uv.w = q.s1 - q.s0;
        uv.h = q.t1 - q.t0;

        if (clip == NULL_POINTER || (rect.x + rect.w > clip->x && rect.x < clip->x + clip->w &&
            rect.y + rect.h > clip->y && rect.y < clip->y + clip->h))
        {
            renderer__submit_text_glyph(rect, uv, color);
        }
    }
}

void font__draw_n(gui_font_t* f, float x, float y, gui_color_t color, char* utf8, int64 n)
{
    _font_internal__draw_n(f, x, y, color, utf8, n, NULL_POINTER);
}

void font__draw_n_clipped(gui_font_t* f, float x, float y, gui_color_t color, char* utf8, int64 n, gui_rect_t clip)
{
    _font_internal__draw_n(f, x, y, color, utf8, n, &clip);
}

int64 font__text_position(gui_font_t* f, const char* utf8, int64 length, float target_x)
{
    if (f == NULL_POINTER || utf8 == NULL_POINTER || length <= 0 || target_x <= 0)
    {
        return 0;
    }
    const ubyte* p = (const ubyte*)utf8;
    const ubyte* end = p + length;
    float x = 0;
    uint previous = 0;
    while (p < end)
    {
        int64 at = (int64)(p - (const ubyte*)utf8);
        uint cp = _font_internal__utf8_next(&p, end);
        int index = _font_internal__glyph_index(cp);
        float next = x;
        if (index >= 0)
        {
            next += _font_internal__kern(f, previous, cp) + f->packed[index].xadvance + _font_internal__tracking;
            previous = cp;
        }
        // Missing glyphs still occupy a valid caret boundary in the text.
        if (target_x < (x + next) * .5f)
        {
            return at;
        }
        x = next;
    }
    return length;
}

// ============================================================================
// internal: cache lookup
// ============================================================================

// Kerning adjustment in pixels between two consecutive codepoints, 0
// when the font has no pair data or one of the two is missing.
static float _font_internal__kern(gui_font_t* f, uint prev_cp, uint cp)
{
    _font_internal__kern_entry_t* entry;
    uint hash;
    if (f->has_kern == FALSE || prev_cp == 0 || cp == 0)
    {
        return 0.0f;
    }

    hash = (uint)f->metrics_generation * 2654435761u ^ (uint)(f->metrics_generation >> 32) ^
        prev_cp * 2246822519u ^ cp * 3266489917u;
    hash ^= hash >> 16;
    entry = &_font_internal__kern_cache[hash & (_FONT_INTERNAL__KERN_CACHE_CAP - 1)];
    if (entry->generation != f->metrics_generation || entry->previous != prev_cp || entry->current != cp)
    {
        entry->generation = f->metrics_generation;
        entry->previous = prev_cp;
        entry->current = cp;
        entry->adjustment = (float)stbtt_GetCodepointKernAdvance(&f->info, (int)prev_cp, (int)cp) * f->em_scale;
    }
    return entry->adjustment;
}

// Vertical metrics the way a browser on Windows picks them (GDI and
// DirectWrite convention): the OS/2 typo metrics when the font sets
// USE_TYPO_METRICS, else the OS/2 win metrics, with hhea as the fallback
// for fonts without an OS/2 table or with win metrics that are clearly
// broken (smaller than half the em; Freedom.ttf says 0.5 em). The line
// gap always comes from hhea. stb_truetype's own stbtt_GetFontVMetrics
// reads hhea only, which disagrees with browsers for fonts whose tables
// differ (Roboto-Medium, ProggyTiny, ArianaVioleta among the shipped ones).
// Returns 0 for the hhea fallback, 1 for typo, 2 for win metrics.
static int _font_internal__vmetrics(const stbtt_fontinfo* info, int* out_ascent, int* out_descent, int* out_line_gap)
{
    int hhea_ascent = 0;
    int hhea_descent = 0;
    int hhea_gap = 0;
    int typo_ascent = 0;
    int typo_descent = 0;
    int typo_gap = 0;
    int win_ascent = 0;
    int win_descent = 0;
    int units_per_em = 0;
    stbtt_uint32 os2 = 0;

    stbtt_GetFontVMetrics(info, &hhea_ascent, &hhea_descent, &hhea_gap);
    *out_ascent   = hhea_ascent;
    *out_descent  = hhea_descent;
    *out_line_gap = hhea_gap;

    os2 = stbtt__find_table(info->data, info->fontstart, "OS/2");

    if (os2 == 0)
    {
        return 0;
    }

    // fsSelection bit 7: USE_TYPO_METRICS
    if ((ttUSHORT(info->data + os2 + 62) & 0x80) != 0)
    {
        if (stbtt_GetFontVMetricsOS2(info, &typo_ascent, &typo_descent, &typo_gap) != 0)
        {
            *out_ascent  = typo_ascent;
            *out_descent = typo_descent;
            return 1;
        }

        return 0;
    }

    win_ascent   = (int)ttUSHORT(info->data + os2 + 74);
    win_descent  = (int)ttUSHORT(info->data + os2 + 76);
    units_per_em = (int)ttUSHORT(info->data + info->head + 18);

    // usWinAscent + usWinDescent below half the em is a broken table
    // (Freedom.ttf: 0.51 em); browsers fall back to hhea for such fonts
    if (win_ascent + win_descent >= units_per_em * 3 / 5)
    {
        *out_ascent  = win_ascent;
        *out_descent = -win_descent;
        return 2;
    }

    return 0;
}

static _font_internal__family_t* _font_internal__find_family(char* name)
{
    for (int64 i = 0; i < _font_internal__family_count; i++)
    {
        if (_font_internal__families[i].active && stdlib__strcmp(_font_internal__families[i].name, name) == 0)
        {
            return &_font_internal__families[i];
        }
    }
    return NULL_POINTER;
}

static gui_font_t* _font_internal__find_cached(int64 family_index, float size_px)
{
    for (int64 i = 0; i < _font_internal__font_count; i++)
    {
        gui_font_t* f = &_font_internal__fonts[i];
        if (f->family_index == family_index && f->size_px == size_px)
        {
            // Touch the LRU clock so this entry sticks around. Cheap
            // counter increment, called once per font__at hit.
            f->lru_tick = ++_font_internal__lru_clock;
            return f;
        }
    }
    return NULL_POINTER;
}

// Evict the least-recently-used cache entry to make room for a new
// rasterization when the cache is full. Returns the freed slot
// index (which the caller will overwrite). Releases the GPU atlas
// + any held atlas pixels first so the GPU memory comes back.
static int64 _font_internal__evict_lru(void)
{
    int64  oldest_idx  = 0;
    uint64 oldest_tick = _font_internal__fonts[0].lru_tick;
    for (int64 i = 1; i < _font_internal__font_count; i++)
    {
        if (_font_internal__fonts[i].lru_tick < oldest_tick)
        {
            oldest_tick = _font_internal__fonts[i].lru_tick;
            oldest_idx  = i;
        }
    }
    gui_font_t* victim = &_font_internal__fonts[oldest_idx];
    if (victim->atlas_tex != NULL_POINTER)
    {
        renderer__destroy_atlas(victim->atlas_tex);
        victim->atlas_tex = NULL_POINTER;
    }
    if (victim->atlas_pixels != NULL_POINTER)
    {
        GUI_FREE(victim->atlas_pixels);
        victim->atlas_pixels = NULL_POINTER;
    }
    log_info("%s %lld %s %lld %s %.1f %s", "font cache: evicted LRU slot", (long long)oldest_idx, "(family=", (long long)victim->family_index, "size=", (double)victim->size_px, ")");
    stdlib__memset(victim, 0, sizeof(*victim));
    victim->family_index = -1;
    return oldest_idx;
}

// ============================================================================
// internal: rasterize + atlas build
// ============================================================================
// called lazily the first time we see a (family, size) combo. picks an
// atlas size that's large enough to hold the full codepoint range at
// this `size_px`, runs stb_truetype's packer to rasterize every glyph
// into it, uploads the result to the renderer, frees the cpu buffer,
// caches the font. if rasterization fails (typically because we've
// maxed out at 4096^2 and the font is still too big), the cache entry
// is still written but with rasterize_failed=TRUE -- subsequent
// font__at calls for that size return NULL from the cached-failure
// branch without attempting another allocation.

static int _font_internal__pick_atlas_dim(float size_px)
{
    // rough area estimate: each glyph occupies (size_px * oversample_x
    // + padding) by (size_px + padding) pixels, times 224 glyphs,
    // times 1.5 for rect-pack overhead. take sqrt for a square side,
    // round UP to the next power of two so we land on sampler-friendly
    // dimensions.
    // we ask _pick_oversample for the factor with pixel_snap_h=FALSE
    // (the non-bitmap path) so the budget is sized for the worst-case
    // the rasterizer might actually use at this size. a bitmap family
    // at the same size will pick a smaller oversample and just leave
    // some atlas unused -- which is fine, beats under-budgeting and
    // failing to pack.
    int ovs_h;
    int ovs_v;
    _font_internal__pick_oversample(size_px, FALSE, &ovs_h, &ovs_v);

    // glyphs of an em-scaled font run past the em box: ascenders plus
    // descenders reach about 1.3 em on Roboto and wide glyphs about
    // 1.2 em, hence the factors
    float w_per = size_px * 1.25f * (float)ovs_h + 2.0f;
    float h_per = size_px * 1.5f  * (float)ovs_v + 2.0f;
    float area  = (float)_FONT_INTERNAL__NUM_CODEPOINTS * w_per * h_per * 1.5f;
    float side  = 1.0f;
    while (side* side < area)
    {
        side *= 2.0f;
    }

    int dim = (int)side;
    if (dim < _FONT_INTERNAL__ATLAS_MIN_DIM)
    {
        dim = _FONT_INTERNAL__ATLAS_MIN_DIM;
    }
    if (dim > _FONT_INTERNAL__ATLAS_MAX_DIM)
    {
        dim = _FONT_INTERNAL__ATLAS_MAX_DIM;
    }
    return dim;
}

static gui_font_t* _font_internal__rasterize(int64 family_index, float size_px)
{
    _font_internal__family_t* fam = &_font_internal__families[family_index];
    if (fam->active == FALSE || fam->ttf_bytes == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    // Reuse a hole left by family replacement, otherwise append if
    // possible or evict the least-recently-used entry. The
    // `slot_is_reuse` flag suppresses the unconditional font_count++
    // at the end of rasterize when we're recycling an existing
    // slot -- previously a decrement-here / increment-there dance
    // hid valid entries from find_cached during the gap.
    int64 slot;
    boole slot_is_reuse;
    for (slot = 0; slot < _font_internal__font_count; slot++)
    {
        if (_font_internal__fonts[slot].family_index < 0) break;
    }
    if (slot < _font_internal__font_count)
    {
        slot_is_reuse = TRUE;
    }
    else if (_font_internal__font_count < _FONT_INTERNAL__MAX_FONTS)
    {
        slot          = _font_internal__font_count;
        slot_is_reuse = FALSE;
    }
    else
    {
        slot          = _font_internal__evict_lru();
        slot_is_reuse = TRUE;
    }
    gui_font_t* f = &_font_internal__fonts[slot];
    stdlib__memset(f, 0, sizeof(*f));
    f->family_index = family_index;
    f->size_px      = size_px;
    f->lru_tick     = ++_font_internal__lru_clock;
    f->metrics_generation = ++_font_internal__metrics_clock;

    // pick an atlas size that should comfortably fit the full glyph
    // range for this font size. scaled dynamically rather than a fixed
    // constant so a 14 px body font keeps using 1024x1024 while a 73 px
    // heading pulls up to 4096x4096 transparently. stbtt_GetPackedQuad
    // reads these back via f->atlas_w / f->atlas_h when drawing.
    f->atlas_w = _font_internal__pick_atlas_dim(size_px);
    f->atlas_h = f->atlas_w;

    // record the failure path once up front: if anything below fails,
    // we jump to `fail_cache`, mark the entry rasterize_failed, and
    // increment font_count so subsequent font__at calls see the cached
    // failure and bail fast instead of re-rasterizing.
    #define _FONT_INTERNAL__RASTER_FAIL_CACHE() \
        do { \
            if (f->atlas_pixels != NULL_POINTER) \
            { \
                GUI_FREE(f->atlas_pixels); \
                f->atlas_pixels = NULL_POINTER; \
            } \
            f->rasterize_failed = TRUE; \
            if (slot_is_reuse == FALSE) { _font_internal__font_count++; } \
            return NULL_POINTER; \
        } while (0)

    // allocate the atlas bitmap. one byte per pixel (R8 alpha-only).
    // at 4096x4096 that's 16 MiB of transient CPU memory -- freed
    // right after upload to the renderer.
    int atlas_bytes = f->atlas_w * f->atlas_h;
    f->atlas_pixels = (ubyte*)GUI_CALLOC_T((size_t)atlas_bytes, 1, MM_TYPE__FONT);
    if (f->atlas_pixels == NULL_POINTER)
    {
        log_error("%s %d %s %d %s", "out of memory for atlas (", f->atlas_w, "x", f->atlas_h, ")");
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    // stbtt packer handles rect-packing + rasterization in one pass.
    // padding = 1 keeps adjacent glyphs from bleeding into each other
    // under bilinear sampling.
    stbtt_pack_context pc;
    if (stbtt_PackBegin(&pc, f->atlas_pixels, f->atlas_w, f->atlas_h, 0, 1, NULL_POINTER) == 0)
    {
        log_error("%s %.1f %s %d %s %d %s", "stbtt_PackBegin failed for size", size_px, "(atlas", f->atlas_w, "x", f->atlas_h, ")");
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    // size-adaptive oversample. imgui's formula: 2x horizontal below
    // 36 px, 1x at or above; 1x for pixel_snap_h families regardless
    // of size. reason big sizes get 1x: at >36 px the atlas samples
    // are already large enough that the GPU's LINEAR filter resolves
    // subpixel positioning on its own, so the extra horizontal
    // resolution just wastes atlas area. see
    // _font_internal__pick_oversample comment for the full story.
    int ovs_h;
    int ovs_v;
    _font_internal__pick_oversample(size_px, fam->pixel_snap_h, &ovs_h, &ovs_v);
    stbtt_PackSetOversampling(&pc, (unsigned int)ovs_h, (unsigned int)ovs_v);

    // Pack two ranges into the same atlas:
    // 1. The contiguous Latin-1 range 32..255 (NUM_CODEPOINTS glyphs).
    // 2. The non-contiguous extras list (BULLET, arrows, etc.).
    // stbtt_PackFontRanges handles both in one rect-pack pass so the
    // extras don't fragment the atlas. chardata_for_range pointers
    // share the same packed[] allocation -- range 1 writes [0..224),
    // range 2 writes [224..232).
    // first_unicode_codepoint_in_range + num_chars define range 1;
    // array_of_unicode_codepoints + num_chars define range 2 (stb
    // disambiguates via first_unicode_codepoint_in_range == 0).
    int extras_array[_FONT_INTERNAL__EXTRA_COUNT];
    for (int i = 0; i < _FONT_INTERNAL__EXTRA_COUNT; i++)
    {
        extras_array[i] = _font_internal__extra_codepoints[i];
    }

    stbtt_pack_range ranges[2];
    stdlib__memset(ranges, 0, sizeof(ranges));
    // negative: stb_truetype maps the em (unitsPerEm) to size_px pixels, the CSS meaning of font-size; positive would map ascent + descent instead
    ranges[0].font_size                        = -size_px;
    ranges[0].first_unicode_codepoint_in_range = _FONT_INTERNAL__FIRST_CODEPOINT;
    ranges[0].array_of_unicode_codepoints      = NULL_POINTER;
    ranges[0].num_chars                        = _FONT_INTERNAL__NUM_CODEPOINTS;
    ranges[0].chardata_for_range               = &f->packed[0];
    ranges[1].font_size                        = -size_px;
    ranges[1].first_unicode_codepoint_in_range = 0;
    ranges[1].array_of_unicode_codepoints      = extras_array;
    ranges[1].num_chars                        = _FONT_INTERNAL__EXTRA_COUNT;
    ranges[1].chardata_for_range               = &f->packed[_FONT_INTERNAL__NUM_CODEPOINTS];

    int ok = stbtt_PackFontRanges(&pc, fam->ttf_bytes, 0, ranges, 2);
    stbtt_PackEnd(&pc);

    if (ok == FALSE)
    {
        // atlas still couldn't hold the range (we already capped at
        // 4096x4096 via pick_atlas_dim). cache the failure so we don't
        // retry every frame -- a noisy text widget would otherwise
        // allocate + log + free 16 MiB per frame.
        log_error("%s %.1f %s %d %s %d %s", "stbtt_PackFontRange failed for size", size_px, "at", f->atlas_w, "x", f->atlas_h, "; caching as failed");
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    // pixel_snap_h families: round every glyph's x-advance to the
    // nearest integer so consecutive glyphs always land on integer
    // pixel columns. without this, a string of 13 px ProggyClean
    // glyphs drifts half-pixel by half-pixel across a line and the
    // pristine bitmap design starts to smear. matches imgui's
    // `if (src->PixelSnapH) advance_x = IM_ROUND(advance_x)` step.
    if (fam->pixel_snap_h)
    {
        for (int gi = 0; gi < _FONT_INTERNAL__TOTAL_PACKED; gi++)
        {
            float a = f->packed[gi].xadvance;
            f->packed[gi].xadvance = (float)((int)(a + 0.5f));
        }
    }

    // grab vertical metrics directly from the TTF so line spacing
    // matches what the font designer intended.
    stbtt_fontinfo info;
    stbtt_InitFont(&info, fam->ttf_bytes, stbtt_GetFontOffsetForIndex(fam->ttf_bytes, 0));
    float scale = stbtt_ScaleForMappingEmToPixels(&info, size_px);
    f->info     = info;
    f->em_scale = scale;
    f->has_kern = (boole)(fam->pixel_snap_h == FALSE && (info.kern != 0 || info.gpos != 0));
    int   ascent_i;
    int   descent_i;
    int   line_gap_i;
    int   metric_source = _font_internal__vmetrics(&info, &ascent_i, &descent_i, &line_gap_i);
    // ascent, descent and line gap rounded to whole pixels, each on its
    // own: that is how a browser derives a font's normal line height,
    // so text boxes here are the size the same text gets in HTML
    f->ascent      = (float)(int)((float)ascent_i * scale + 0.5f);
    f->descent     = -(float)(int)((float)(-descent_i) * scale + 0.5f); // negative: distance below the baseline
    f->line_gap    = (float)(int)((float)line_gap_i * scale + 0.5f);
    if (metric_source == 0)
    {
        // a font without usable OS/2 metrics gets its hhea total rounded
        // as a whole, halves to even, and the descent takes the difference:
        // measured on Edge with Freedom.ttf (22.5 px gives 22, not 17 + 6)
        float total   = (float)(ascent_i - descent_i + line_gap_i) * scale;
        int   rounded = (int)(total + 0.5f);
        if ((float)rounded == total + 0.5f && (rounded & 1) == 1)
        {
            rounded = rounded - 1;
        }
        f->descent = -((float)rounded - f->ascent - f->line_gap);
    }
    f->line_height = f->ascent - f->descent + f->line_gap;

    // upload the bitmap to the renderer. we ask for R8 (single channel,
    // which the fragment shader interprets as alpha). the renderer
    // returns an opaque handle that we store + free at shutdown.
    f->atlas_tex = renderer__create_atlas_r8(f->atlas_pixels, f->atlas_w, f->atlas_h);

    // done with the cpu-side bitmap. the GPU has its own copy now.
    GUI_FREE(f->atlas_pixels);
    f->atlas_pixels = NULL_POINTER;

    if (f->atlas_tex == NULL_POINTER)
    {
        log_error("%s %.1f %s %d %s %d %s", "renderer__create_atlas_r8 returned NULL (size", size_px, ", atlas", f->atlas_w, "x", f->atlas_h, ")");
        _FONT_INTERNAL__RASTER_FAIL_CACHE();
    }

    if (slot_is_reuse == FALSE)
    {
        _font_internal__font_count++;
    }
    return f;
    #undef _FONT_INTERNAL__RASTER_FAIL_CACHE
}

// ============================================================================
// internal: directory scan + auto-register
// ============================================================================
// enumerate *.ttf in `dir_path` and call font__register_from_file for each,
// using the file's basename (without the .ttf extension) as the family
// name. so "gui/src/fonts/Roboto-Regular.ttf" becomes family "Roboto-Regular",
// referenced from .style as `font_family: Roboto-Regular;`.
// called from font__init with GUI_FONTS_SOURCE_DIR. missing directory is
// not fatal -- we log and return. failing to register an individual file
// is also not fatal; other fonts still load.
// uses Win32 FindFirstFileW / FindNextFileW. needs UTF-16 paths, which we
// build by concatenating dir_path + "\\*.ttf" in UTF-8 and converting.

static void _font_internal__scan_and_register_dir(char* dir_path)
{
#if !defined(_WIN32)
    // Non-Windows scan. Android routes font loading through its
    // own AAssetManager path inside platform_android.c (APK assets
    // aren't on a POSIX filesystem), so we skip the opendir walk
    // there. Everywhere else -- Linux DRM/X11, macOS Cocoa, etc. --
    // we scan the directory for *.ttf files the same way the Win32
    // branch does, just via opendir/readdir instead of FindFirstFile.
  #if defined(__ANDROID__)
    (void)dir_path;
    return;
  #else
    if (dir_path == NULL_POINTER || dir_path[0] == 0)
    {
        return;
    }
    DIR* d = opendir(dir_path);
    if (d == NULL_POINTER)
    {
        log_warn("%s %s %s %s %s", "fonts: scan '", dir_path, "' failed (opendir:", strerror(errno), ")");
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL_POINTER)
    {
        // Suffix check: must end in ".ttf" (case-insensitive). Skip
        // "." / ".." and anything that isn't a regular file (symlinks
        // to a real .ttf still resolve through the subsequent open).
        size_t nlen = stdlib__strlen(ent->d_name);
        if (nlen < 5)
        {
            continue;
        }
        const char* suf = ent->d_name + nlen - 4;
        if ((suf[0] == '.' && (suf[1] == 't' || suf[1] == 'T') && (suf[2] == 't' || suf[2] == 'T') && (suf[3] == 'f' || suf[3] == 'F')) == FALSE)
        {
            continue;
        }
        // Family name = filename stem; build absolute path from
        // dir_path + '/' + filename for font__register_from_file.
        char family[64];
        size_t stem_len = nlen - 4;
        if (stem_len >= sizeof(family))
        {
            stem_len = sizeof(family) - 1;
        }
        stdlib__memcpy(family, ent->d_name, stem_len);
        family[stem_len] = 0;

        char path[1024];
        size_t dlen = stdlib__strlen(dir_path);
        if (dlen + 1 + nlen + 1 > sizeof(path))
        {
            continue;
        }
        stdlib__memcpy(path, dir_path, dlen);
        path[dlen] = '/';
        stdlib__memcpy(path + dlen + 1, ent->d_name, nlen + 1);

        if (font__register_from_file(family, path) == FALSE)
        {
            log_warn("%s %s %s", "fonts: failed to register '", path, "'");
        }
    }
    closedir(d);
  #endif
    return;
#else
    if (dir_path == NULL_POINTER || dir_path[0] == 0)
    {
        return;
    }

    // build the search pattern: "<dir_path>\*.ttf". 512 chars is enough
    // for any reasonable absolute path on windows (MAX_PATH = 260).
    char pattern[1024];
    size_t n = stdlib__strlen(dir_path);
    if (n + 8 >= sizeof(pattern))
    {
        log_error("%s %s %s", "fonts dir path too long: '", dir_path, "'");
        return;
    }
    stdlib__memcpy(pattern, dir_path, n);
    // append "\*.ttf". forward or backslashes both work on windows, but
    // we use backslash for consistency with the other Win32 code in this
    // project.
    pattern[n++] = '\\';
    pattern[n++] = '*';
    pattern[n++] = '.';
    pattern[n++] = 't';
    pattern[n++] = 't';
    pattern[n++] = 'f';
    pattern[n]   = 0;

    // convert to UTF-16 for FindFirstFileW.
    WCHAR wpattern[1024];
    int wn = MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, (int)(sizeof(wpattern) / sizeof(wpattern[0])));
    if (wn == 0)
    {
        log_error("%s %s %s", "MultiByteToWideChar failed for pattern '", pattern, "'");
        return;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        // directory empty / missing. that's allowed -- the host app can
        // still register fonts manually via font__register_from_file.
        return;
    }

    do
    {
        // skip subdirectories; we only want .ttf files at this level.
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }

        // convert WIN32_FIND_DATAW::cFileName (UTF-16) to UTF-8 filename
        // and absolute path.
        char filename_utf8[512];
        int  fn = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, filename_utf8, (int)sizeof(filename_utf8), NULL_POINTER, NULL_POINTER);
        if (fn == 0)
        {
            continue;
        }

        // strip the ".ttf" extension to derive the family name.
        char family[128];
        size_t fnl = stdlib__strlen(filename_utf8);
        if (fnl < 5)
        {
            continue; // shorter than "x.ttf" -- shouldn't happen but skip defensively.
        }
        size_t stem_len = fnl - 4; // drop ".ttf".
        if (stem_len >= sizeof(family))
        {
            stem_len = sizeof(family) - 1;
        }
        stdlib__memcpy(family, filename_utf8, stem_len);
        family[stem_len] = 0;

        // build the full path: "<dir_path>\<filename>".
        char full_path[1024];
        size_t dl = stdlib__strlen(dir_path);
        if (dl + 1 + fnl >= sizeof(full_path))
        {
            log_error("%s %s %s", "full path too long for '", filename_utf8, "'");
            continue;
        }
        stdlib__memcpy(full_path, dir_path, dl);
        full_path[dl] = '\\';
        stdlib__memcpy(full_path + dl + 1, filename_utf8, fnl + 1); // include NUL.

        if (font__register_from_file(family, full_path) == FALSE)
        {
            log_error("%s %s %s", "auto-register failed for '", full_path, "'");
            continue;
        }
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);
#endif // _WIN32
}
