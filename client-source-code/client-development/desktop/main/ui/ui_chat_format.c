#include "ui/ui_chat_format.h"
#include "ui/ui_fonts.h"
#include "ui/ui_layout.h"
#include "platform/system.h"
#include "widgets/widget_select.h"

static desktop_session_t *g_format_session;
static desktop_settings_t *g_format_settings;
static boole g_format_changed;
static boole g_format_controls_visible;
static uint64 g_format_save_at;

static gui_color_t _ui_chat_format_internal__color(const char *text)
{
    unsigned int value = 0;
    int64 index = 0;
    gui_color_t color = { 0, 0, 0, 1 };
    for (index = 1; index < 7; index++)
    {
        ubyte digit = (ubyte)clib__tolower(text[index]);
        value = (value << 4) | (digit <= '9' ? digit - '0' : digit - 'a' + 10);
    }
    color.r = ((value >> 16) & 255) / 255.0f;
    color.g = ((value >> 8) & 255) / 255.0f;
    color.b = (value & 255) / 255.0f;
    return color;
}

/** @brief Applies validated text appearance directly; font tokens are never parsed as layout. */
void desktop_ui_chat_format__message(gui_node_t *node, const desktop_chat_format_t *format)
{
    desktop_chat_format_t fallback;
    boole browser_default = FALSE;
    if (!node)
    {
        return;
    }
    if (!desktop_chat_format__valid(format))
    {
        desktop_chat_format__default(&fallback);
        format = &fallback;
    }
    scene__set_font_override(node, desktop_ui_fonts__family(format->font), (float)format->size * (g_format_settings ? (float)g_format_settings->chat.scale_percent / 100 : 1));
    // Older browser messages spell the default as white, which is invisible on Classic's white chat background.
    browser_default = format->size == 12 && clib__strcmp(format->font, "custom-font-usage-default") == 0 && (clib__strcmp(format->color, "#ffffff") == 0 || clib__strcmp(format->color, "#FFFFFF") == 0);
    if (format->color[0] && !browser_default)
    {
        scene__set_font_color_override(node, _ui_chat_format_internal__color(format->color));
    }
    else
    {
        scene__clear_font_color_override(node);
    }
}

static void _ui_chat_format_internal__choices(void)
{
    gui_node_t *select = scene__find_by_id("chat-font");
    gui_node_t *child = NULL_POINTER;
    gui_node_t *next = NULL_POINTER;
    int64 index = 0;
    if (!select || !g_format_settings)
    {
        return;
    }
    scene__select_open(select, FALSE);
    child = select->first_child;
    select->first_child = NULL;
    select->last_child = NULL;
    select->child_count = 0;
    while (child)
    {
        next = child->next_sibling;
        child->parent = NULL;
        child->next_sibling = NULL;
        scene__node_free(child);
        child = next;
    }
    select->value = 0;
    for (index = -1; index < desktop_ui_fonts__count(); index++)
    {
        const char *label = index < 0 ? "Default font" : desktop_ui_fonts__name(index);
        child = scene__node_new(GUI_NODE_TYPE__OPTION);
        if (!child)
        {
            break;
        }
        scene__set_text(child, label, clib__strlen(label));
        scene__add_child(select, child);
        if (clib__strcmp(g_format_settings->chat_format.font, desktop_ui_fonts__token(index)) == 0)
        {
            select->value = (float)(index + 1);
        }
    }
}

static void _ui_chat_format_internal__preview(void)
{
    gui_node_t *input = scene__find_by_id("message-input");
    gui_node_t *slider = scene__find_by_id("chat-font-size");
    gui_node_t *label = scene__find_by_id("chat-font-size-label");
    gui_node_t *swatch = scene__find_by_id("chat-color-button");
    gui_node_t *hex = scene__find_by_id("chat-color-hex");
    char text[20];
    if (!g_format_settings)
    {
        return;
    }
    desktop_ui_chat_format__message(input, &g_format_settings->chat_format);
    if (slider)
    {
        slider->value = (float)g_format_settings->chat_format.size;
    }
    if (label)
    {
        clib__string_format(text, sizeof(text), "%lld px", g_format_settings->chat_format.size);
        scene__set_text(label, text, clib__strlen(text));
    }
    if (swatch)
    {
        if (g_format_settings->chat_format.color[0])
        {
            scene__set_font_color_override(swatch, _ui_chat_format_internal__color(g_format_settings->chat_format.color));
        }
        else
        {
            scene__clear_font_color_override(swatch);
        }
    }
    if (hex && scene__focus() != hex)
    {
        const char *value = g_format_settings->chat_format.color[0] ? g_format_settings->chat_format.color : "";
        scene__set_text(hex, value, clib__strlen(value));
    }
}

static void _ui_chat_format_internal__changed(void)
{
    desktop_session__set_chat_format(g_format_session, &g_format_settings->chat_format);
    _ui_chat_format_internal__preview();
    g_format_save_at = desktop_system__milliseconds() + 500;
    g_format_changed = TRUE;
}

static void _ui_chat_format_internal__font(gui_event_t *event)
{
    int64 index = event && event->sender ? (int64)event->sender->value - 1 : -1;
    clib__string_copy(g_format_settings->chat_format.font, sizeof(g_format_settings->chat_format.font), desktop_ui_fonts__token(index));
    _ui_chat_format_internal__changed();
}

static void _ui_chat_format_internal__size(gui_event_t *event)
{
    int64 size = event && event->sender ? (int64)event->sender->value : 12;
    g_format_settings->chat_format.size = size < 12 ? 12 : size > 30 ? 30 : size;
    _ui_chat_format_internal__changed();
}

static void _ui_chat_format_internal__close_color(gui_event_t *event)
{
    gui_node_t *panel = scene__find_by_id("chat-color-panel");
    (void)event;
    if (panel)
    {
        clib__string_copy(panel->klass, sizeof(panel->klass), "column panel hidden");
    }
    scene__set_focus(scene__find_by_id("message-input"));
}

/** @brief Keeps the formatting row's open state when a theme or layout is reloaded. */
static void _ui_chat_format_internal__controls(void)
{
    gui_node_t *controls = scene__find_by_id("chat-format-controls");
    gui_node_t *toggle = scene__find_by_id("chat-format-toggle");

    if (controls)
    {
        clib__string_copy(controls->klass, sizeof(controls->klass), g_format_controls_visible ? "row" : "row hidden");
    }
    if (toggle)
    {
        toggle->value = g_format_controls_visible ? 1 : 0;
    }
}

/** @brief Opens formatting only when requested and returns focus to the draft when closed. */
static void _ui_chat_format_internal__toggle(gui_event_t *event)
{
    (void)event;
    g_format_controls_visible = !g_format_controls_visible;
    _ui_chat_format_internal__controls();
    if (!g_format_controls_visible)
    {
        scene__select_open(scene__find_by_id("chat-font"), FALSE);
        _ui_chat_format_internal__close_color(NULL);
    }
    g_format_changed = TRUE;
}

static void _ui_chat_format_internal__open_color(gui_event_t *event)
{
    gui_node_t *panel = scene__find_by_id("chat-color-panel");
    gui_node_t *picker = scene__find_by_id("chat-color-picker");
    gui_node_t *input = scene__find_by_id("message-input");
    gui_color_t color;
    float maximum;
    float minimum;
    float difference;
    (void)event;
    if (!panel || !picker || !input)
    {
        return;
    }
    color = g_format_settings->chat_format.color[0] ? _ui_chat_format_internal__color(g_format_settings->chat_format.color) : input->resolved.font_color;
    maximum = color.r > color.g ? color.r : color.g;
    maximum = maximum > color.b ? maximum : color.b;
    minimum = color.r < color.g ? color.r : color.g;
    minimum = minimum < color.b ? minimum : color.b;
    difference = maximum - minimum;
    picker->value = 0;
    if (difference > 0)
    {
        picker->value = maximum == color.r ? (color.g - color.b) / difference : maximum == color.g ? 2 + (color.b - color.r) / difference : 4 + (color.r - color.g) / difference;
        picker->value /= 6;
        if (picker->value < 0)
        {
            picker->value += 1;
        }
    }
    picker->value_min = maximum > 0 ? difference / maximum : 0;
    picker->value_max = maximum;
    clib__string_copy(panel->klass, sizeof(panel->klass), "column panel");
    desktop_ui_layout__raise(panel);
    _ui_chat_format_internal__preview();
}

static void _ui_chat_format_internal__color_change(gui_event_t *event)
{
    gui_color_t color = event->color.value;
    clib__string_format(g_format_settings->chat_format.color, sizeof(g_format_settings->chat_format.color), "#%02x%02x%02x", (unsigned int)(color.r * 255 + 0.5f), (unsigned int)(color.g * 255 + 0.5f), (unsigned int)(color.b * 255 + 0.5f));
    _ui_chat_format_internal__changed();
}

static void _ui_chat_format_internal__hex(gui_event_t *event)
{
    desktop_chat_format_t candidate = g_format_settings->chat_format;
    gui_node_t *input = scene__find_by_id("chat-color-hex");
    (void)event;
    if (input && clib__string_copy(candidate.color, sizeof(candidate.color), input->text) && desktop_chat_format__valid(&candidate))
    {
        g_format_settings->chat_format = candidate;
        _ui_chat_format_internal__changed();
        _ui_chat_format_internal__close_color(NULL);
    }
}

static void _ui_chat_format_internal__reset(gui_event_t *event)
{
    (void)event;
    desktop_chat_format__default(&g_format_settings->chat_format);
    desktop_ui_chat_format__apply();
    _ui_chat_format_internal__changed();
}

void desktop_ui_chat_format__initialize(desktop_session_t *session, desktop_settings_t *settings)
{
    g_format_session = session;
    g_format_settings = settings;
    g_format_controls_visible = FALSE;
    scene__register_handler("on_chat_font", _ui_chat_format_internal__font);
    scene__register_handler("on_chat_font_size", _ui_chat_format_internal__size);
    scene__register_handler("on_chat_color_open", _ui_chat_format_internal__open_color);
    scene__register_handler("on_chat_color_close", _ui_chat_format_internal__close_color);
    scene__register_handler("on_chat_color_change", _ui_chat_format_internal__color_change);
    scene__register_handler("on_chat_color_hex", _ui_chat_format_internal__hex);
    scene__register_handler("on_chat_format_reset", _ui_chat_format_internal__reset);
    scene__register_handler("on_chat_format_toggle", _ui_chat_format_internal__toggle);
}

void desktop_ui_chat_format__apply(void)
{
    if (!g_format_settings)
    {
        return;
    }
    if (!desktop_chat_format__valid(&g_format_settings->chat_format))
    {
        desktop_chat_format__default(&g_format_settings->chat_format);
    }
    if (clib__strcmp(g_format_settings->chat_format.font, "custom-font-usage-default") != 0 && !desktop_ui_fonts__family(g_format_settings->chat_format.font)[0])
    {
        clib__string_copy(g_format_settings->chat_format.font, sizeof(g_format_settings->chat_format.font), "custom-font-usage-default");
    }
    desktop_session__set_chat_format(g_format_session, &g_format_settings->chat_format);
    _ui_chat_format_internal__controls();
    _ui_chat_format_internal__choices();
    _ui_chat_format_internal__preview();
    g_format_changed = TRUE;
}

void desktop_ui_chat_format__open_fonts(void)
{
    g_format_controls_visible = TRUE;
    desktop_ui_fonts__scan(TRUE);
    desktop_ui_chat_format__apply();
    scene__select_open(scene__find_by_id("chat-font"), TRUE);
}

boole desktop_ui_chat_format__tick(void)
{
    boole changed = desktop_ui_fonts__scan(FALSE);
    if (changed)
    {
        if (clib__strcmp(g_format_settings->chat_format.font, "custom-font-usage-default") != 0 && !desktop_ui_fonts__family(g_format_settings->chat_format.font)[0])
        {
            clib__string_copy(g_format_settings->chat_format.font, sizeof(g_format_settings->chat_format.font), "custom-font-usage-default");
            _ui_chat_format_internal__changed();
        }
        desktop_ui_chat_format__apply();
    }
    changed |= g_format_changed;
    g_format_changed = FALSE;
    if (g_format_save_at && desktop_system__milliseconds() >= g_format_save_at)
    {
        if (!desktop_settings__save(g_format_settings))
        {
            gui_node_t *status = scene__find_by_id("connection-status");
            if (status)
            {
                scene__set_text(status, "Could not save chat formatting preferences.", 42);
            }
        }
        g_format_save_at = 0;
    }
    return changed;
}
