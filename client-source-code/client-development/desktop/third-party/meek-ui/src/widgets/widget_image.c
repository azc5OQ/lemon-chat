// widget_image.c - <image src="..."/> decoder + drawer.
// Pipeline:
// 1. parser_xml sees <image src="foo.png">, builds a GUI_NODE_TYPE__IMAGE
// node, then calls apply_attribute("src", "foo.png") on us.
// 2. We load the file through fs__read_entire_file so it works
// identically on Windows (absolute path / DEMO_SOURCE_DIR) and
// Android (AAssetManager / sideload dir).
// 3. We decode the file bytes with stb_image into a tightly-packed
// RGBA8 buffer. JPEG and PNG both go through the same call --
// stb_image auto-detects.
// 4. We hand the pixel buffer to renderer__create_texture_rgba and
// stash the returned handle (plus width/height) on the node's
// user_data slot.
// 5. Layout sizes the node using either the style's size (if set)
// or the image's natural pixel dimensions.
// 6. emit_draws submits the textured quad via renderer__submit_image,
// optionally falling back to a bg rect underneath for loading /
// decode-failure visual feedback.
// 7. on_destroy frees the GPU texture and the user_data struct.
// Standalone and embedded backends share this decoder. An unsuccessful
// texture upload leaves the widget's styled fallback background visible.
// Supported formats: everything stb_image does -- PNG, JPEG, BMP, GIF
// (first frame), TGA, PSD, HDR. .ico isn't supported; skip it.

#include <string.h>
#include <stdlib.h> // getenv -- only used by the Linux icon-path bypass

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "../atlas.h"
#include "fs.h"
#include "parser_xml.h"
#include "style_resources.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "meek_ui_config.h"
#include "third_party/log.h"

// stb_image owns its own memory hooks; we tell it to use our tracker
// by overriding STBI_MALLOC / STBI_FREE / STBI_REALLOC before the
// implementation is included. The realloc needs a size-aware sizing
// callback; stb_image only provides the old-size parameter to
// STBI_REALLOC_SIZED, which is what we want.
// Textures are large (RGBA8 = 4 bytes per pixel; a 1024x1024 image is
// 4 MiB) so routing through the tracker gives us useful live-bytes
// figures under MM_TYPE__IMAGE.
// WebP decoder. Single-header library (tiny_webp.h). Implementation
// macro defined here so the static lib lands in widget_image.c
// only; other TUs that #include "tiny_webp.h" get declarations
// only. Returns RGBA8 pixels matching stb_image's tight pack, so
// the rest of the load path can treat the buffer identically.
#define twp_MALLOC(n) GUI_MALLOC_T((n), MM_TYPE__IMAGE)
#define twp_CALLOC(n, s) GUI_CALLOC_T((n), (s), MM_TYPE__IMAGE)
#define twp_REALLOC(p, n) GUI_REALLOC((p), (n))
#define twp_FREE(p) GUI_FREE(p)
// tiny_webp likewise: its file entry points go, the byte ones stay
#define twp_NO_STDIO
#define twp_IMPLEMENTATION
#include "third_party/tiny_webp.h"

#define STBI_NO_STDIO // we pass bytes via stbi_load_from_memory.
#if !MEEK_UI_HOST_SERVICES_FALLBACK
    // A build with no C runtime drops the two stb paths that need one:
    // the Radiance HDR loader (pow, ldexp and strtol) and the per-thread
    // failure string (thread-local storage). Neither is reachable from a
    // .ui file, which loads PNG, JPEG, BMP, GIF, TGA and WebP.
    #define STBI_NO_HDR
    #define STBI_NO_LINEAR
    #define STBI_NO_THREAD_LOCALS
#endif
#define STBI_MALLOC(sz)               GUI_MALLOC_T((sz),    MM_TYPE__IMAGE)
#define STBI_REALLOC(p, sz)           GUI_REALLOC((p), (sz))
#define STBI_REALLOC_SIZED(p, old, n) GUI_REALLOC((p), (n))
#define STBI_FREE(p)                  GUI_FREE((p))
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

// NanoSVG uses local primitives and explicit allocator hooks. Input bytes
// come from fs__read_entire_file; the parser has no filesystem entry point.
#define NSVG_MALLOC(sz)      GUI_MALLOC_T((sz), MM_TYPE__IMAGE)
#define NSVG_REALLOC(p, sz)  GUI_REALLOC((p), (sz))
#define NSVG_FREE(p)         GUI_FREE((p))
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"
#include "clib/stdlib.h"

// Per-node state. `tex == NULL` means "either decode hasn't happened
// yet, or it failed"; both cases emit the bg rect instead of an
// image. natural_w / natural_h are the image's own pixel dimensions
// from stb_image, used by layout when the style has no explicit size.
// Atlas-or-private-texture: every loaded image is in exactly one
// of two states.
// atlas_page >= 0 : pixels live in atlas page `atlas_page` at
// pixel rect `atlas_uv`. `tex` is NULL (no
// per-image GL texture to destroy).
// atlas_page == -1: pixels live in private texture `tex` (legacy
// path). Used when an image is too large for
// an atlas page, or atlas allocation failed.
// Atlas path is the fast one (consecutive icons sharing an atlas
// collapse into a single batched draw call). The fallback covers
// wallpapers, fullscreen mimics, anything bigger than the atlas
// can hold.
typedef struct _widget_image_internal__state_t
{
    // private texture (fallback); NULL when atlased.
    void*    tex;          // 0x0
    // >=0 = atlased; -1 = private tex (or unloaded).
    int      atlas_page;   // 0x8
    // pixel rect within atlas page; valid iff atlas_page >= 0.
    gui_rect_t atlas_uv;     // 0xC
    // decoded pixel width.
    int      natural_w;    // 0x1C
    // decoded pixel height.
    int      natural_h;    // 0x20
    // path as written in the .ui, kept for logs.
    char     src[256];     // 0x24
} _widget_image_internal__state_t;

static _widget_image_internal__state_t* _widget_image_internal__state_of(gui_node_t* n)
{
    if (n->user_data == NULL_POINTER)
    {
        // Lazy allocation. Keeps init_defaults optional and means nodes
        // that never get an src attribute (broken .ui) don't leak a
        // zero-filled state struct.
        n->user_data = GUI_CALLOC_T(1, sizeof(_widget_image_internal__state_t), MM_TYPE__IMAGE);
        if (n->user_data != NULL_POINTER)
        {
            // atlas_page defaults to 0 from CALLOC, but 0 is a VALID
            // page id (the first atlas). Bias to -1 so "not atlased"
            // is the well-defined initial state.
            ((_widget_image_internal__state_t*)n->user_data)->atlas_page = -1;
        }
    }
    return (_widget_image_internal__state_t*)n->user_data;
}

// Read the file (with platform-native fs -- AAssetManager on Android,
// sideload override on both, fopen-equivalent on Windows), decode via
// stb_image, upload to the GPU, stash the handle on the node. On any
// failure we log once and leave tex at NULL so the widget degrades to
// its bg rect.
// Reject paths that contain ".." segments or absolute roots --
// they point outside the asset tree and a malicious .ui file
// could otherwise read arbitrary files. Allows '/' inside the
// path (subfolders are fine), forbids it as a leading char
// (absolute), forbids ':' (Windows drive letters), and forbids
// any "/.." or "..\\" segment. Returns FALSE if the path is
// rejected; the caller logs + bails.
// On Linux desktops we allow absolute paths that point inside the
// standard XDG icon-theme directories so the host can render
// installed-app icons (.desktop Icon= value -> hicolor lookup ->
// /usr/share/icons/...). The wider safety check above still rejects
// arbitrary absolute paths; this just whitelists the prefixes a
// shell legitimately needs. Android uses AAssetManager for assets,
// not absolute paths, so the bypass is excluded there.
#if defined(__linux__) && !defined(__ANDROID__)
static boole _widget_image_internal__is_trusted_icon_path(const char* p)
{
    static const char* prefixes[] = {
        "/usr/share/icons/",
        "/usr/share/pixmaps/",
        "/usr/local/share/icons/",
        "/usr/local/share/pixmaps/",
        "/run/host/usr/share/icons/", // flatpak host-spawned shells
        NULL_POINTER,
    };
    for (int i = 0; prefixes[i] != NULL_POINTER; i++)
    {
        const char* pre = prefixes[i];
        size_t plen = 0;
        while (pre[plen] != 0)
        {
            plen++;
        }
        size_t qlen = 0;
        while (p[qlen] != 0)
        {
            qlen++;
        }
        if (qlen <= plen)
        {
            continue;
        }
        boole match = TRUE;
        for (size_t j = 0; j < plen; j++)
        {
            if (p[j] != pre[j])
            {
                match = FALSE;
                break;
            }
        }
        if (match)
        {
            return TRUE;
        }
    }
    // ~/.icons/, ~/.local/share/icons/, ~/.local/share/pixmaps/
    // resolved against $HOME.
    const char* home = getenv("HOME");
    if (home != NULL_POINTER && home[0] != 0)
    {
        const char* user_subs[] = {
            "/.icons/",
            "/.local/share/icons/",
            "/.local/share/pixmaps/",
            NULL_POINTER,
        };
        for (int i = 0; user_subs[i] != NULL_POINTER; i++)
        {
            char joined[512];
            int64 n = 0;
            stdlib__strcpy_safe(joined, (int64)sizeof(joined), home);
            n = stdlib__strcat_safe(joined, (int64)sizeof(joined), user_subs[i]);
            if (n <= 0 || n >= (int64)sizeof(joined))
            {
                continue;
            }
            size_t jlen = (size_t)n;
            size_t qlen = 0;
            while (p[qlen] != 0)
            {
                qlen++;
            }
            if (qlen <= jlen)
            {
                continue;
            }
            boole match = TRUE;
            for (size_t k = 0; k < jlen; k++)
            {
                if (p[k] != joined[k])
                {
                    match = FALSE;
                    break;
                }
            }
            if (match)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}
#endif

static boole _widget_image_internal__path_safe(const char* p)
{
    if (p == NULL_POINTER || p[0] == 0)
    {
        return FALSE;
    }
    // Absolute path: leading '/' on POSIX, leading drive-letter+':'
    // on Windows ("C:..." / "c:..."), leading '\\' on Windows UNC.
    // On Linux we make a narrow exception for paths under known
    // icon-theme roots; the ".." scan below still applies to those,
    // so an attacker who somehow produced "/usr/share/icons/../../etc/passwd"
    // still gets rejected.
#if defined(__linux__) && !defined(__ANDROID__)
    if (p[0] == '/' && _widget_image_internal__is_trusted_icon_path(p) == FALSE)
    {
        return FALSE;
    }
#else
    if (p[0] == '/' || p[0] == '\\')
    {
        return FALSE;
    }
#endif
    if (p[0] != 0 && p[1] == ':')
    {
        return FALSE;
    }
    // ".." segment scan. Walks once over the string, looking for
    // a '.' that's either at the start or after a path separator,
    // followed by another '.', followed by a separator or string
    // end. Conservative: rejects "foo/..bar" too (no real images
    // are named like that).
    for (const char* c = p; *c != 0; c++)
    {
        boole at_seg_start = (c == p) || (*(c - 1) == '/') || (*(c - 1) == '\\');
        if (at_seg_start && c[0] == '.' && c[1] == '.')
        {
            char trailing = c[2];
            if (trailing == 0 || trailing == '/' || trailing == '\\')
            {
                return FALSE;
            }
        }
    }
    return TRUE;
}

// Hard cap on decoded image bytes BEFORE handing to stb_image.
// stb_image is a 10k-line single-header parser with a history of
// CVEs; constraining the input size limits blast radius from a
// malicious / malformed PNG. 32 MiB covers any reasonable UI
// asset (a 4096x4096 RGBA8 is 64 MiB; we don't expect anything
// near that for a button or background). Tune if a host needs
// genuinely huge textures.
#define _WIDGET_IMAGE_INTERNAL__MAX_INPUT_BYTES (32 * 1024 * 1024)

static void _widget_image_internal__load(gui_node_t* n, char* src)
{
    _widget_image_internal__state_t* st = _widget_image_internal__state_of(n);
    if (st == NULL_POINTER)
    {
        return;
    }

    // Path safety check. Reject ".." segments + absolute paths
    // before doing any IO so a hostile .ui can't escape the
    // asset directory.
    if (_widget_image_internal__path_safe(src) == FALSE)
    {
        log_warn("%s %s %s", "image: rejecting unsafe src '", src, "' (absolute path or '..' segment)");
        return;
    }

    // Replace a previously-loaded texture if src changes (e.g. hot
    // reload after the user edits the .ui). renderer__destroy_texture
    // tolerates NULL so the first-load path is safe.
    if (st->tex != NULL_POINTER)
    {
        renderer__destroy_texture(st->tex);
        st->tex = NULL_POINTER;
    }
    st->natural_w = 0;
    st->natural_h = 0;

    // Remember src for the log message on decode failure. Truncate
    // silently; the tag is a dev-convenience, not a correctness lever.
    size_t slen = 0;
    while (src[slen] != 0 && slen + 1 < sizeof(st->src))
    {
        slen++;
    }
    stdlib__memcpy(st->src, src, slen);
    st->src[slen] = 0;

    // Try <base_dir>/<src> first so a .ui file can reference sibling
    // assets by bare name ("gradient.png") and still work from any
    // CWD. base_dir is empty until a .ui has been loaded (Windows
    // runs hot_reload->parser_xml first, so by the time this fires
    // it has the value). Fall back to the raw src so absolute paths
    // and Android asset names (which live in AAssetManager, not the
    // dev-tree base_dir) still resolve.
    int64 size  = 0;
    char* bytes = style_resources__read(src, &size);

    const char* base = parser_xml__base_dir();
    if (bytes == NULL_POINTER && base != NULL_POINTER && base[0] != 0)
    {
        char joined[1024];
        size_t bl = 0;
        while (base[bl] != 0 && bl + 1 < sizeof(joined))
        {
            joined[bl] = base[bl];
            bl++;
        }
        size_t sl = 0;
        while (src[sl] != 0 && bl + sl + 1 < sizeof(joined))
        {
            joined[bl + sl] = src[sl];
            sl++;
        }
        joined[bl + sl] = 0;
        bytes = fs__read_entire_file(joined, &size);
    }

    if (bytes == NULL_POINTER)
    {
        bytes = fs__read_entire_file(src, &size);
    }
    if (bytes == NULL_POINTER)
    {
        log_error("%s %s %s", "image: failed to read '", src, "'");
        return;
    }

    // Cap the input byte count before handing to stb_image. A
    // multi-GB image file would otherwise drive stb's parser deep
    // into untrusted territory; we'd rather fail loud and early.
    if (size > _WIDGET_IMAGE_INTERNAL__MAX_INPUT_BYTES)
    {
        log_warn("%s %s %s %lld %s %d %s", "image: '", src, "' is", (long long)size, "bytes, exceeds", _WIDGET_IMAGE_INTERNAL__MAX_INPUT_BYTES, "byte cap; rejecting");
        GUI_FREE(bytes);
        return;
    }

    // Branch on format: SVG vs raster (PNG/JPEG/etc).
    // Detection: extension '.svg' OR a quick signature peek for
    // '<svg' / '<?xml' in the first 256 bytes. Some XDG icons
    // strip the XML preamble and start straight with `<svg ...>`.
    // Doing both lets us catch SVGs that lack the .svg extension
    // (rare but possible -- e.g. .desktop's Icon=foo, resolver
    // returns foo.png that's actually inline SVG; doesn't happen
    // in practice but the cost of the peek is ~32 byte memcmp).
    boole is_svg = FALSE;
    {
        size_t slen2 = 0; while (src[slen2] != 0) { slen2++; }
        if (slen2 >= 4 && (src[slen2-4] == '.') && (src[slen2-3] == 's' || src[slen2-3] == 'S') && (src[slen2-2] == 'v' || src[slen2-2] == 'V') && (src[slen2-1] == 'g' || src[slen2-1] == 'G'))
        {
            is_svg = TRUE;
        }
        else if (size >= 5)
        {
            int probe_n = (int)(size < 256 ? size : 256);
            for (int i = 0; i + 4 < probe_n; i++)
            {
                if (bytes[i] == '<' && (bytes[i+1] == 's' || bytes[i+1] == 'S') && (bytes[i+2] == 'v' || bytes[i+2] == 'V') && (bytes[i+3] == 'g' || bytes[i+3] == 'G') && (bytes[i+4] == ' ' || bytes[i+4] == '>'))
                {
                    is_svg = TRUE;
                    break;
                }
            }
        }
    }

    // WebP detection: extension '.webp' OR the canonical
    // RIFF....WEBP magic in the first 12 bytes. Same dual-check
    // pattern as SVG so files served with the wrong extension
    // still decode.
    boole is_webp = FALSE;
    if (is_svg == FALSE)
    {
        size_t slen3 = 0; while (src[slen3] != 0) { slen3++; }
        if (slen3 >= 5 && src[slen3-5] == '.' && (src[slen3-4] == 'w' || src[slen3-4] == 'W') && (src[slen3-3] == 'e' || src[slen3-3] == 'E') && (src[slen3-2] == 'b' || src[slen3-2] == 'B') && (src[slen3-1] == 'p' || src[slen3-1] == 'P'))
        {
            is_webp = TRUE;
        }
        else if (size >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
        {
            is_webp = TRUE;
        }
    }

    int w = 0, h = 0;
    stbi_uc* pixels = NULL_POINTER;
    boole pixels_owned_by_stb = FALSE;

    if (is_webp)
    {
        // WebP decode via tiny_webp. Returns RGBA8 tight-packed,
        // caller-frees via GUI_FREE(). Same downstream handling as
        // the SVG path (pixels_owned_by_stb=FALSE -> GUI_FREE on
        // cleanup). On decode failure we fall through to the
        // stb_image path -- it'll fail too, but at least the
        // single shared error log line covers both cases.
        int wp_w = 0, wp_h = 0;
        unsigned char* wp_pixels = twp_read_from_memory(bytes, (int)size, &wp_w, &wp_h, twp_FORMAT_RGBA, 0);
        GUI_FREE(bytes);
        bytes = NULL_POINTER;
        if (wp_pixels == NULL_POINTER)
        {
            log_error("%s %s %s %lld %s", "image: tiny_webp decode failed for '", src, "' (size=", (long long)size, ")");
            return;
        }
        // Decoder output and scratch allocations use the same tracker.
        pixels = wp_pixels;
        w = wp_w;
        h = wp_h;
        pixels_owned_by_stb = FALSE;
    }
    else if (is_svg)
    {
        // nanosvg parses ASCII / UTF-8; nsvgParse modifies its
        // input in place so we copy into a +1 buffer with an
        // explicit terminator. The +1 also covers files lacking a
        // trailing nul.
        char* svg_text = (char*)GUI_MALLOC_T((size_t)size + 1, MM_TYPE__IMAGE);
        if (svg_text == NULL_POINTER)
        {
            log_error("%s %s %s", "image: oom allocating svg text buffer for '", src, "'");
            GUI_FREE(bytes);
            return;
        }
        stdlib__memcpy(svg_text, bytes, (size_t)size);
        svg_text[size] = 0;
        GUI_FREE(bytes);
        bytes = NULL_POINTER;

        NSVGimage* svg_img = nsvgParse(svg_text, "px", 96.0f);
        GUI_FREE(svg_text);
        if (svg_img == NULL_POINTER)
        {
            log_error("%s %s %s", "image: nanosvg parse failed for '", src, "'");
            return;
        }

        // Rasterize at the SVG's natural width/height when
        // reasonable; otherwise default 256. Also clamp absurdly
        // large values: SVGs are vector and their viewBox can claim
        // arbitrary numbers, but we don't want a 10000x10000 buffer.
        int rw = (int)svg_img->width;
        int rh = (int)svg_img->height;
        if (rw < 16)
        {
            rw = 256;
        }
        if (rh < 16)
        {
            rh = 256;
        }
        if (rw > 1024)
        {
            rw = 1024;
        }
        if (rh > 1024)
        {
            rh = 1024;
        }

        unsigned char* svg_pixels = (unsigned char*)GUI_MALLOC_T((size_t)rw * (size_t)rh * 4, MM_TYPE__IMAGE);
        if (svg_pixels == NULL_POINTER)
        {
            nsvgDelete(svg_img);
            log_error("%s %s %s %d %s %d %s", "image: oom allocating svg raster buffer for '", src, "' (", rw, "x", rh, ")");
            return;
        }

        NSVGrasterizer* rast = nsvgCreateRasterizer();
        if (rast == NULL_POINTER)
        {
            GUI_FREE(svg_pixels);
            nsvgDelete(svg_img);
            log_error("%s %s %s", "image: nsvgCreateRasterizer failed for '", src, "'");
            return;
        }

        // Scale = rw / svg.width so the SVG fills the raster
        // buffer. Equivalent to a "fit-to-buffer" rasterize.
        float scale_x = (svg_img->width  > 0.0f) ? (float)rw / svg_img->width  : 1.0f;
        float scale_y = (svg_img->height > 0.0f) ? (float)rh / svg_img->height : 1.0f;
        float scale_uniform = (scale_x < scale_y) ? scale_x : scale_y;

        nsvgRasterize(rast, svg_img, 0.0f, 0.0f, scale_uniform, svg_pixels, rw, rh, rw * 4);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg_img);

        pixels = svg_pixels;
        w = rw;
        h = rh;
        pixels_owned_by_stb = FALSE;
    }
    else
    {
        // Raster path: stb_image decode. Forcing 4-channel output
        // means we don't have to branch the renderer on channel
        // count -- always RGBA8.
        int channels = 0;
        pixels = stbi_load_from_memory((stbi_uc*)bytes, (int)size, &w, &h, &channels, 4);
        GUI_FREE(bytes);
        bytes = NULL_POINTER;
        if (pixels == NULL_POINTER)
        {
            log_error("%s %s %s %s %s", "image: stb_image decode failed for '", src, "' (", stbi_failure_reason(), ")");
            return;
        }
        pixels_owned_by_stb = TRUE;
    }

    // Sanity-clamp decoded dimensions. A maliciously crafted PNG
    // header can claim 16384x16384 at 4 bytes/pixel = 1 GiB; if
    // stb_image actually allocates that we'd OOM. The check fires
    // post-decode (stb already paid the alloc cost), but at least
    // we don't try to upload to the GPU.
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
    {
        log_warn("%s %s %s %d %s %d %s", "image: '", src, "' decoded to unreasonable", w, "x", h, "; rejecting");
        if (pixels_owned_by_stb)
        {
            stbi_image_free(pixels);
        }
        else
        {
            GUI_FREE(pixels);
        }
        return;
    }

    st->natural_w = w;
    st->natural_h = h;
    st->tex        = NULL_POINTER;
    st->atlas_page = -1;
    // Try the atlas first. Small images (most app icons) fit and
    // batch with all the others sharing the same page -- one
    // draw call per atlas page per frame instead of one per icon.
    // Atlas rejects sizes larger than (page - 2*pad) on either
    // axis; for those we fall through to the legacy per-image
    // texture path so wallpapers and fullscreen mimics still load.
    {
        int      page = -1;
        gui_rect_t rect_px;
        if (atlas__alloc(w, h, &page, &rect_px))
        {
            atlas__upload(page, rect_px, (const unsigned char*)pixels);
            st->atlas_page = page;
            st->atlas_uv   = rect_px;
        }
    }

    // Fallback path -- private GL texture, one draw call per submit.
    // Same behaviour as the original (pre-atlas) code, kept for
    // backends without an image pipeline, oversized images, and
    // atlas-allocation failures (page count cap, OOM).
    if (st->atlas_page < 0)
    {
        st->tex = renderer__create_texture_rgba((const ubyte*)pixels, w, h);
    }

    if (pixels_owned_by_stb)
    {
        stbi_image_free(pixels);
    }
    else
    {
        GUI_FREE(pixels);
    }

    if (st->atlas_page < 0 && st->tex == NULL_POINTER)
    {
        // Renderer backend without an image pipeline (d3d11 / d3d9
        // stubs return NULL). Not an error -- we keep natural_w/_h so
        // layout still reserves the right amount of space, and the
        // draw path falls through to the bg rect.
        log_info("%s %s %s", "image: renderer has no texture support; '", src, "' will render as bg rect");
    }
    else
    {
        log_info("%s %s %s %d %s %d %s %s", "image: loaded '", src, "' (", w, "x", h, ")", st->atlas_page >= 0 ? "(atlased)" : "(private tex)");
    }
}

static boole image_apply_attribute(gui_node_t* n, char* name, char* value)
{
    if (stdlib__strcmp(name, "src") == 0)
    {
        _widget_image_internal__load(n, value);
        return TRUE;
    }
    return FALSE;
}

static void image_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    gui_style_t* s = &n->resolved;
    _widget_image_internal__state_t* st = (_widget_image_internal__state_t*)n->user_data;

    // Priority: explicit style size > natural pixel size > avail.
    // Natural size is NOT multiplied by scale -- the image's pixel
    // dimensions are already absolute, and scaling them as if they
    // were logical-pixel style sizes would double-apply the DPI
    // factor on high-DPI Android devices.
    // Priority order:
    // 1. explicit pixel size (`size: WxH;` or `width: NNpx;`)
    // 2. percentage of the parent-offered space
    // (`width: 100%;`) -- forces uniform tiles when an
    // <image> sits in a grid/collection where the parent
    // hands every child the same avail_w/avail_h. Without
    // this branch the image falls back to natural pixel
    // dimensions and tiles end up sized to the source files
    // rather than the slot they live in.
    // 3. natural pixel size of the decoded image
    // 4. parent-offered avail.
    float w;
    if (s->size_w > 0.0f)
    {
        w = s->size_w * scale;
    }
    else if (s->width_pct > 0.0f)
    {
        w = avail_w * (s->width_pct / 100.0f);
    }
    else if (st != NULL_POINTER && st->natural_w > 0)
    {
        w = (float)st->natural_w;
    }
    else
    {
        w = avail_w;
    }

    float h;
    if (s->size_h > 0.0f)
    {
        h = s->size_h * scale;
    }
    else if (s->height_pct > 0.0f)
    {
        h = avail_h * (s->height_pct / 100.0f);
    }
    else if (st != NULL_POINTER && st->natural_h > 0)
    {
        h = (float)st->natural_h;
    }
    else
    {
        h = avail_h;
    }

    n->bounds.x = x;
    n->bounds.y = y;
    n->bounds.w = w;
    n->bounds.h = h;
}

static void image_emit_draws(gui_node_t* n, float scale)
{
    gui_style_t* s = &n->resolved;

    // bg rect renders UNDERNEATH the image (submit_image flushes the
    // solid-rect batch before issuing the textured-quad draw). For
    // backends without an image pipeline this bg is all you see --
    // handy for "loading..." placeholders too.
    if (s->has_background_color)
    {
        renderer__submit_rect(n->bounds, s->background_color, s->radius * scale);
    }

    _widget_image_internal__state_t* st = (_widget_image_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }

    // Tint: treat the node's accent-color as an optional tint
    // multiplier. White (1,1,1,1) is the default "draw as-is" case;
    // set `accent-color: #80ffffff` in .style to dim the image to
    // 50% alpha.
    gui_color_t tint;
    if (s->accent_color.r == 0.0f && s->accent_color.g == 0.0f && s->accent_color.b == 0.0f && s->accent_color.a == 0.0f)
    {
        tint.r = 1.0f; tint.g = 1.0f; tint.b = 1.0f; tint.a = 1.0f;
    }
    else
    {
        tint = s->accent_color;
    }
    tint.a *= n->effective_opacity;

    // Atlased path: pixels live inside a shared GL texture. Sample
    // the sub-rect via UV and submit through the UV-aware helper.
    // Many icons sharing one atlas page collapse into a single
    // batched draw call -- the whole point of the atlas.
    // Legacy path (st->atlas_page < 0): private GL texture, one
    // draw per submit. Used for oversized images (wallpapers, etc.)
    // that can't fit a page.
    if (st->atlas_page >= 0)
    {
        void* atlas_tex = atlas__get_texture(st->atlas_page);
        float u0, v0, u1, v1;
        atlas__rect_to_uv(st->atlas_uv, &u0, &v0, &u1, &v1);
        scene__submit_fitted_image_uv(n->bounds, atlas_tex, st->natural_w, st->natural_h, (int)s->object_fit, tint, s->radius * scale, u0, v0, u1, v1);
    }
    else
    {
        scene__submit_fitted_image(n->bounds, st->tex, st->natural_w, st->natural_h, (int)s->object_fit, tint, s->radius * scale);
    }
}

static void image_on_destroy(gui_node_t* n)
{
    _widget_image_internal__state_t* st = (_widget_image_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return;
    }
    // Private texture: own it, destroy it.
    if (st->tex != NULL_POINTER)
    {
        renderer__destroy_texture(st->tex);
        st->tex = NULL_POINTER;
    }
    // Atlased: the atlas page owns the pixels and is shared with
    // many other widgets. We don't reclaim the sub-rect here in v1
    // -- if an icon is unloaded, its slot leaks until atlas
    // shutdown. A 2048x2048 page holds ~160 icons; users rarely
    // uninstall enough apps for this to matter. Defrag is v2.
    st->atlas_page = -1;
    GUI_FREE(st);
    n->user_data = NULL_POINTER;
}

static const widget_vtable_t g_image_vtable = {
    .type_name        = "image",
    .apply_attribute  = image_apply_attribute,
    .layout           = image_layout,
    .emit_draws       = image_emit_draws,
    .on_destroy       = image_on_destroy,
};

void widget_image__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__IMAGE, &g_image_vtable);
}

// ============================================================================
// Shared path-keyed image cache.
// ============================================================================
// Used by container widgets that want a `background-image: foo.png` --
// lookup by path, decode + upload once, reuse the texture every frame.
// Separate from the per-node state above because (a) keyed by path not
// node so two divs pointing at the same file share one GPU upload,
// (b) node state owns its texture's lifetime via on_destroy; the
// shared cache persists for the process lifetime until explicit
// shutdown.

#define _WIDGET_IMAGE_INTERNAL__CACHE_MAX 64
// Total decoded-bytes cap across the whole cache. RGBA8 means
// 4 bytes/pixel: 256 MiB holds e.g. 16 x 4096x4096 images, or
// many more smaller ones. The cap exists because the per-entry
// max (16384x16384 = 1 GiB single image) times 64 slots is 64 GiB
// theoretical worst case. When a load would push past the cap,
// least-recently-used entries get evicted until there's room.
#define _WIDGET_IMAGE_INTERNAL__CACHE_MAX_BYTES ((int64)256 * 1024 * 1024)

static void _widget_image_internal__cache_evict_lru(void);
static int  _widget_image_internal__cache_acquire_slot(int64 incoming_bytes, char* path_for_log);

typedef struct _widget_image_internal__cache_entry_t
{
    char  path[256];   // 0x0
    void* tex;         // 0x100
    int   w;           // 0x108
    int   h;           // 0x10C
    // w*h*4, tracked so eviction can subtract.
    int64 bytes;       // 0x110
    // last-access counter; smallest = oldest.
    int64 lru_tick;    // 0x118
    boole in_use;      // 0x120
} _widget_image_internal__cache_entry_t;

static _widget_image_internal__cache_entry_t _widget_image_internal__cache[_WIDGET_IMAGE_INTERNAL__CACHE_MAX];
static int64 _widget_image_internal__cache_total_bytes = 0;
static int64 _widget_image_internal__cache_clock = 0;

// Drop the entry with the smallest lru_tick. Helper for the byte-cap
// path; safe to call when nothing is in use (it just no-ops).
static void _widget_image_internal__cache_evict_lru(void)
{
    int   victim     = -1;
    int64 victim_tick = 0;
    for (int i = 0; i < _WIDGET_IMAGE_INTERNAL__CACHE_MAX; i++)
    {
        _widget_image_internal__cache_entry_t* e = &_widget_image_internal__cache[i];
        if (e->in_use == FALSE)
        {
            continue;
        }
        if (victim < 0 || e->lru_tick < victim_tick)
        {
            victim      = i;
            victim_tick = e->lru_tick;
        }
    }
    if (victim < 0)
    {
        return;
    }
    _widget_image_internal__cache_entry_t* e = &_widget_image_internal__cache[victim];
    log_info("%s %s %s %lld %s", "image cache: evicting LRU '", e->path, "' (", (long long)e->bytes, "bytes) to make room");
    if (e->tex != NULL_POINTER)
    {
        renderer__destroy_texture(e->tex);
    }
    _widget_image_internal__cache_total_bytes -= e->bytes;
    if (_widget_image_internal__cache_total_bytes < 0)
    {
        _widget_image_internal__cache_total_bytes = 0;
    }
    e->in_use   = FALSE;
    e->tex      = NULL_POINTER;
    e->bytes    = 0;
    e->path[0]  = 0;
}

// Find a free slot, evicting LRU entries as needed to (a) free a slot
// and (b) bring `total_bytes + incoming_bytes` under the byte cap.
// Returns the slot index or -1 if even a single eviction couldn't make
// room (incoming_bytes alone exceeds the whole cap).
static int _widget_image_internal__cache_acquire_slot(int64 incoming_bytes, char* path_for_log)
{
    if (incoming_bytes > _WIDGET_IMAGE_INTERNAL__CACHE_MAX_BYTES)
    {
        log_warn("%s %s %s %lld %s %lld %s", "image cache: '", path_for_log, "' (", (long long)incoming_bytes, "bytes) alone exceeds", (long long)_WIDGET_IMAGE_INTERNAL__CACHE_MAX_BYTES, "byte cap; rejecting");
        return -1;
    }
    // Evict until the new entry would fit under the byte cap.
    // Sequential LRU eviction is O(n^2) in the worst case but n=64
    // and evictions are rare; not worth a heap.
    while (_widget_image_internal__cache_total_bytes + incoming_bytes > _WIDGET_IMAGE_INTERNAL__CACHE_MAX_BYTES)
    {
        int64 before = _widget_image_internal__cache_total_bytes;
        _widget_image_internal__cache_evict_lru();
        if (_widget_image_internal__cache_total_bytes >= before)
        {
            // No entry was evicted (cache is empty) but we're still
            // over -- can't happen given the alone-exceeds check
            // above, but bail rather than spin.
            return -1;
        }
    }
    // Find a free slot. If none, evict LRU once more to free one.
    for (int i = 0; i < _WIDGET_IMAGE_INTERNAL__CACHE_MAX; i++)
    {
        if (_widget_image_internal__cache[i].in_use == FALSE)
        {
            return i;
        }
    }
    _widget_image_internal__cache_evict_lru();
    for (int i = 0; i < _WIDGET_IMAGE_INTERNAL__CACHE_MAX; i++)
    {
        if (_widget_image_internal__cache[i].in_use == FALSE)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief look up a cached image by path. Loads + decodes + uploads on the first request, returning the cached texture handle thereafter. Bumps the LRU clock on hit so recently-used entries survive the next byte-cap eviction.
 *
 * @param char* path -> asset-relative path. Rejected if absolute or contains "..".
 * @param int* out_w -> receives natural width in pixels (may be NULL).
 * @param int* out_h -> receives natural height in pixels (may be NULL).
 *
 * @return void* -> renderer texture handle, or NULL on any failure (file missing, decode failed, byte cap exceeded, renderer has no image pipeline). NULL is also valid for "no GPU texture but the cache knows the dimensions"; check out_w/out_h.
 */
void* widget_image__cache_get_or_load(char* path, int* out_w, int* out_h)
{
    if (out_w != NULL_POINTER)
    {
        *out_w = 0;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = 0;
    }
    if (path == NULL_POINTER || path[0] == 0)
    {
        return NULL_POINTER;
    }

    // Same path-traversal guard as the per-node load. A
    // background-image: ../../etc/passwd would otherwise read
    // arbitrary files and pin them in the GPU cache.
    if (_widget_image_internal__path_safe(path) == FALSE)
    {
        log_warn("%s %s %s", "image cache: rejecting unsafe path '", path, "'");
        return NULL_POINTER;
    }

    // Linear lookup first. Cache is small (64); a scan is cheaper
    // than a hash table at this size. Bump lru_tick on hit so
    // recently-used entries survive the next byte-cap eviction.
    for (int i = 0; i < _WIDGET_IMAGE_INTERNAL__CACHE_MAX; i++)
    {
        _widget_image_internal__cache_entry_t* e = &_widget_image_internal__cache[i];
        if (e->in_use == FALSE)
        {
            continue;
        }
        if (stdlib__strcmp(e->path, path) == 0)
        {
            e->lru_tick = ++_widget_image_internal__cache_clock;
            if (out_w != NULL_POINTER)
            {
                *out_w = e->w;
            }
            if (out_h != NULL_POINTER)
            {
                *out_h = e->h;
            }
            return e->tex;
        }
    }

    // Two-step path resolution: `<base_dir>/<path>` first (matches
    // widget_image's src attribute), then raw `<path>` so absolute
    // paths still work.
    int64 size = 0;
    char* bytes = style_resources__read(path, &size);
    const char* base = parser_xml__base_dir();
    if (bytes == NULL_POINTER && base != NULL_POINTER && base[0] != 0)
    {
        char joined[1024];
        size_t bl = 0;
        while (base[bl] != 0 && bl + 1 < sizeof(joined))
        {
            joined[bl] = base[bl];
            bl++;
        }
        size_t sl = 0;
        while (path[sl] != 0 && bl + sl + 1 < sizeof(joined))
        {
            joined[bl + sl] = path[sl];
            sl++;
        }
        joined[bl + sl] = 0;
        bytes = fs__read_entire_file(joined, &size);
    }
    if (bytes == NULL_POINTER)
    {
        bytes = fs__read_entire_file(path, &size);
    }
    if (bytes == NULL_POINTER)
    {
        log_error("%s %s %s", "image cache: failed to read '", path, "'");
        return NULL_POINTER;
    }

    if (size > _WIDGET_IMAGE_INTERNAL__MAX_INPUT_BYTES)
    {
        log_warn("%s %s %s %lld %s %d %s", "image cache: '", path, "' is", (long long)size, "bytes, exceeds", _WIDGET_IMAGE_INTERNAL__MAX_INPUT_BYTES, "byte cap; rejecting");
        GUI_FREE(bytes);
        return NULL_POINTER;
    }

    // WebP detection (path extension OR RIFF...WEBP magic) so
    // background-image: foo.webp in .style works -- including
    // wallpaper2.webp. Same dual-check pattern as the per-node
    // load above. WebP path uses tiny_webp; raster path falls
    // through to stb_image (PNG/JPEG).
    boole is_webp_cache = FALSE;
    {
        size_t plen2 = 0; while (path[plen2] != 0) { plen2++; }
        if (plen2 >= 5 && path[plen2-5] == '.' && (path[plen2-4] == 'w' || path[plen2-4] == 'W') && (path[plen2-3] == 'e' || path[plen2-3] == 'E') && (path[plen2-2] == 'b' || path[plen2-2] == 'B') && (path[plen2-1] == 'p' || path[plen2-1] == 'P'))
        {
            is_webp_cache = TRUE;
        }
        else if (size >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
        {
            is_webp_cache = TRUE;
        }
    }

    int w = 0, h = 0;
    stbi_uc* pixels = NULL_POINTER;
    // Track which allocator owns the decoded buffer. WebP path
    // uses the tracked allocator directly (so the cleanup uses
    // GUI_FREE / stbi_image_free uniformly via the same logic
    // below).
    boole pixels_via_stb = FALSE;
    if (is_webp_cache)
    {
        int wp_w = 0, wp_h = 0;
        unsigned char* wp_pixels = twp_read_from_memory(bytes, (int)size, &wp_w, &wp_h, twp_FORMAT_RGBA, 0);
        GUI_FREE(bytes);
        if (wp_pixels == NULL_POINTER)
        {
            log_error("%s %s %s", "image cache: webp decode failed for '", path, "'");
            return NULL_POINTER;
        }
        pixels = wp_pixels;
        w = wp_w;
        h = wp_h;
        pixels_via_stb = FALSE;
    }
    else
    {
        int channels = 0;
        pixels = stbi_load_from_memory((stbi_uc*)bytes, (int)size, &w, &h, &channels, 4);
        GUI_FREE(bytes);
        if (pixels == NULL_POINTER)
        {
            log_error("%s %s %s %s %s", "image cache: decode failed for '", path, "' (", stbi_failure_reason(), ")");
            return NULL_POINTER;
        }
        pixels_via_stb = TRUE;
    }

    if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
    {
        log_warn("%s %s %s %d %s %d %s", "image cache: '", path, "' decoded to unreasonable", w, "x", h, "; rejecting");
        if (pixels_via_stb)
        {
            stbi_image_free(pixels);
        }
        else
        {
            GUI_FREE(pixels);
        }
        return NULL_POINTER;
    }

    // Compute the decoded byte cost (RGBA8 = 4 bytes/pixel) and
    // acquire a slot. The acquire helper evicts LRU entries as
    // needed to bring total_bytes + this image under the cap. If
    // the image is bigger than the entire cap, we reject without
    // uploading to the GPU.
    int64 incoming_bytes = (int64)w * (int64)h * 4;
    int slot = _widget_image_internal__cache_acquire_slot(incoming_bytes, path);
    if (slot < 0)
    {
        if (pixels_via_stb)
        {
            stbi_image_free(pixels);
        }
        else
        {
            GUI_FREE(pixels);
        }
        return NULL_POINTER;
    }

    void* tex = renderer__create_texture_rgba((const ubyte*)pixels, w, h);
    if (pixels_via_stb)
    {
        stbi_image_free(pixels);
    }
    else
    {
        GUI_FREE(pixels);
    }
    // tex may be NULL on backends without an image pipeline (d3d11 /
    // d3d9 stubs). We still cache the dimensions so the fitted-draw
    // helper can run its math; the placeholder fallback in
    // scene__submit_fitted_image handles NULL-tex visually.

    _widget_image_internal__cache_entry_t* e = &_widget_image_internal__cache[slot];
    size_t plen = 0;
    while (path[plen] != 0 && plen + 1 < sizeof(e->path))
    {
        e->path[plen] = path[plen];
        plen++;
    }
    e->path[plen] = 0;
    e->tex       = tex;
    e->w         = w;
    e->h         = h;
    e->bytes     = incoming_bytes;
    e->lru_tick  = ++_widget_image_internal__cache_clock;
    e->in_use    = TRUE;
    _widget_image_internal__cache_total_bytes += incoming_bytes;

    if (out_w != NULL_POINTER)
    {
        *out_w = w;
    }
    if (out_h != NULL_POINTER)
    {
        *out_h = h;
    }
    log_info("%s %s %s %d %s %d %s %lld %s %lld %s %lld %s", "image cache: loaded '", path, "' (", w, "x", h, ",", (long long)incoming_bytes, "bytes; total", (long long)_widget_image_internal__cache_total_bytes, "/", (long long)_WIDGET_IMAGE_INTERNAL__CACHE_MAX_BYTES, ")");
    return tex;
}

/**
 * @brief release every cached entry: destroys the GPU texture, frees the tracked-bytes accounting, clears the LRU clock. Safe to call multiple times. Must be called BEFORE renderer__shutdown so the texture-destroy callbacks reach a live backend.
 *
 * @return void
 */
void widget_image__cache_shutdown(void)
{
    for (int i = 0; i < _WIDGET_IMAGE_INTERNAL__CACHE_MAX; i++)
    {
        _widget_image_internal__cache_entry_t* e = &_widget_image_internal__cache[i];
        if (e->in_use == FALSE)
        {
            continue;
        }
        if (e->tex != NULL_POINTER)
        {
            renderer__destroy_texture(e->tex);
        }
        e->in_use   = FALSE;
        e->tex      = NULL_POINTER;
        e->bytes    = 0;
        e->lru_tick = 0;
        e->path[0]  = 0;
    }
    _widget_image_internal__cache_total_bytes = 0;
    _widget_image_internal__cache_clock       = 0;
}

