#include "ui/ui_theme.h"
#include "ui/ui_fonts.h"
#include "ui/ui_layout.h"
#include "ui/ui_headers.h"
#include "attachments/attachment_file.h"
#include "platform/memory.h"
#include "platform/system.h"
#include "settings/settings.h"
#include <stdlib.h>

#define DESKTOP_THEME_BYTES 65536

static char g_theme_bytes[DESKTOP_THEME_BYTES];
static int64 g_theme_length;
static int64 g_theme_scale = 100;
static const struct
{
    const char *selector;
    float font;
    float width;
    float height;
    float padding;
    float gap;
} g_theme_scale_rules[] = { { "#conversation .chat-text", 13, 0, 0, 0, 0 }, { "#conversation .chat-sender", 12, 0, 0, 0, 0 }, { "#conversation button", 12, 0, 0, 0, 0 }, { "#conversation input", 13, 0, 0, 0, 0 }, { "#view-caption", 11, 0, 22, 3, 0 }, { ".chat-message", 0, 0, 0, 0, 3 }, { "#chat-history", 0, 0, 0, 0, 8 }, { "#composer", 0, 0, 80, 4, 4 }, { "#composer-actions", 0, 0, 24, 0, 0 }, { "#message-input", 0, 0, 32, 4, 0 }, { "#send-button", 0, 64, 24, 2, 0 }, { "#picture-button", 0, 64, 24, 2, 0 }, { "#file-button", 0, 64, 24, 2, 0 }, { ".conversation-tab", 0, 0, 24, 0, 0 }, { "#conversation-tabs", 0, 0, 24, 0, 0 }, { ".tab-label", 0, 0, 18, 1, 0 }, { ".tab-close", 10, 0, 18, 0, 0 }, { ".tab-scroll", 0, 0, 24, 0, 0 }, { "#chat-tabs", 0, 0, 30, 0, 0 }, { ".attachment-actions button", 0, 0, 24, 2, 0 } };
static uint64 g_theme_scale_handles[sizeof(g_theme_scale_rules) / sizeof(g_theme_scale_rules[0])];

// declarations
static boole _ui_theme_internal__load(const char *name, const desktop_settings_t *settings);
static void _ui_theme_internal__clear_node_styles(gui_node_t *node);

/**
 * @brief updates the existing conversation rules in place, recreating handles only after a stylesheet reset.
 * @param int64 percent -> bounded chat scale from 75 to 150 percent
 * @return boole -> TRUE when every rule is installed; failed registrations are reported to the caller
 */
boole desktop_ui_theme__scale(int64 percent)
{
    gui_style_t style = { 0 };
    int64 index = 0;
    float scale = (float)percent / 100;
    if (percent < 75 || percent > 150)
    {
        return FALSE;
    }
    for (index = 0; index < (int64)(sizeof(g_theme_scale_rules) / sizeof(g_theme_scale_rules[0])); index++)
    {
        clib__null_memory(&style, sizeof(style));
        style.font_size = g_theme_scale_rules[index].font * scale;
        style.font_size_explicit = style.font_size > 0;
        style.size_w = g_theme_scale_rules[index].width * scale;
        style.size_w_explicit = style.size_w > 0;
        style.size_h = g_theme_scale_rules[index].height * scale;
        style.size_h_explicit = style.size_h > 0;
        style.pad_t = g_theme_scale_rules[index].padding * scale;
        style.pad_b = style.pad_t;
        style.pad_l = style.pad_t;
        style.pad_r = style.pad_t;
        style.gap = g_theme_scale_rules[index].gap * scale;
        if (!scene__update_style(g_theme_scale_handles[index], &style))
        {
            g_theme_scale_handles[index] = scene__add_style((char *)g_theme_scale_rules[index].selector, &style, FALSE);
            if (!g_theme_scale_handles[index])
            {
                clib__null_memory(&style, sizeof(style));
                return FALSE;
            }
        }
    }
    clib__null_memory(&style, sizeof(style));
    g_theme_scale = percent;
    return TRUE;
}

/**
 * @brief removes the old theme's cached colours, gradients and geometry while retaining widget contents and state.
 * @param gui_node_t* node -> scene-owned node; this application does not store runtime overrides in node->style
 * @return void
 */
static void _ui_theme_internal__clear_node_styles(gui_node_t *node)
{
    gui_node_t *child = NULL_POINTER;
    if (!node)
    {
        return;
    }
    clib__null_memory(node->style, sizeof(node->style));
    for (child = node->first_child; child; child = child->next_sibling)
    {
        _ui_theme_internal__clear_node_styles(child);
    }
}

/**
 * @brief registers native UI font aliases, falling back to bundled fonts on unavailable systems.
 * @return void
 */
void desktop_ui_theme__fonts(void)
{
    char path[1024] = { 0 };
    if (!desktop_system__font_path(path, sizeof(path), FALSE) || !font__register_from_file("Desktop", path))
    {
        font__register_from_file("Desktop", "fonts/SpaceGrotesk-Regular.ttf");
    }
    clib__null_memory(path, sizeof(path));
    if (!desktop_system__font_path(path, sizeof(path), TRUE) || !font__register_from_file("Desktop-Bold", path))
    {
        font__register_from_file("Desktop-Bold", "fonts/SpaceGrotesk-Bold.ttf");
    }
    desktop_memory__erase(path, sizeof(path));
    // Register the neutral UI fallback first, so removing a theme font never selects a decorative face.
    desktop_ui_fonts__scan(TRUE);
}

/**
 * @brief replaces stylesheet rules from the last valid theme without rereading files on each frame.
 * @return void
 */
void desktop_ui_theme__apply(void)
{
    scene__clear_styles();
    _ui_theme_internal__clear_node_styles(scene__root());
    if (g_theme_length)
    {
        parser_style__load_styles_from_memory(g_theme_bytes, g_theme_length, "desktop theme");
        desktop_ui_theme__scale(g_theme_scale);
    }
}

/**
 * @brief loads a bounded local stylesheet, retaining the previous theme when reading or parsing fails.
 * @param const char* name -> CSS filename under assets/themes; empty chooses classic.css
 * @param const desktop_settings_t* settings -> optional preferences to save before committing the theme
 * @return boole -> TRUE when the new rules were loaded; FALSE without changing the cached theme
 */
static boole _ui_theme_internal__load(const char *name, const desktop_settings_t *settings)
{
    char path[128] = { 0 };
    ubyte *bytes = NULL_POINTER;
    uint64 length = 0;
    boole loaded = FALSE;
    if (!desktop_settings__valid_theme(name) || clib__string_format(path, sizeof(path), "assets/themes/%s", name[0] ? name : "classic.css") <= 0)
    {
        return FALSE;
    }
    bytes = desktop_attachment_file__read(path, DESKTOP_THEME_BYTES - 1, &length);
    if (bytes && !clib__memchr(bytes, 0, length))
    {
        scene__clear_styles();
        _ui_theme_internal__clear_node_styles(scene__root());
        loaded = parser_style__load_styles_from_memory((const char *)bytes, (int64)length, path);
        if (loaded)
        {
            // Keep enough rule slots for the application before committing a custom theme.
            loaded = desktop_ui_theme__scale(g_theme_scale);
        }
        if (loaded && settings)
        {
            loaded = desktop_settings__save(settings);
        }
        if (loaded)
        {
            desktop_memory__erase(g_theme_bytes, sizeof(g_theme_bytes));
            clib__memcpy(g_theme_bytes, bytes, length);
            g_theme_length = (int64)length;
            desktop_ui_layout__theme(name);
        }
        else
        {
            desktop_ui_theme__apply();
        }
    }
    if (bytes)
    {
        desktop_memory__erase(bytes, length);
        free(bytes);
    }
    desktop_memory__erase(path, sizeof(path));
    return loaded;
}

/**
 * @brief selects a stylesheet without changing saved preferences.
 * @param const char* name -> bounded local CSS filename
 * @return boole -> TRUE after a successful load
 */
boole desktop_ui_theme__load(const char *name)
{
    return _ui_theme_internal__load(name, NULL);
}

/**
 * @brief commits a validated theme only after the corresponding settings were saved.
 * @param const desktop_settings_t* settings -> candidate preferences
 * @return boole -> TRUE when both theme parsing and the settings write succeeded
 */
boole desktop_ui_theme__save(const desktop_settings_t *settings)
{
    return settings && _ui_theme_internal__load(settings->theme, settings);
}

/**
 * @brief clears application-owned theme storage after the GUI has stopped using the rules.
 * @return void
 */
void desktop_ui_theme__shutdown(void)
{
    desktop_memory__erase(g_theme_bytes, sizeof(g_theme_bytes));
    g_theme_length = 0;
    g_theme_scale = 100;
    desktop_ui_layout__theme(NULL);
    clib__null_memory(g_theme_scale_handles, sizeof(g_theme_scale_handles));
}
