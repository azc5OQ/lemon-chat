#include "attachments/attachments_internal.h"
#include <stdlib.h>
#include "mbedtls/platform_util.h"

// Private instances of the decoders already vendored with Meek UI.
#define nuint meek_ui_nuint
#define nint meek_ui_nint
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"
#define twp_STATIC
#define twp_NO_STDIO
#define twp_IMPLEMENTATION
#include "third_party/tiny_webp.h"
#undef nint
#undef nuint

/**
 * @brief identifies supported image bytes without trusting the filename or supplied MIME type.
 * @param const ubyte* bytes -> borrowed file bytes
 * @param uint64 size -> available byte count
 * @return const char* -> static MIME string, or NULL
 */
const char *desktop_attachment_image__mime(const ubyte *bytes, uint64 size)
{
    if (!bytes || size < 12)
    {
        return NULL;
    }
    if (clib__memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0)
    {
        return "image/png";
    }
    if (bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
    {
        return "image/jpeg";
    }
    if (clib__memcmp(bytes, "GIF87a", 6) == 0 || clib__memcmp(bytes, "GIF89a", 6) == 0)
    {
        return "image/gif";
    }
    if (bytes[0] == 'B' && bytes[1] == 'M')
    {
        return "image/bmp";
    }
    if (clib__memcmp(bytes, "RIFF", 4) == 0 && clib__memcmp(bytes + 8, "WEBP", 4) == 0)
    {
        return "image/webp";
    }
    return NULL;
}

/**
 * @brief checks dimensions before decoding and retains an aspect-correct preview of at most 1024 pixels per side.
 * @param const ubyte* bytes -> bounded original image bytes
 * @param uint64 size -> byte count, at most the native picture limit
 * @param int64* width -> preview width on success
 * @param int64* height -> preview height on success
 * @return ubyte* -> owned RGBA preview, or NULL; release with free
 */
ubyte *desktop_attachment_image__decode(const ubyte *bytes, uint64 size, int64 *width, int64 *height)
{
    return desktop_attachment_image__decode_bounded(bytes, size, width, height, 8192, 1024);
}

/**
 * @brief decodes a picture with caller-supplied dimension and preview bounds, before allocating pixels.
 * @param const ubyte* bytes -> borrowed encoded bytes
 * @param uint64 size -> available bytes
 * @param int64* width -> output preview width
 * @param int64* height -> output preview height
 * @param int64 maximum -> maximum input dimension, at most 8192
 * @param int64 preview_limit -> maximum retained dimension, at most 1024
 * @return ubyte* -> owned RGBA pixels, cleared by the caller before free
 */
ubyte *desktop_attachment_image__decode_bounded(const ubyte *bytes, uint64 size, int64 *width, int64 *height, int64 maximum, int64 preview_limit)
{
    const char *mime = desktop_attachment_image__mime(bytes, size);
    int w = 0;
    int h = 0;
    int components = 0;
    int lossless = 0;
    int alpha = 0;
    int decoded_w = 0;
    int decoded_h = 0;
    boole webp = mime && clib__strcmp(mime, "image/webp") == 0;
    ubyte *decoded = NULL;
    ubyte *preview = NULL;
    int64 target_w = 0;
    int64 target_h = 0;
    int64 x = 0;
    int64 y = 0;
    int64 source_x = 0;
    int64 source_y = 0;
    if (!width || !height)
    {
        return NULL;
    }
    *width = 0;
    *height = 0;
    if (!mime || size > DESKTOP_ATTACHMENT_IMAGE_LIMIT || maximum <= 0 || maximum > 8192 || preview_limit <= 0 || preview_limit > 1024)
    {
        return NULL;
    }
    if (webp ? !twp_get_info_from_memory((void *)bytes, (int)size, &w, &h, &lossless, &alpha) : !stbi_info_from_memory(bytes, (int)size, &w, &h, &components))
    {
        return NULL;
    }
    if (w <= 0 || h <= 0 || w > maximum || h > maximum || (uint64)w * (uint64)h > 16ULL * 1024 * 1024)
    {
        return NULL;
    }
    decoded = webp ? twp_read_from_memory((void *)bytes, (int)size, &decoded_w, &decoded_h, twp_FORMAT_RGBA, 0) : stbi_load_from_memory(bytes, (int)size, &decoded_w, &decoded_h, &components, 4);
    if (!decoded || decoded_w != w || decoded_h != h)
    {
        if (decoded && decoded_w > 0 && decoded_h > 0)
        {
            mbedtls_platform_zeroize(decoded, (uint64)decoded_w * (uint64)decoded_h * 4);
        }
        free(decoded);
        return NULL;
    }
    target_w = w;
    target_h = h;
    if (target_w > preview_limit || target_h > preview_limit)
    {
        if (w >= h)
        {
            target_w = preview_limit;
            target_h = (int64)h * preview_limit / w;
        }
        else
        {
            target_h = preview_limit;
            target_w = (int64)w * preview_limit / h;
        }
    }
    target_w = target_w > 0 ? target_w : 1;
    target_h = target_h > 0 ? target_h : 1;
    preview = calloc(1, (uint64)target_w * (uint64)target_h * 4);
    if (preview)
    {
        for (y = 0; y < target_h; y++)
        {
            source_y = y * h / target_h;
            for (x = 0; x < target_w; x++)
            {
                source_x = x * w / target_w;
                clib__memcpy(preview + (y * target_w + x) * 4, decoded + (source_y * w + source_x) * 4, 4);
            }
        }
        *width = target_w;
        *height = target_h;
    }
    mbedtls_platform_zeroize(decoded, (uint64)w * (uint64)h * 4);
    free(decoded);
    return preview;
}
