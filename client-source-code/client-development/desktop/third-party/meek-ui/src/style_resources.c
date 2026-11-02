// Inline stylesheet assets use small stable keys in ordinary style records.
// Encoded payloads never enter fixed-size property buffers or touch the disk.
#include "style_resources.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "clib/stdlib.h"
#include "font.h"
#include "third_party/log.h"

#include <stdio.h>
#include <string.h>

typedef struct _style_resources_internal__asset_t
{
    struct _style_resources_internal__asset_t* next;
    char key[32];
    int64 size;
    ubyte* bytes;
} _style_resources_internal__asset_t;

static _style_resources_internal__asset_t* _style_resources_internal__assets;
static uint64 _style_resources_internal__serial;
static int64 _style_resources_internal__bytes;
static int _style_resources_internal__count;

/** @brief skip spaces without stepping outside a bounded source span. */
static const char* _style_resources_internal__space(const char* p, const char* end)
{
    while (p < end && stdlib__isspace((unsigned char)*p))
    {
        p++;
    }
    return p;
}

/** @brief find a delimiter outside quoted text, comments and parentheses. */
static const char* _style_resources_internal__end(const char* p, const char* end, char delimiter)
{
    char quote = 0;
    int depth = 0;
    while (p < end)
    {
        if (quote)
        {
            if (*p == '\\' && p + 1 < end)
            {
                p += 2;
                continue;
            }
            if (*p == quote)
            {
                quote = 0;
            }
        }
        else if (*p == '"' || *p == '\'')
        {
            quote = *p;
        }
        else if (*p == '/' && p + 1 < end && p[1] == '*')
        {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
            {
                p++;
            }
            p = p + 1 < end ? p + 2 : end;
            continue;
        }
        else if (!depth && *p == delimiter)
        {
            return p;
        }
        else if (*p == '(')
        {
            depth++;
        }
        else if (*p == ')' && depth)
        {
            depth--;
        }
        p++;
    }
    return end;
}

/** @brief decode one base64 digit; padding and invalid bytes are separate. */
static int _style_resources_internal__digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if (c == '+')
    {
        return 62;
    }
    if (c == '/')
    {
        return 63;
    }
    return -1;
}

/** @brief validate and retain a bounded base64 data URL, without file I/O. */
static _style_resources_internal__asset_t* _style_resources_internal__decode(const char* p, const char* end)
{
    const const char* comma = stdlib__memchr(p, ',', (size_t)(end - p));
    const char* q;
    _style_resources_internal__asset_t* asset;
    ubyte* bytes;
    int64 count = 0, used = 0;
    int quartet[4], n = 0;
    boole padded = FALSE;
    if (!comma || comma - p < 12 || stdlib__memcmp(comma - 7, ";base64", 7))
    {
        return NULL_POINTER;
    }
    for (q = comma + 1; q < end; q++)
    {
        if (!stdlib__isspace((unsigned char)*q))
        {
            count++;
        }
    }
    if (!count || count % 4 || count / 4 > (32 * 1024 * 1024) / 3 ||
        _style_resources_internal__bytes + count / 4 * 3 > 128 * 1024 * 1024)
    {
        return NULL_POINTER;
    }
    bytes = GUI_MALLOC_T((size_t)(count / 4 * 3), MM_TYPE__IMAGE);
    if (!bytes)
    {
        return NULL_POINTER;
    }
    for (q = comma + 1; q < end; q++)
    {
        if (stdlib__isspace((unsigned char)*q))
        {
            continue;
        }
        if (padded)
        {
            goto invalid;
        }
        quartet[n] = *q == '=' ? -2 : _style_resources_internal__digit((unsigned char)*q);
        if (quartet[n++] == -1)
        {
            goto invalid;
        }
        if (n == 4)
        {
            if (quartet[0] < 0 || quartet[1] < 0 || (quartet[2] == -2 && quartet[3] != -2))
            {
                goto invalid;
            }
            bytes[used++] = (ubyte)((quartet[0] << 2) | (quartet[1] >> 4));
            if (quartet[2] >= 0)
            {
                bytes[used++] = (ubyte)((quartet[1] << 4) | (quartet[2] >> 2));
                if (quartet[3] >= 0)
                {
                    bytes[used++] = (ubyte)((quartet[2] << 6) | quartet[3]);
                }
                else if (quartet[2] & 3)
                {
                    goto invalid;
                }
            }
            else if (quartet[1] & 15)
            {
                goto invalid;
            }
            padded = quartet[3] == -2;
            n = 0;
        }
    }
    for (asset = _style_resources_internal__assets; asset; asset = asset->next)
    {
        if (asset->size == used && !stdlib__memcmp(asset->bytes, bytes, (size_t)used))
        {
            GUI_FREE(bytes);
            return asset;
        }
    }
    if (_style_resources_internal__count >= 1024)
    {
        goto invalid;
    }
    asset = GUI_CALLOC(1, sizeof(*asset));
    if (!asset)
    {
        goto invalid;
    }
    // the key is "@m" plus the serial in hex, built without stdio
    stdlib__strcpy_safe(asset->key, (int64)sizeof(asset->key), "@m");
    stdlib__u64toa_hex(asset->key + 2, (int64)sizeof(asset->key) - 2, (uint64)++_style_resources_internal__serial);
    asset->bytes = bytes;
    asset->size = used;
    asset->next = _style_resources_internal__assets;
    _style_resources_internal__assets = asset;
    _style_resources_internal__bytes += used;
    _style_resources_internal__count++;
    return asset;
invalid:
    GUI_FREE(bytes);
    return NULL_POINTER;
}

/** @brief copy an inline resource into the caller's normal tracked read buffer. */
char* style_resources__read(const char* key, int64* size)
{
    _style_resources_internal__asset_t* asset;
    char* copy;
    if (!key || !size)
    {
        return NULL_POINTER;
    }
    for (asset = _style_resources_internal__assets; asset; asset = asset->next)
    {
        if (stdlib__strcmp(asset->key, key))
        {
            continue;
        }
        copy = GUI_MALLOC_T((size_t)asset->size, MM_TYPE__IMAGE);
        if (copy)
        {
            stdlib__memcpy(copy, asset->bytes, (size_t)asset->size);
            *size = asset->size;
        }
        return copy;
    }
    return NULL_POINTER;
}

/** @brief release stylesheet-owned encoded resources; decoded textures own copies. */
void style_resources__clear(void)
{
    _style_resources_internal__asset_t* asset;
    while (_style_resources_internal__assets)
    {
        asset = _style_resources_internal__assets;
        _style_resources_internal__assets = asset->next;
        GUI_FREE(asset->bytes);
        GUI_FREE(asset);
    }
    _style_resources_internal__bytes = 0;
    _style_resources_internal__count = 0;
    // Keep the serial: cached images from the previous sheet cannot alias new data.
}

/** @brief copy a quoted or bare CSS value with surrounding spaces removed. */
static boole _style_resources_internal__value(char* out, size_t capacity, const char* p, const char* end)
{
    p = _style_resources_internal__space(p, end);
    while (end > p && stdlib__isspace((unsigned char)end[-1]))
    {
        end--;
    }
    if (end - p >= 2 && (*p == '"' || *p == '\'') && end[-1] == *p)
    {
        p++;
        end--;
    }
    if (end <= p || (size_t)(end - p) >= capacity)
    {
        return FALSE;
    }
    stdlib__memcpy(out, p, (size_t)(end - p));
    out[end - p] = 0;
    return TRUE;
}

/** @brief register a CSS font family from its first URL source. */
static boole _style_resources_internal__font(const char* p, const char* end, const char* label)
{
    char family[64] = {0}, source[1024] = {0}, path[2048];
    const char* colon;
    const char* stop;
    const char* value;
    const char* close;
    const char* slash;
    int64 size = 0;
    int64 prefix_length = 0;
    char* bytes;
    boole result;
    while (p < end)
    {
        p = _style_resources_internal__space(p, end);
        if (p + 1 < end && p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
            {
                p++;
            }
            p = p + 1 < end ? p + 2 : end;
            continue;
        }
        colon = _style_resources_internal__end(p, end, ':');
        if (colon == end)
        {
            break;
        }
        stop = _style_resources_internal__end(colon + 1, end, ';');
        value = _style_resources_internal__space(colon + 1, stop);
        if (colon - p >= 11 && !stdlib__memcmp(p, "font-family", 11))
        {
            if (!_style_resources_internal__value(family, sizeof(family), value, stop))
            {
                return FALSE;
            }
        }
        else if (colon - p >= 3 && !stdlib__memcmp(p, "src", 3))
        {
            if (stop - value < 4 || stdlib__memcmp(value, "url", 3))
            {
                return FALSE;
            }
            value = _style_resources_internal__space(value + 3, stop);
            if (value == stop || *value != '(')
            {
                return FALSE;
            }
            close = _style_resources_internal__end(value + 1, stop, ')');
            if (close == stop || !_style_resources_internal__value(source, sizeof(source), value + 1, close))
            {
                return FALSE;
            }
        }
        p = stop < end ? stop + 1 : end;
    }
    if (!family[0] || !source[0])
    {
        return FALSE;
    }
    bytes = style_resources__read(source, &size);
    if (bytes)
    {
        result = font__register_from_memory_copy(family, (const ubyte*)bytes, size);
        GUI_FREE(bytes);
        return result;
    }
    slash = label ? stdlib__strrchr(label, '/') : NULL_POINTER;
    if (label && stdlib__strrchr(label, '\\') && (!slash || stdlib__strrchr(label, '\\') > slash))
    {
        slash = stdlib__strrchr(label, '\\');
    }
    if (slash && source[0] != '/' && source[0] != '\\' && !stdlib__strchr(source, ':'))
    {
        // the label's directory part, then the source name after it
        prefix_length = (int64)(slash + 1 - label);
        if (prefix_length < 0 || prefix_length >= (int64)sizeof(path))
        {
            return FALSE;
        }
        stdlib__memcpy(path, label, prefix_length);
        path[prefix_length] = 0;
        // strcat_safe reports the length it would need, like strlcat, so a
        // result at or past the capacity means the path was truncated
        if (stdlib__strcat_safe(path, (int64)sizeof(path), source) >= (int64)sizeof(path))
        {
            return FALSE;
        }
        return font__register_from_file(family, path);
    }
    return font__register_from_file(family, source);
}

/** @brief extract inline URLs and font faces before fixed-size style parsing. */
boole style_resources__prepare(const char* source, int64 length, const char* label, char** output, int64* output_length)
{
    char* text;
    char* p;
    char* end;
    char* value;
    char* close;
    char* finish;
    char quote;
    _style_resources_internal__asset_t* asset;
    size_t key_length, removed;
    int64 i;
    boole needed = FALSE;
    *output = NULL_POINTER;
    *output_length = length;
    if (!source || length < 1 || length > 256 * 1024 * 1024)
    {
        return FALSE;
    }
    // Most ordinary style sheets need no preprocessing allocation.
    for (i = 0; i < length; i++)
    {
        if ((length - i >= 10 && source[i] == '@' && !stdlib__memcmp(source + i, "@font-face", 10)) ||
            (length - i >= 5 && source[i] == 'd' && !stdlib__memcmp(source + i, "data:", 5)))
        {
            needed = TRUE;
            break;
        }
    }
    if (!needed)
    {
        return TRUE;
    }
    text = GUI_MALLOC_T((size_t)length + 1, MM_TYPE__TEXT);
    if (!text)
    {
        return FALSE;
    }
    stdlib__memcpy(text, source, (size_t)length);
    text[length] = 0;
    end = text + length;
    for (p = text; p < end; p++)
    {
        if (*p == '/' && p + 1 < end && p[1] == '*')
        {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
            {
                p++;
            }
            if (p + 1 < end)
            {
                p++;
            }
            else
            {
                break;
            }
            continue;
        }
        if (*p == '"' || *p == '\'')
        {
            quote = *p++;
            while (p < end && *p != quote)
            {
                if (*p == '\\' && p + 1 < end)
                {
                    p++;
                }
                p++;
            }
            if (p == end)
            {
                break;
            }
            continue;
        }
        if (end - p < 4 || stdlib__memcmp(p, "url", 3) || (p > text && (stdlib__isalnum((unsigned char)p[-1]) || p[-1] == '-')))
        {
            continue;
        }
        value = (char*)_style_resources_internal__space(p + 3, end);
        if (value == end || *value != '(')
        {
            continue;
        }
        value = (char*)_style_resources_internal__space(value + 1, end);
        quote = value < end && (*value == '\'' || *value == '"') ? *value++ : 0;
        close = value;
        while (close < end && (quote ? *close != quote : *close != ')'))
        {
            close++;
        }
        if (close == end)
        {
            goto invalid;
        }
        finish = quote ? (char*)_style_resources_internal__space(close + 1, end) : close;
        if (finish == end || *finish != ')')
        {
            goto invalid;
        }
        if (close - value >= 5 && !stdlib__memcmp(value, "data:", 5))
        {
            // Non-base64 data URLs retain their existing behavior.
            const char* comma = stdlib__memchr(value, ',', (size_t)(close - value));
            if (!comma || comma - value < 7 || stdlib__memcmp(comma - 7, ";base64", 7))
            {
                p = finish;
                continue;
            }
            asset = _style_resources_internal__decode(value, close);
            if (!asset)
            {
                goto invalid;
            }
            key_length = stdlib__strlen(asset->key);
            if (key_length > (size_t)(close - value))
            {
                goto invalid;
            }
            removed = (size_t)(close - value) - key_length;
            stdlib__memcpy(value, asset->key, key_length);
            stdlib__memmove(value + key_length, close, (size_t)(end - close) + 1);
            end -= removed;
            finish -= removed;
        }
        p = finish;
    }
    for (p = text; p < end; p++)
    {
        if (*p == '/' && p + 1 < end && p[1] == '*')
        {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
            {
                p++;
            }
            if (p + 1 < end)
            {
                p++;
            }
            else
            {
                break;
            }
            continue;
        }
        if (*p == '"' || *p == '\'')
        {
            quote = *p++;
            while (p < end && *p != quote)
            {
                if (*p == '\\' && p + 1 < end)
                {
                    p++;
                }
                p++;
            }
            if (p == end)
            {
                break;
            }
            continue;
        }
        if (end - p < 10 || stdlib__memcmp(p, "@font-face", 10))
        {
            continue;
        }
        value = (char*)_style_resources_internal__space(p + 10, end);
        if (value == end || *value != '{')
        {
            goto invalid;
        }
        close = (char*)_style_resources_internal__end(value + 1, end, '}');
        if (close == end || !_style_resources_internal__font(value + 1, close, label))
        {
            goto invalid;
        }
        while (p <= close)
        {
            if (*p != '\n' && *p != '\r')
            {
                *p = ' ';
            }
            p++;
        }
        p--;
    }
    *output = text;
    *output_length = end - text;
    return TRUE;
invalid:
    log_error("%s: invalid inline stylesheet resource or font face", label ? label : "(memory)");
    GUI_FREE(text);
    return FALSE;
}
