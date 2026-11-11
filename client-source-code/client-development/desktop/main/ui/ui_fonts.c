#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#include "ui/ui_fonts.h"
#include "ui/ui_headers.h"
#include "attachments/attachment_file.h"
#include "platform/system.h"
#include "fs.h"
#include <stdlib.h>
#include <stdint.h>

#define DESKTOP_FONT_LIMIT 56
#define DESKTOP_FONT_BYTES (64ULL * 1024 * 1024)

typedef struct desktop_font_file
{
    char name[64];
    char file[68];
    char token[224];
    uint64 size;
    uint64 modified;
    boole seen;
    boole loaded;
} desktop_font_file_t;

static desktop_font_file_t g_fonts[DESKTOP_FONT_LIMIT];
static int64 g_font_count;
static uint64 g_font_scan_time;
static boole g_fonts_changed;

static int _ui_fonts_internal__compare_names(const char *first, const char *second)
{
    // compare ignoring letter case
    while (*first && *second && clib__tolower((ubyte)*first) == clib__tolower((ubyte)*second))
    {
        first++;
        second++;
    }
    return clib__tolower((ubyte)*first) - clib__tolower((ubyte)*second);
}

// Browser class names for the bundled faces. Extra local fonts use an encoded
// filename token, so punctuation cannot become CSS or a path in a remote client.
static const char *g_browser_fonts[] = { "Yellowtail", "Bruno Ace SC", "Raleway", "Kavoon", "Codystar", "Hanalei", "Just Me Again Down Here", "Londrina Outline", "Mystery Quest", "Nosifer", "NovaMono", "Reenie Beanie", "Sacramento", "Sofia", "Simonetta", "Inter", "Manrope", "Plus Jakarta Sans" };

static uint64 _ui_fonts_internal__modified(const char *path)
{
#ifdef _WIN32
    wchar_t wide[1024];
    WIN32_FILE_ATTRIBUTE_DATA attributes;

    // read the file time
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024) || !GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes))
    {
        return 0;
    }
    return ((uint64)attributes.ftLastWriteTime.dwHighDateTime << 32) | attributes.ftLastWriteTime.dwLowDateTime;
#else
    struct stat attributes;

    // read the file time
    if (stat(path, &attributes) != 0)
    {
        return 0;
    }
#ifdef __APPLE__
    return (uint64)attributes.st_mtimespec.tv_sec * 1000000000 + attributes.st_mtimespec.tv_nsec;
#else
    return (uint64)attributes.st_mtim.tv_sec * 1000000000 + attributes.st_mtim.tv_nsec;
#endif
#endif
}

static void _ui_fonts_internal__token(desktop_font_file_t *font)
{
    static const char hex[] = "0123456789abcdef";
    int64 index = 0;
    int64 offset = 0;
    boole bundled = FALSE;
    const char *name = font->name;

    // check for bundled font
    for (index = 0; index < (int64)(sizeof(g_browser_fonts) / sizeof(g_browser_fonts[0])); index++)
    {
        bundled |= clib__strcmp(name, g_browser_fonts[index]) == 0;
    }

    // write the token prefix
    clib__string_copy(font->token, sizeof(font->token), bundled ? "custom-font-usage-" : "custom-font-usage-local-");

    // replace the NovaMono name
    if (clib__strcmp(name, "NovaMono") == 0)
    {
        name = "Nova-Mono";
    }

    // append the encoded name
    offset = clib__strlen(font->token);
    for (index = 0; name[index]; index++)
    {
        ubyte byte = (ubyte)name[index];
        if (bundled)
        {
            font->token[offset++] = byte == ' ' ? '-' : (char)byte;
        }
        else
        {
            font->token[offset++] = hex[byte >> 4];
            font->token[offset++] = hex[byte & 15];
        }
    }
    font->token[offset] = 0;
}

static uint _ui_fonts_internal__be32(const ubyte *bytes)
{
    return ((uint)bytes[0] << 24) | ((uint)bytes[1] << 16) | ((uint)bytes[2] << 8) | bytes[3];
}

// Reject truncated or unrelated files before the bounds-aware FreeType loader checks their contents.
static boole _ui_fonts_internal__font(const ubyte *bytes, uint64 size)
{
    uint64 tables = 0;
    uint64 index = 0;

    // check the font signature
    if (!bytes || size < 12 || !(_ui_fonts_internal__be32(bytes) == 0x00010000 || _ui_fonts_internal__be32(bytes) == 0x4f54544f || _ui_fonts_internal__be32(bytes) == 0x74727565))
    {
        return FALSE;
    }

    // read the table count
    tables = ((uint64)bytes[4] << 8) | bytes[5];
    if (!tables || tables > 256 || 12 + tables * 16 > size)
    {
        return FALSE;
    }

    // check every table range
    for (index = 0; index < tables; index++)
    {
        uint64 offset = _ui_fonts_internal__be32(bytes + 12 + index * 16 + 8);
        uint64 length = _ui_fonts_internal__be32(bytes + 12 + index * 16 + 12);
        if (offset > size || length > size - offset)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static void _ui_fonts_internal__entry(const char *name, boole directory, int64 size, void *context)
{
    char path[128];
    char family[64];
    ubyte *bytes = NULL_POINTER;
    uint64 read_size = 0;
    uint64 modified = 0;
    int64 length = clib__strlen(name);
    int64 index = 0;
    desktop_font_file_t *font = NULL_POINTER;
    (void)context;

    // check name and size
    if (directory || length < 5 || length - 4 >= (int64)sizeof(family) || size <= 0 || (uint64)size > DESKTOP_FONT_BYTES || (_ui_fonts_internal__compare_names(name + length - 4, ".ttf") != 0 && _ui_fonts_internal__compare_names(name + length - 4, ".otf") != 0))
    {
        return;
    }

    // copy the family name
    clib__memcpy(family, name, length - 4);
    family[length - 4] = 0;
    // keep the two platform UI fallback names under the application's control
    if (clib__strcmp(family, "Desktop") == 0 || clib__strcmp(family, "Desktop-Bold") == 0)
    {
        return;
    }

    // find the known font
    for (index = 0; index < g_font_count; index++)
    {
        if (clib__strcmp(g_fonts[index].name, family) == 0)
        {
            font = &g_fonts[index];
            break;
        }
    }

    // add a new font
    if (!font)
    {
        if (g_font_count == DESKTOP_FONT_LIMIT)
        {
            return;
        }
        font = &g_fonts[g_font_count++];
        clib__null_memory(font, sizeof(*font));
        clib__string_copy(font->name, sizeof(font->name), family);
        clib__string_copy(font->file, sizeof(font->file), name);
        _ui_fonts_internal__token(font);
    }

    // use each font once
    if (font->seen)
    {
        return;
    }
    font->seen = TRUE;

    // skip an unchanged file
    clib__string_format(path, sizeof(path), "fonts/%s", name);
    modified = _ui_fonts_internal__modified(path);
    if (font->loaded && font->size == (uint64)size && font->modified == modified && clib__strcmp(font->file, name) == 0)
    {
        return;
    }

    // load the font file
    bytes = desktop_attachment_file__read(path, DESKTOP_FONT_BYTES, &read_size);
    if (_ui_fonts_internal__font(bytes, read_size) && font__register_from_memory_copy(font->name, bytes, (int64)read_size))
    {
        font->loaded = TRUE;
        font->size = read_size;
        font->modified = modified;
        clib__string_copy(font->file, sizeof(font->file), name);
        g_fonts_changed = TRUE;
    }

    // free the file bytes
    free(bytes);
}

static int _ui_fonts_internal__compare(const void *first, const void *second)
{
    return _ui_fonts_internal__compare_names(((const desktop_font_file_t *)first)->name, ((const desktop_font_file_t *)second)->name);
}

/** @brief Discovers changed local fonts between frames; font files stay unlocked after reading. */
boole desktop_ui_fonts__scan(boole force)
{
    uint64 now = desktop_system__milliseconds();
    int64 index = 0;

    // skip a recent scan
    if (!force && now - g_font_scan_time < 2000)
    {
        return FALSE;
    }

    // reset the scan state
    g_font_scan_time = now;
    g_fonts_changed = FALSE;
    for (index = 0; index < g_font_count; index++)
    {
        g_fonts[index].seen = FALSE;
    }

    // read the fonts folder
    fs__list_dir("fonts", _ui_fonts_internal__entry, NULL);

    // remove missing fonts
    for (index = g_font_count - 1; index >= 0; index--)
    {
        if (!g_fonts[index].seen || !g_fonts[index].loaded)
        {
            g_fonts_changed |= g_fonts[index].loaded;
            font__unregister(g_fonts[index].name);
            clib__memmove(g_fonts + index, g_fonts + index + 1, (g_font_count - index - 1) * sizeof(g_fonts[0]));
            g_font_count--;
        }
    }

    // sort the fonts
    qsort(g_fonts, (size_t)g_font_count, sizeof(g_fonts[0]), _ui_fonts_internal__compare);
    return g_fonts_changed;
}

int64 desktop_ui_fonts__count(void)
{
    return g_font_count;
}

const char *desktop_ui_fonts__name(int64 index)
{
    return index >= 0 && index < g_font_count ? g_fonts[index].name : "";
}

const char *desktop_ui_fonts__token(int64 index)
{
    return index >= 0 && index < g_font_count ? g_fonts[index].token : "custom-font-usage-default";
}

const char *desktop_ui_fonts__family(const char *token)
{
    int64 index = 0;

    // find font by token
    for (index = 0; index < g_font_count; index++)
    {
        if (clib__strcmp(g_fonts[index].token, token) == 0)
        {
            return g_fonts[index].name;
        }
    }
    return "";
}
