// scene_style.c -- selector-based style registration + two-pass
// resolution, split out of scene.c for readability. Owns the rule
// table + all selector parsing, class-set hashing, rule matching,
// and per-node style overlay logic.
// Public API (scene__register_style, scene__clear_styles,
// scene__set_background_color_override, etc.) stays declared in
// scene.h; this TU implements it. The entry point scene.c calls
// every frame is scene_style__apply_rules_tree +
// scene_style__resolve_tree -- see scene_style.h.

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "motion.h"
#include "scene_style.h"
#include "style_resources.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// ===== selector-based style registration ====================================
// each registered rule has a parsed selector (optional type, optional
// class, optional id, optional pseudo-state) and a gui_style_t payload.
// rules are applied in three specificity passes:
// pass 0: type-only rules
// pass 1: rules with a class
// pass 2: rules with an id
// within one pass, registration order decides ties (later wins).

#define _SCENE_STYLE_INTERNAL__MAX_STYLES MEEK_UI_MAX_STYLES

/**
 *one parsed style-registration row. specificity tier and per-state
 *destination are precomputed at registration time so application is
 *just a sweep of three passes.
 */
static void _scene_style_internal__checkbox_part(gui_checkbox_part_t* destination, const gui_checkbox_part_t* source);

typedef struct _scene_style_internal__style_rule_t
{
    char           type[32];      // 0x0
    char           klass[32];     // 0x20
    char           id[32];        // 0x40
    gui_node_state_e state;         // 0x60
    int64          specificity;   // 0x68
    gui_style_t      style;         // 0x70
    gui_node_type_e  type_enum;     // 0x330
    uint           klass_hash;    // 0x334
    uint           id_hash;       // 0x338
    boole          important;     // 0x33C  applied after every normal rule (!important)
    uint64 handle; // stable across updates; never reused after scene__clear_styles
    char compound[256]; // full selector for descendant and compound matching
} _scene_style_internal__style_rule_t;

static _scene_style_internal__style_rule_t _scene_style_internal__styles[_SCENE_STYLE_INTERNAL__MAX_STYLES];
static int64                             _scene_style_internal__style_count = 0;
static uint64                            _scene_style_internal__next_handle = 1;

/** @brief merges only explicitly supplied checkbox-part properties, including zero resets
 * @param gui_checkbox_part_t* destination -> accumulated part style
 * @param const gui_checkbox_part_t* source -> next partial rule
 * @return void
 */
static void _scene_style_internal__checkbox_part(gui_checkbox_part_t* destination, const gui_checkbox_part_t* source)
{
    if (source->set & GUI_CHECKBOX_PART__WIDTH)
    {
        destination->width = source->width;
    }
    if (source->set & GUI_CHECKBOX_PART__HEIGHT)
    {
        destination->height = source->height;
    }
    if (source->set & GUI_CHECKBOX_PART__RADIUS)
    {
        destination->radius = source->radius;
    }
    if (source->set & GUI_CHECKBOX_PART__INSET)
    {
        destination->inset = source->inset;
    }
    if (source->set & GUI_CHECKBOX_PART__BORDER_WIDTH)
    {
        destination->border_width = source->border_width;
    }
    if (source->set & GUI_CHECKBOX_PART__BORDER_STYLE)
    {
        destination->border_style = source->border_style;
    }
    if (source->set & GUI_CHECKBOX_PART__BORDER_COLOR)
    {
        destination->border_color = source->border_color;
        destination->border_gradient = source->border_gradient;
        destination->has_border_gradient = source->has_border_gradient;
    }
    if (source->set & GUI_CHECKBOX_PART__BACKGROUND)
    {
        destination->background = source->background;
        destination->gradient = source->gradient;
        destination->has_gradient = source->has_gradient;
    }
    destination->set |= source->set;
}

// tiny char tests so we don't drag in <ctype.h>.
static boole _scene_style_internal__is_alpha(char c)
{
    return (boole)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}
static boole _scene_style_internal__is_alnum(char c)
{
    return (boole)(_scene_style_internal__is_alpha(c) || (c >= '0' && c <= '9') || c == '-');
}

static void _scene_style_internal__copy_token(char* start, char* end, char* out, int64 cap)
{
    int64 n = (int64)(end - start);
    if (n >= cap)
    {
        n = cap - 1;
    }
    stdlib__memcpy(out, start, (size_t)n);
    out[n] = 0;
}

static boole _scene_style_internal__parse_selector(char* sel, _scene_style_internal__style_rule_t* out)
{
    if (stdlib__strchr(sel, ' ') != NULL_POINTER || stdlib__strstr(sel, ":focus") != NULL_POINTER || stdlib__strstr(sel, ":checked") != NULL_POINTER || stdlib__strstr(sel, ":nth-child(") != NULL_POINTER || (stdlib__strchr(sel, '.') != NULL_POINTER && stdlib__strchr(stdlib__strchr(sel, '.') + 1, '.') != NULL_POINTER))
    {
        stdlib__strcpy_safe(out->compound, sizeof(out->compound), sel);
        out->specificity = (stdlib__strchr(sel, '#') != NULL_POINTER) ? 2 : ((stdlib__strchr(sel, '.') != NULL_POINTER) ? 1 : 0);
        out->state = GUI_NODE_STATE__DEFAULT;
        return TRUE;
    }
    out->type[0]  = 0;
    out->klass[0] = 0;
    out->id[0]    = 0;
    out->state    = GUI_NODE_STATE__DEFAULT;

    char* p   = sel;
    char* end = sel + stdlib__strlen(sel);

    char* colon = NULL_POINTER;
    for (char* q = p; q < end; q++)
    {
        if (*q == ':')
        {
            colon = q;
            break;
        }
    }
    if (colon != NULL_POINTER)
    {
        char* ps = colon + 1;
        if      (stdlib__strcmp(ps, "hover")     == 0)
        {
            out->state = GUI_NODE_STATE__HOVER;
        }
        else if (stdlib__strcmp(ps, "pressed")   == 0 || stdlib__strcmp(ps, "active") == 0)
        {
            out->state = GUI_NODE_STATE__PRESSED;
        }
        else if (stdlib__strcmp(ps, "disabled")  == 0)
        {
            out->state = GUI_NODE_STATE__DISABLED;
        }
        else if (stdlib__strcmp(ps, "default")   == 0)
        {
            out->state = GUI_NODE_STATE__DEFAULT;
        }
        else if (stdlib__strcmp(ps, "appear")    == 0)
        {
            out->state = GUI_NODE_STATE__APPEAR;
        }
        else if (stdlib__strcmp(ps, "disappear") == 0 || stdlib__strcmp(ps, "dissapear") == 0)
        {
            out->state = GUI_NODE_STATE__DISAPPEAR;
        }
        else
        {
            log_warn("%s %s %s", "unknown pseudo-state ':", ps, "'");
            return FALSE;
        }
        end = colon;
    }

    if (p < end && _scene_style_internal__is_alpha(*p))
    {
        char* start = p;
        while (p < end && _scene_style_internal__is_alnum(*p))
        {
            p++;
        }
        _scene_style_internal__copy_token(start, p, out->type, (int64)sizeof(out->type));
    }

    while (p < end)
    {
        if (*p == '.')
        {
            p++;
            char* start = p;
            while (p < end && _scene_style_internal__is_alnum(*p))
            {
                p++;
            }
            if (start == p)
            {
                log_warn("%s", "empty class after '.'");
                return FALSE;
            }
            _scene_style_internal__copy_token(start, p, out->klass, (int64)sizeof(out->klass));
        }
        else if (*p == '#')
        {
            p++;
            char* start = p;
            while (p < end && _scene_style_internal__is_alnum(*p))
            {
                p++;
            }
            if (start == p)
            {
                log_warn("%s", "empty id after '#'");
                return FALSE;
            }
            _scene_style_internal__copy_token(start, p, out->id, (int64)sizeof(out->id));
        }
        else
        {
            log_warn("%s %c %s %s %s", "unexpected '", *p, "' in selector '", sel, "'");
            return FALSE;
        }
    }

    if      (out->id[0]    != 0)
    {
        out->specificity = 2;
    }
    else if (out->klass[0] != 0)
    {
        out->specificity = 1;
    }
    else
    {
        out->specificity = 0;
    }

    if (out->type[0] != 0)
    {
        gui_node_type_e te = GUI_NODE_TYPE__COUNT;
        if (widget_registry__lookup_by_name(out->type, &te))
        {
            out->type_enum = te;
        }
        else
        {
            out->type_enum = GUI_NODE_TYPE__COUNT;
        }
    }
    else
    {
        out->type_enum = GUI_NODE_TYPE__COUNT;
    }
    out->klass_hash = (out->klass[0] != 0) ? scene__hash_name(out->klass) : 0u;
    out->id_hash    = (out->id[0]    != 0) ? scene__hash_name(out->id)    : 0u;

    if (out->type[0] == 0 && out->klass[0] == 0 && out->id[0] == 0)
    {
        log_warn("%s", "empty selector");
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief register a style under a selector. The style is copied; the caller may free or reuse its source after the call returns.
 *
 * @param char* selector -> Selector text (see header comment for forms).
 * @param const gui_style_t* style -> Style payload to apply when the selector matches.
 *
 * @return void
 */
void scene__register_style(char* selector, const gui_style_t* style)
{
    (void)scene__add_style(selector, style, FALSE);
}

/**
 * @brief register a rule whose declarations carry !important: it is applied after every normal rule, whatever the tiers say, and among important rules the usual precedence holds.
 *
 * @param char* selector -> Same grammar as scene__register_style.
 * @param const gui_style_t* style -> The important declarations only.
 *
 * @return void
 */
void scene__register_style_important(char* selector, const gui_style_t* style)
{
    (void)scene__add_style(selector, style, TRUE);
}

uint64 scene__add_style(char* selector, const gui_style_t* style, boole important)
{
    _scene_style_internal__style_rule_t rule;

    stdlib__null_memory(&rule, sizeof(rule));
    if (selector == NULL_POINTER || style == NULL_POINTER || _scene_style_internal__next_handle == 0)
    {
        return 0;
    }
    if (_scene_style_internal__style_count >= _SCENE_STYLE_INTERNAL__MAX_STYLES)
    {
        log_error("%s %d %s", "scene__register_style: table full (", _SCENE_STYLE_INTERNAL__MAX_STYLES, ")");
        return 0;
    }
    if (_scene_style_internal__parse_selector(selector, &rule) == FALSE)
    {
        return 0;
    }
    rule.style = *style;
    rule.important = important;
    rule.handle = _scene_style_internal__next_handle++;
    _scene_style_internal__styles[_scene_style_internal__style_count++] = rule;
    return rule.handle;
}

boole scene__update_style(uint64 handle, const gui_style_t* style)
{
    if (handle == 0 || style == NULL_POINTER)
    {
        return FALSE;
    }
    for (int64 i = 0; i < _scene_style_internal__style_count; i++)
    {
        if (_scene_style_internal__styles[i].handle == handle)
        {
            _scene_style_internal__styles[i].style = *style;
            return TRUE;
        }
    }
    return FALSE;
}

int64 scene__style_count(void)
{
    return _scene_style_internal__style_count;
}

int64 scene__style_capacity(void)
{
    return _SCENE_STYLE_INTERNAL__MAX_STYLES;
}

/**
 * @brief forget every previously registered style.
 *
 * @return void
 */
void scene__clear_styles(void)
{
    motion__reset();
    style_resources__clear();
    _scene_style_internal__style_count = 0;
}

// Multi-class support. A node's klass field stores the literal
// `class="..."` attribute string ("primary big-button"); we tokenize
// on whitespace into up to 8 individual hashes so the rule's
// klass_hash can match ANY of them.
#define _SCENE_STYLE_INTERNAL__MAX_CLASS_TOKENS 8

typedef struct _scene_style_internal__class_set_t
{
    uint  hashes[_SCENE_STYLE_INTERNAL__MAX_CLASS_TOKENS];   // 0x0
    int64 count;                                             // 0x20
} _scene_style_internal__class_set_t;

static void _scene_style_internal__build_class_set(const char* klass, _scene_style_internal__class_set_t* out)
{
    out->count = 0;
    if (klass == NULL_POINTER || klass[0] == 0)
    {
        return;
    }

    const char* p = klass;
    while (*p != 0)
    {
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p == 0)
        {
            break;
        }
        const char* tok_start = p;
        while (*p != 0 && *p != ' ' && *p != '\t')
        {
            p++;
        }
        if (out->count >= _SCENE_STYLE_INTERNAL__MAX_CLASS_TOKENS)
        {
            return;
        }
        // fnv-1a 32-bit inline. Mirrors scene__hash_name exactly so
        // a rule's klass_hash (computed via scene__hash_name at parse
        // time) compares cleanly.
        uint h = 2166136261u;
        for (const char* c = tok_start; c < p; c++)
        {
            h ^= (uint)(unsigned char)*c;
            h *= 16777619u;
        }
        out->hashes[out->count++] = h;
    }
}

static boole _scene_style_internal__class_set_contains(const _scene_style_internal__class_set_t* set, uint h)
{
    for (int64 i = 0; i < set->count; i++)
    {
        if (set->hashes[i] == h)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static boole _scene_style_internal__rule_matches(gui_node_t* n, const _scene_style_internal__style_rule_t* r, const _scene_style_internal__class_set_t* node_classes, uint node_id_hash)
{
    if (r->compound[0] != 0)
    {
        return motion__matches(n, r->compound);
    }
    if (r->type_enum != GUI_NODE_TYPE__COUNT && r->type_enum != n->type)
    {
        return FALSE;
    }
    if (r->klass_hash != 0 && _scene_style_internal__class_set_contains(node_classes, r->klass_hash) == FALSE)
    {
        return FALSE;
    }
    if (r->id_hash    != 0 && r->id_hash != node_id_hash)
    {
        return FALSE;
    }
    return TRUE;
}

static void _scene_style_internal__overlay_style(gui_style_t* dst, const gui_style_t* src)
{
    if (src->has_background_color)
    {
        dst->background_color = src->background_color;
        dst->has_background_color = TRUE;
    }
    if (src->has_accent_color)
    {
        dst->accent_color     = src->accent_color;
        dst->has_accent_color     = TRUE;
    }
    if (src->has_font_color)
    {
        dst->font_color       = src->font_color;
        dst->has_font_color       = TRUE;
    }

    // a written border-width wins even when it is 0, which is how a
    // later rule removes an earlier border (CSS cascade)
    if (src->border_width_set == TRUE)
    {
        dst->border_width     = src->border_width;
        dst->border_width_set = TRUE;
    }
    if (src->has_border_color)
    {
        dst->border_color = src->border_color;
        dst->has_border_color = TRUE;
        dst->has_border_gradient = src->has_border_gradient;
    }
    if (src->has_border_gradient)
    {
        dst->border_gradient     = src->border_gradient;
        dst->has_border_gradient = TRUE;
    }
    if (src->border_style_set || src->border_style != GUI_BORDER_STYLE__NONE)
    {
        dst->border_style = src->border_style;
        dst->border_style_set = TRUE;
    }
    if (src->font_family[0] != 0)
    {
        stdlib__memcpy(dst->font_family, src->font_family, sizeof(dst->font_family));
    }
    if (src->font_size > 0.0f)
    {
        dst->font_size = src->font_size;
    }
    if (src->font_size_explicit)
    {
        dst->font_size_explicit = TRUE;
    }
    if (src->line_height != 0.0f)
    {
        dst->line_height    = src->line_height;
        dst->line_height_px = 0.0f;
    }
    if (src->line_height_px > 0.0f)
    {
        dst->line_height_px = src->line_height_px;
        dst->line_height    = 0.0f;
    }
    if (src->radius    > 0.0f)
    {
        dst->radius    = src->radius;
    }
    if (src->pad_t     > 0.0f)
    {
        dst->pad_t     = src->pad_t;
    }
    if (src->pad_r     > 0.0f)
    {
        dst->pad_r     = src->pad_r;
    }
    if (src->pad_b     > 0.0f)
    {
        dst->pad_b     = src->pad_b;
    }
    if (src->pad_l     > 0.0f)
    {
        dst->pad_l     = src->pad_l;
    }
    if (src->gap       > 0.0f)
    {
        dst->gap       = src->gap;
    }
    if (src->size_w    > 0.0f)
    {
        dst->size_w    = src->size_w;
    }
    if (src->size_h    > 0.0f)
    {
        dst->size_h    = src->size_h;
    }
    if (src->size_w_explicit)
    {
        dst->size_w_explicit = TRUE;
    }
    if (src->size_h_explicit)
    {
        dst->size_h_explicit = TRUE;
    }
    if (src->width_pct > 0.0f)
    {
        dst->width_pct = src->width_pct;
    }
    if (src->height_pct> 0.0f)
    {
        dst->height_pct= src->height_pct;
    }
    if (src->min_w     > 0.0f)
    {
        dst->min_w     = src->min_w;
    }
    if (src->min_h     > 0.0f)
    {
        dst->min_h     = src->min_h;
    }

    if (src->overflow_x != GUI_OVERFLOW__VISIBLE)
    {
        dst->overflow_x = src->overflow_x;
    }
    if (src->overflow_y != GUI_OVERFLOW__VISIBLE)
    {
        dst->overflow_y = src->overflow_y;
    }
    if (src->scrollbar_size   > 0.0f)
    {
        dst->scrollbar_size   = src->scrollbar_size;
    }
    if (src->scrollbar_radius > 0.0f)
    {
        dst->scrollbar_radius = src->scrollbar_radius;
    }
    if (src->has_scrollbar_track)
    {
        dst->scrollbar_track = src->scrollbar_track;
        dst->has_scrollbar_track = TRUE;
    }
    if (src->has_scrollbar_thumb)
    {
        dst->scrollbar_thumb = src->scrollbar_thumb;
        dst->has_scrollbar_thumb = TRUE;
    }

    if (src->visibility != GUI_VISIBILITY__VISIBLE)
    {
        dst->visibility = src->visibility;
    }
    if (src->display_set == TRUE || src->display != GUI_DISPLAY__BLOCK)
    {
        dst->display    = src->display;
        dst->display_set = src->display_set;
    }
    if (src->flex_direction != 0) { dst->flex_direction = src->flex_direction; }
    if (src->table_layout != 0) { dst->table_layout = src->table_layout; }
    if (src->border_collapse != 0) { dst->border_collapse = src->border_collapse; }
    if (src->table_vertical_align != 0) { dst->table_vertical_align = src->table_vertical_align; }
    if (src->border_spacing_set == TRUE)
    {
        dst->border_spacing_x = src->border_spacing_x;
        dst->border_spacing_y = src->border_spacing_y;
        dst->border_spacing_set = TRUE;
    }
    if (src->position   != GUI_POSITION__STATIC)
    {
        dst->position   = src->position;
    }
    if (src->inset_t_set == TRUE || src->inset_t != 0.0f || src->inset_t_pct != 0.0f)
    {
        dst->inset_t     = src->inset_t;
        dst->inset_t_pct = src->inset_t_pct;
        dst->inset_t_set = TRUE;
    }
    if (src->inset_r_set == TRUE || src->inset_r != 0.0f || src->inset_r_pct != 0.0f)
    {
        dst->inset_r     = src->inset_r;
        dst->inset_r_pct = src->inset_r_pct;
        dst->inset_r_set = TRUE;
    }
    if (src->inset_b_set == TRUE || src->inset_b != 0.0f || src->inset_b_pct != 0.0f)
    {
        dst->inset_b     = src->inset_b;
        dst->inset_b_pct = src->inset_b_pct;
        dst->inset_b_set = TRUE;
    }
    if (src->inset_l_set == TRUE || src->inset_l != 0.0f || src->inset_l_pct != 0.0f)
    {
        dst->inset_l     = src->inset_l;
        dst->inset_l_pct = src->inset_l_pct;
        dst->inset_l_set = TRUE;
    }

    if (src->scroll_smooth_ms > 0.0f)
    {
        dst->scroll_smooth_ms = src->scroll_smooth_ms;
    }
    // scroll-end-effect: a rule that wrote it wins, so none overrides wobble
    if (src->scroll_end_effect_set == TRUE)
    {
        dst->scroll_end_effect = src->scroll_end_effect;
        dst->scroll_end_effect_amount = src->scroll_end_effect_amount;
        dst->scroll_end_effect_set = TRUE;
    }
    if (src->scroll_start_effect_set == TRUE)
    {
        dst->scroll_start_effect = src->scroll_start_effect;
        dst->scroll_start_effect_amount = src->scroll_start_effect_amount;
        dst->scroll_start_effect_set = TRUE;
    }
    if (src->scroll_fade_px   > 0.0f)
    {
        dst->scroll_fade_px   = src->scroll_fade_px;
    }
    // rubber-band overscroll: a rule that wrote the flags wins (so none
    // overrides auto); the tuning values win when set
    if (src->overscroll_set == TRUE)
    {
        dst->overscroll_x = src->overscroll_x;
        dst->overscroll_y = src->overscroll_y;
        dst->overscroll_set = TRUE;
    }
    if (src->overscroll_resistance > 0.0f)
    {
        dst->overscroll_resistance = src->overscroll_resistance;
    }
    if (src->overscroll_limit_px > 0.0f || src->overscroll_limit_pct > 0.0f)
    {
        dst->overscroll_limit_px = src->overscroll_limit_px;
        dst->overscroll_limit_pct = src->overscroll_limit_pct;
    }
    if (src->overscroll_duration_ms > 0.0f)
    {
        dst->overscroll_duration_ms = src->overscroll_duration_ms;
    }
    if (src->overscroll_easing_set == TRUE)
    {
        dst->overscroll_easing = src->overscroll_easing;
        stdlib__memcpy(dst->overscroll_easing_params, src->overscroll_easing_params, sizeof(dst->overscroll_easing_params));
        dst->overscroll_easing_set = TRUE;
    }
    _scene_style_internal__checkbox_part(&dst->checkbox_track, &src->checkbox_track);
    _scene_style_internal__checkbox_part(&dst->checkbox_thumb, &src->checkbox_thumb);
    if (src->checkbox_mark != GUI_CHECKBOX_MARK__DEFAULT)
    {
        dst->checkbox_mark = src->checkbox_mark;
    }
    if (src->has_checkbox_mark_width)
    {
        dst->checkbox_mark_width = src->checkbox_mark_width;
        dst->has_checkbox_mark_width = TRUE;
    }
    if (src->has_checkbox_mark_inset)
    {
        dst->checkbox_mark_inset = src->checkbox_mark_inset;
        dst->has_checkbox_mark_inset = TRUE;
    }
    if (src->has_checkbox_mark_color)
    {
        dst->checkbox_mark_color = src->checkbox_mark_color;
        dst->has_checkbox_mark_color = TRUE;
    }
    // widget looks: set when written
    if (src->switch_look == TRUE)
    {
        dst->switch_look = TRUE;
    }
    if (src->has_switch_track_h == TRUE)
    {
        dst->switch_track_h = src->switch_track_h;
        dst->has_switch_track_h = TRUE;
    }
    if (src->has_switch_thumb_size == TRUE)
    {
        dst->switch_thumb_size = src->switch_thumb_size;
        dst->has_switch_thumb_size = TRUE;
    }
    if (src->has_switch_thumb_color == TRUE)
    {
        dst->switch_thumb_color = src->switch_thumb_color;
        dst->has_switch_thumb_color = TRUE;
    }
    if (src->has_switch_thumb_off_color == TRUE)
    {
        dst->switch_thumb_off_color = src->switch_thumb_off_color;
        dst->has_switch_thumb_off_color = TRUE;
    }
    if (src->app_region_drag == TRUE)
    {
        dst->app_region_drag = TRUE;
    }
    if (src->slider_track_h > 0.0f)
    {
        dst->slider_track_h = src->slider_track_h;
    }
    if (src->slider_thumb_w > 0.0f)
    {
        dst->slider_thumb_w = src->slider_thumb_w;
    }
    if (src->slider_value_mode != 0)
    {
        dst->slider_value_mode = src->slider_value_mode;
    }
    if (src->swatch_size > 0.0f)
    {
        dst->swatch_size = src->swatch_size;
    }
    if (src->swatch_radius > 0.0f)
    {
        dst->swatch_radius = src->swatch_radius;
    }
    if (src->swatch_gap > 0.0f)
    {
        dst->swatch_gap = src->swatch_gap;
    }
    if (src->has_slider_fill == TRUE)
    {
        dst->slider_fill = src->slider_fill;
        dst->has_slider_fill = TRUE;
    }
    if (src->has_slider_value_color == TRUE)
    {
        dst->slider_value_color = src->slider_value_color;
        dst->has_slider_value_color = TRUE;
    }
    if (src->slider_value_pos != 0)
    {
        dst->slider_value_pos = src->slider_value_pos;
    }
    if (src->slider_decimals != 0)
    {
        dst->slider_decimals = src->slider_decimals;
    }
    if (src->popup_blur > 0.0f || (src->popup_set & GUI_POPUP_SET__BLUR))
    {
        dst->popup_blur = src->popup_blur;
    }
    if (src->has_popup_dim == TRUE)
    {
        dst->popup_dim = src->popup_dim;
        dst->has_popup_dim = TRUE;
    }
    if (src->popup_centered == TRUE || (src->popup_set & GUI_POPUP_SET__ALIGN))
    {
        dst->popup_centered = src->popup_centered;
    }
    // Preserve presence bits: zero can intentionally override an earlier rule.
    if (src->popup_set & GUI_POPUP_SET__PLACEMENT) dst->popup_placement = src->popup_placement;
    if (src->popup_set & GUI_POPUP_SET__REVEAL) dst->popup_reveal = src->popup_reveal;
    if (src->popup_set & GUI_POPUP_SET__DURATION) dst->popup_duration_ms = src->popup_duration_ms;
    if (src->popup_set & GUI_POPUP_SET__STAGGER) dst->popup_stagger_ms = src->popup_stagger_ms;
    if (src->popup_set & GUI_POPUP_SET__OFFSET) dst->popup_offset = src->popup_offset;
    if (src->popup_set & GUI_POPUP_SET__GAP) dst->popup_gap = src->popup_gap;
    if (src->popup_set & GUI_POPUP_SET__WIDTH) dst->popup_width = src->popup_width;
    if (src->popup_set & GUI_POPUP_SET__ROW_HEIGHT) dst->option_height = src->option_height;
    if (src->popup_set & GUI_POPUP_SET__PADDING) dst->option_padding = src->option_padding;
    dst->popup_set |= src->popup_set;
    if (src->has_popup_background) { dst->popup_background = src->popup_background; dst->has_popup_background = TRUE; }
    if (src->has_option_current_background) { dst->option_current_background = src->option_current_background; dst->has_option_current_background = TRUE; }
    if (src->has_option_hover_background) { dst->option_hover_background = src->option_hover_background; dst->has_option_hover_background = TRUE; }
    if (src->select_style_set == TRUE)
    {
        dst->select_style = src->select_style;
        dst->select_style_set = TRUE;
    }
    if (src->select_rows > 0.0f)
    {
        dst->select_rows = src->select_rows;
    }
    if (src->has_option_separator == TRUE)
    {
        dst->option_separator = src->option_separator;
        dst->has_option_separator = TRUE;
    }
    if (src->has_option_color == TRUE)
    {
        dst->option_color = src->option_color;
        dst->has_option_color = TRUE;
    }
    if (src->has_option_current_color == TRUE)
    {
        dst->option_current_color = src->option_current_color;
        dst->has_option_current_color = TRUE;
    }
    if (src->has_option_hover_color == TRUE)
    {
        dst->option_hover_color = src->option_hover_color;
        dst->has_option_hover_color = TRUE;
    }

    if (src->appear_ms > 0.0f)
    {
        dst->appear_ms     = src->appear_ms;
        dst->appear_easing = src->appear_easing;
        dst->appear_easing_params[0] = src->appear_easing_params[0];
        dst->appear_easing_params[1] = src->appear_easing_params[1];
        dst->appear_easing_params[2] = src->appear_easing_params[2];
        dst->appear_easing_params[3] = src->appear_easing_params[3];
    }
    if (src->has_appear_threshold)
    {
        dst->appear_threshold     = src->appear_threshold;
        dst->has_appear_threshold = TRUE;
    }
    if (src->has_disappear_threshold)
    {
        dst->disappear_threshold     = src->disappear_threshold;
        dst->has_disappear_threshold = TRUE;
    }

    // the animation declaration of a :appear / :disappear block: its
    // duration and easing. It was never copied before 2026-09-13, so
    // every pseudo block ran on the animator's 300 ms default and a
    // block with only an animation declaration did nothing at all.
    if (src->animation_duration_ms > 0.0f)
    {
        dst->animation_duration_ms   = src->animation_duration_ms;
        dst->animation_easing        = src->animation_easing;
        dst->animation_easing_params[0] = src->animation_easing_params[0];
        dst->animation_easing_params[1] = src->animation_easing_params[1];
        dst->animation_easing_params[2] = src->animation_easing_params[2];
        dst->animation_easing_params[3] = src->animation_easing_params[3];
    }

    if (src->transition_duration_ms > 0.0f)
    {
        dst->transition_duration_ms      = src->transition_duration_ms;
        dst->transition_easing           = src->transition_easing;
        dst->transition_easing_params[0] = src->transition_easing_params[0];
        dst->transition_easing_params[1] = src->transition_easing_params[1];
        dst->transition_easing_params[2] = src->transition_easing_params[2];
        dst->transition_easing_params[3] = src->transition_easing_params[3];
    }

    if (src->has_shadow)
    {
        dst->shadow_dx    = src->shadow_dx;
        dst->shadow_dy    = src->shadow_dy;
        dst->shadow_blur  = src->shadow_blur;
        dst->shadow_color = src->shadow_color;
        dst->has_shadow   = TRUE;
    }

    if (src->has_opacity)
    {
        dst->opacity = src->opacity;
        dst->has_opacity = TRUE;
    }
    if (src->z_index != 0)
    {
        dst->z_index = src->z_index;
    }
    if (src->blur_px > 0.0f)
    {
        dst->blur_px = src->blur_px;
    }

    if (src->has_bg_gradient)
    {
        dst->bg_gradient     = src->bg_gradient;
        dst->has_bg_gradient = TRUE;
    }

    if (src->has_background_image)
    {
        stdlib__memcpy(dst->background_image, src->background_image, sizeof(dst->background_image));
        dst->has_background_image = TRUE;
    }
    if (src->background_size != GUI_IMAGE_FIT__FILL)
    {
        dst->background_size = src->background_size;
    }
    if (src->object_fit      != GUI_IMAGE_FIT__FILL)
    {
        dst->object_fit      = src->object_fit;
    }

    if (src->halign != GUI_HALIGN__LEFT)
    {
        dst->halign = src->halign;
    }
    if (src->valign != GUI_VALIGN__TOP)
    {
        dst->valign = src->valign;
    }
    // margins were never copied from a rule into the node's slot, so a
    // .style margin silently did nothing; negative margins are legal in
    // CSS, hence != 0 rather than > 0
    if (src->margin_t != 0.0f)
    {
        dst->margin_t = src->margin_t;
    }
    if (src->margin_r != 0.0f)
    {
        dst->margin_r = src->margin_r;
    }
    if (src->margin_b != 0.0f)
    {
        dst->margin_b = src->margin_b;
    }
    if (src->margin_l != 0.0f)
    {
        dst->margin_l = src->margin_l;
    }
    if (src->justify_content != GUI_JUSTIFY__UNSET)
    {
        dst->justify_content = src->justify_content;
    }
    if (src->align_items != GUI_ALIGN__UNSET)
    {
        dst->align_items = src->align_items;
    }
    if (src->align_self != GUI_ALIGN__UNSET)
    {
        dst->align_self = src->align_self;
    }
    if (src->text_align != GUI_TEXT_ALIGN__LEFT)
    {
        dst->text_align = src->text_align;
    }
    if (src->text_transform != GUI_TEXT_TRANSFORM__NONE)
    {
        dst->text_transform = src->text_transform;
    }
    if (src->max_w > 0.0f)
    {
        dst->max_w = src->max_w;
    }
    if (src->max_h > 0.0f)
    {
        dst->max_h = src->max_h;
    }

    if (src->collection_layout  != GUI_COLLECTION_LAYOUT__GRID)
    {
        dst->collection_layout  = src->collection_layout;
    }
    if (src->collection_columns > 0)
    {
        dst->collection_columns = src->collection_columns;
    }
    if (src->item_width         > 0.0f)
    {
        dst->item_width         = src->item_width;
    }
    if (src->item_height        > 0.0f)
    {
        dst->item_height        = src->item_height;
    }
}

static void _scene_style_internal__apply_rules_to_node(gui_node_t* n)
{
    // See long-form comment in the pre-split scene.c for why we
    // preserve visibility + display across the memset: handlers
    // mutate them between frames, and a naive wipe would clobber.
    gui_visibility_e saved_vis  = n->style[GUI_NODE_STATE__DEFAULT].visibility;
    gui_display_e    saved_disp = n->style[GUI_NODE_STATE__DEFAULT].display;

    // Preserve runtime-driven absolute-positioning fields the same
    // way visibility + display are preserved. main.c animates a
    // task-switcher overlay's inset_t each tick to follow the
    // user's swipe-up progress; without this preservation the rule
    // re-apply below wipes the writes and the overlay snaps back to
    // its rule-defined inset every frame.
    gui_position_e saved_pos    = n->style[GUI_NODE_STATE__DEFAULT].position;
    float        saved_in_t   = n->style[GUI_NODE_STATE__DEFAULT].inset_t;
    float        saved_in_r   = n->style[GUI_NODE_STATE__DEFAULT].inset_r;
    float        saved_in_b   = n->style[GUI_NODE_STATE__DEFAULT].inset_b;
    float        saved_in_l   = n->style[GUI_NODE_STATE__DEFAULT].inset_l;
    boole        saved_in_t_set = n->style[GUI_NODE_STATE__DEFAULT].inset_t_set;
    boole        saved_in_r_set = n->style[GUI_NODE_STATE__DEFAULT].inset_r_set;
    boole        saved_in_b_set = n->style[GUI_NODE_STATE__DEFAULT].inset_b_set;
    boole        saved_in_l_set = n->style[GUI_NODE_STATE__DEFAULT].inset_l_set;
    boole        saved_has_op = n->style[GUI_NODE_STATE__DEFAULT].has_opacity;
    float        saved_op     = n->style[GUI_NODE_STATE__DEFAULT].opacity;
    // Preserve runtime-driven corner radius. main.c animates this
    // on fullscreen-view during the container-transform launch
    // animation (icon corner roundness -> 0 as the rect grows to
    // full panel). Without this preservation the radius would be
    // wiped to 0 by the memset each frame and never re-applied by
    // any rule (the .style rules for .fullscreen-view don't
    // declare radius), so the rect would look square from the
    // first frame -- losing the "icon morphing into app" cue.
    float        saved_radius = n->style[GUI_NODE_STATE__DEFAULT].radius;
    // Preserve runtime-driven width/height so main.c can animate
    // sizing fields on the task-switcher's cards-deck (grow-from-
    // small effect during the swipe-progress reveal). Same memset-
    // restore pattern as display + opacity above.
    float        saved_w      = n->style[GUI_NODE_STATE__DEFAULT].size_w;
    float        saved_h      = n->style[GUI_NODE_STATE__DEFAULT].size_h;
    boole        saved_w_expl = n->style[GUI_NODE_STATE__DEFAULT].size_w_explicit;
    boole        saved_h_expl = n->style[GUI_NODE_STATE__DEFAULT].size_h_explicit;
    float        saved_w_pct  = n->style[GUI_NODE_STATE__DEFAULT].width_pct;
    float        saved_h_pct  = n->style[GUI_NODE_STATE__DEFAULT].height_pct;
    // Preserve runtime-driven background color so per-tile color
    // sampling (icon_color_sampler in meek-shell) survives the
    // per-frame style resolve. Otherwise the .style rules don't
    // declare these and the memset wipes them between ticks.
    boole     saved_has_bg = n->style[GUI_NODE_STATE__DEFAULT].has_background_color;
    gui_color_t saved_bg     = n->style[GUI_NODE_STATE__DEFAULT].background_color;
    // Same story for the runtime-driven gradient: meek-shell sets
    // bg_gradient on per-tile launcher frames so the tile reads as a
    // soft vertical fade. Without preserving here those wipes back to
    // zero each frame.
    boole            saved_has_grad = n->style[GUI_NODE_STATE__DEFAULT].has_bg_gradient;
    gui_gradient_t   saved_grad     = n->style[GUI_NODE_STATE__DEFAULT].bg_gradient;
    // Preserve the PRESSED-state gradient too. Hosts (meek-shell)
    // may set per-tile pressed gradients programmatically (e.g. a
    // darker variant of the sampled DEFAULT gradient) so the
    // animator can interpolate between them on press. Without
    // preserving across the memset, the PRESSED slot gets wiped
    // each frame and no .style rule re-fills it (the press
    // state's gradient is set per-instance, not via class rules).
    boole            saved_has_grad_p  = n->style[GUI_NODE_STATE__PRESSED].has_bg_gradient;
    gui_gradient_t   saved_grad_p      = n->style[GUI_NODE_STATE__PRESSED].bg_gradient;
    // Preserve runtime-driven PRESSED-state size too. meek-shell
    // writes style[PRESSED].size_w/_h on launcher tiles so press
    // feedback shrinks the icon to 85% of the runtime-set default
    // (CSS percentages can't express "85% of natural" -- they
    // resolve against parent content, which makes the icon WIDER
    // when the user configures a larger launcher_icon_size_pct).
    float            saved_w_p      = n->style[GUI_NODE_STATE__PRESSED].size_w;
    float            saved_h_p      = n->style[GUI_NODE_STATE__PRESSED].size_h;
    boole            saved_w_expl_p = n->style[GUI_NODE_STATE__PRESSED].size_w_explicit;
    boole            saved_h_expl_p = n->style[GUI_NODE_STATE__PRESSED].size_h_explicit;

    if (_scene_style_internal__style_count > 0)
    {
        stdlib__memset(n->style, 0, sizeof(n->style));
    }

    n->style[GUI_NODE_STATE__DEFAULT].visibility            = saved_vis;
    n->style[GUI_NODE_STATE__DEFAULT].display               = saved_disp;
    n->style[GUI_NODE_STATE__DEFAULT].position              = saved_pos;
    n->style[GUI_NODE_STATE__DEFAULT].inset_t               = saved_in_t;
    n->style[GUI_NODE_STATE__DEFAULT].inset_r               = saved_in_r;
    n->style[GUI_NODE_STATE__DEFAULT].inset_b               = saved_in_b;
    n->style[GUI_NODE_STATE__DEFAULT].inset_l               = saved_in_l;
    n->style[GUI_NODE_STATE__DEFAULT].inset_t_set           = saved_in_t_set;
    n->style[GUI_NODE_STATE__DEFAULT].inset_r_set           = saved_in_r_set;
    n->style[GUI_NODE_STATE__DEFAULT].inset_b_set           = saved_in_b_set;
    n->style[GUI_NODE_STATE__DEFAULT].inset_l_set           = saved_in_l_set;
    n->style[GUI_NODE_STATE__DEFAULT].has_opacity           = saved_has_op;
    n->style[GUI_NODE_STATE__DEFAULT].opacity               = saved_op;
    n->style[GUI_NODE_STATE__DEFAULT].radius                = saved_radius;
    n->style[GUI_NODE_STATE__DEFAULT].size_w                = saved_w;
    n->style[GUI_NODE_STATE__DEFAULT].size_h                = saved_h;
    n->style[GUI_NODE_STATE__DEFAULT].size_w_explicit       = saved_w_expl;
    n->style[GUI_NODE_STATE__DEFAULT].size_h_explicit       = saved_h_expl;
    n->style[GUI_NODE_STATE__DEFAULT].width_pct             = saved_w_pct;
    n->style[GUI_NODE_STATE__DEFAULT].height_pct            = saved_h_pct;
    n->style[GUI_NODE_STATE__DEFAULT].has_background_color  = saved_has_bg;
    n->style[GUI_NODE_STATE__DEFAULT].background_color      = saved_bg;
    n->style[GUI_NODE_STATE__DEFAULT].has_bg_gradient       = saved_has_grad;
    n->style[GUI_NODE_STATE__DEFAULT].bg_gradient           = saved_grad;
    n->style[GUI_NODE_STATE__PRESSED].has_bg_gradient       = saved_has_grad_p;
    n->style[GUI_NODE_STATE__PRESSED].bg_gradient           = saved_grad_p;
    n->style[GUI_NODE_STATE__PRESSED].size_w                = saved_w_p;
    n->style[GUI_NODE_STATE__PRESSED].size_h                = saved_h_p;
    n->style[GUI_NODE_STATE__PRESSED].size_w_explicit       = saved_w_expl_p;
    n->style[GUI_NODE_STATE__PRESSED].size_h_explicit       = saved_h_expl_p;

    _scene_style_internal__class_set_t node_classes;
    _scene_style_internal__build_class_set(n->klass, &node_classes);
    uint node_id_hash = (n->id[0] != 0) ? scene__hash_name(n->id) : 0u;

    // pass 0 applies the normal rules by tier (type, class, id; later
    // wins within a tier), pass 1 the !important rules the same way, so
    // an important declaration beats every normal one
    for (int64 pass = 0; pass <= 1; pass++)
    {
        for (int64 spec = 0; spec <= 2; spec++)
        {
            for (int64 i = 0; i < _scene_style_internal__style_count; i++)
            {
                _scene_style_internal__style_rule_t* r = &_scene_style_internal__styles[i];
                if (r->specificity != spec || r->important != (boole)(pass == 1))
                {
                    continue;
                }
                if (_scene_style_internal__rule_matches(n, r, &node_classes, node_id_hash) == FALSE)
                {
                    continue;
                }
                _scene_style_internal__overlay_style(&n->style[r->state], &r->style);
            }
        }
    }
}

static void _scene_style_internal__apply_rules_recursive(gui_node_t* n)
{
    _scene_style_internal__apply_rules_to_node(n);

    gui_node_t* c = n->first_child;
    while (c != NULL_POINTER)
    {
        _scene_style_internal__apply_rules_recursive(c);
        c = c->next_sibling;
    }
}

void scene_style__apply_rules_tree(gui_node_t* root)
{
    if (root == NULL_POINTER)
    {
        return;
    }
    _scene_style_internal__apply_rules_recursive(root);
}

// ===== style resolution =====================================================

static void _scene_style_internal__resolve_recursive(gui_node_t* n)
{
    gui_style_t* base = &n->style[GUI_NODE_STATE__DEFAULT];
    gui_style_t* cur  = &n->style[n->state];

    n->resolved = *base;
    // Overlay per-state slot onto resolved. Was previously only
    // background_color; the rest of the fields below were being
    // silently ignored, which broke `:hover` / `:active` rules
    // that touched opacity, accent color, font color, gradient,
    // border, radius, etc.
    if (cur->has_background_color)
    {
        n->resolved.background_color     = cur->background_color;
        n->resolved.has_background_color = TRUE;
    }
    if (cur->has_bg_gradient)
    {
        n->resolved.bg_gradient     = cur->bg_gradient;
        n->resolved.has_bg_gradient = TRUE;
    }
    if (cur->has_opacity)
    {
        n->resolved.opacity     = cur->opacity;
        n->resolved.has_opacity = TRUE;
    }
    if (cur->has_font_color)
    {
        n->resolved.font_color     = cur->font_color;
        n->resolved.has_font_color = TRUE;
    }
    if (cur->has_accent_color)
    {
        n->resolved.accent_color     = cur->accent_color;
        n->resolved.has_accent_color = TRUE;
    }
    if (cur->has_border_color)
    {
        n->resolved.border_color     = cur->border_color;
        n->resolved.has_border_color = TRUE;
    }
    if (cur->border_width_set == TRUE)
    {
        n->resolved.border_width = cur->border_width;
    }
    if (cur->radius > 0.0f)
    {
        n->resolved.radius       = cur->radius;
    }
    // Box-model + sizing + typography overlays. Same "non-default
    // value wins" pattern used by the rule-overlay code in
    // _overlay_style. Lets `:hover { padding: 16px }` etc. actually
    // change the layout when the state matches.
    if (cur->pad_t      > 0.0f)
    {
        n->resolved.pad_t      = cur->pad_t;
    }
    if (cur->pad_r      > 0.0f)
    {
        n->resolved.pad_r      = cur->pad_r;
    }
    if (cur->pad_b      > 0.0f)
    {
        n->resolved.pad_b      = cur->pad_b;
    }
    if (cur->pad_l      > 0.0f)
    {
        n->resolved.pad_l      = cur->pad_l;
    }
    if (cur->margin_t   > 0.0f)
    {
        n->resolved.margin_t   = cur->margin_t;
    }
    if (cur->margin_r   > 0.0f)
    {
        n->resolved.margin_r   = cur->margin_r;
    }
    if (cur->margin_b   > 0.0f)
    {
        n->resolved.margin_b   = cur->margin_b;
    }
    if (cur->margin_l   > 0.0f)
    {
        n->resolved.margin_l   = cur->margin_l;
    }
    if (cur->gap        > 0.0f)
    {
        n->resolved.gap        = cur->gap;
    }
    if (cur->size_w     > 0.0f)
    {
        n->resolved.size_w     = cur->size_w;
    }
    if (cur->size_h     > 0.0f)
    {
        n->resolved.size_h     = cur->size_h;
    }
    if (cur->size_w_explicit)
    {
        n->resolved.size_w_explicit = TRUE;
    }
    if (cur->size_h_explicit)
    {
        n->resolved.size_h_explicit = TRUE;
    }
    if (cur->width_pct  > 0.0f)
    {
        n->resolved.width_pct  = cur->width_pct;
    }
    if (cur->height_pct > 0.0f)
    {
        n->resolved.height_pct = cur->height_pct;
    }
    if (cur->min_w      > 0.0f)
    {
        n->resolved.min_w      = cur->min_w;
    }
    if (cur->min_h      > 0.0f)
    {
        n->resolved.min_h      = cur->min_h;
    }
    if (cur->font_size  > 0.0f)
    {
        n->resolved.font_size  = cur->font_size;
    }
    if (cur->font_size_explicit)
    {
        n->resolved.font_size_explicit = TRUE;
    }
    if (cur->line_height != 0.0f)
    {
        n->resolved.line_height    = cur->line_height;
        n->resolved.line_height_px = 0.0f;
    }
    if (cur->line_height_px > 0.0f)
    {
        n->resolved.line_height_px = cur->line_height_px;
        n->resolved.line_height    = 0.0f;
    }
    if (cur->font_family[0] != 0)
    {
        stdlib__memcpy(n->resolved.font_family, cur->font_family, sizeof(n->resolved.font_family));
    }
    // Box-shadow. Only swap if the state slot has its own shadow
    // (has_shadow guard, mirrors background_color pattern).
    if (cur->has_shadow)
    {
        n->resolved.shadow_dx    = cur->shadow_dx;
        n->resolved.shadow_dy    = cur->shadow_dy;
        n->resolved.shadow_blur  = cur->shadow_blur;
        n->resolved.shadow_color = cur->shadow_color;
        n->resolved.has_shadow   = TRUE;
    }

    // CSS-style inheritance for typography. See original scene.c
    // comment: font family, font size, font color, and appear
    // animation inherit from parent. bg/radius/pad/gap etc. do not.
    if (n->parent != NULL_POINTER)
    {
        gui_style_t* p = &n->parent->resolved;
        if (n->resolved.font_family[0] == 0 && p->font_family[0] != 0)
        {
            stdlib__memcpy(n->resolved.font_family, p->font_family, sizeof(n->resolved.font_family));
        }
        if (n->resolved.font_size <= 0.0f && p->font_size > 0.0f)
        {
            n->resolved.font_size          = p->font_size;
            // Carry the explicit flag along with the inherited value
            // so the animator's transition gate (which requires both
            // endpoints to be rule-defined) keeps firing on a child
            // that inherits its size from a transitioning parent.
            n->resolved.font_size_explicit = p->font_size_explicit;
        }
        if (n->resolved.line_height == 0.0f && n->resolved.line_height_px <= 0.0f)
        {
            n->resolved.line_height    = p->line_height;
            n->resolved.line_height_px = p->line_height_px;
        }
        if (n->resolved.text_align == GUI_TEXT_ALIGN__LEFT && p->text_align != GUI_TEXT_ALIGN__LEFT)
        {
            n->resolved.text_align = p->text_align;
        }
        if (n->resolved.text_transform == GUI_TEXT_TRANSFORM__NONE && p->text_transform != GUI_TEXT_TRANSFORM__NONE)
        {
            n->resolved.text_transform = p->text_transform;
        }
        if (n->resolved.has_font_color == FALSE && p->has_font_color)
        {
            n->resolved.font_color     = p->font_color;
            n->resolved.has_font_color = TRUE;
        }
        if (n->resolved.appear_ms <= 0.0f && p->appear_ms > 0.0f)
        {
            n->resolved.appear_ms     = p->appear_ms;
            n->resolved.appear_easing = p->appear_easing;
            n->resolved.appear_easing_params[0] = p->appear_easing_params[0];
            n->resolved.appear_easing_params[1] = p->appear_easing_params[1];
            n->resolved.appear_easing_params[2] = p->appear_easing_params[2];
            n->resolved.appear_easing_params[3] = p->appear_easing_params[3];
        }
        if (n->resolved.has_appear_threshold == FALSE && p->has_appear_threshold)
        {
            n->resolved.appear_threshold     = p->appear_threshold;
            n->resolved.has_appear_threshold = TRUE;
        }
        if (n->resolved.has_disappear_threshold == FALSE && p->has_disappear_threshold)
        {
            n->resolved.disappear_threshold     = p->disappear_threshold;
            n->resolved.has_disappear_threshold = TRUE;
        }
    }

    // Host overrides. Applied AFTER inherited-style overlay so they
    // win regardless of what rules / animator wrote.
    if (n->has_background_color_override)
    {
        n->resolved.background_color     = n->background_color_override;
        n->resolved.has_background_color = TRUE;
    }
    if (n->has_font_color_override)
    {
        n->resolved.font_color     = n->font_color_override;
        n->resolved.has_font_color = TRUE;
    }
    if (n->has_background_image_override)
    {
        stdlib__memcpy(n->resolved.background_image, n->background_image_override, sizeof(n->resolved.background_image));
        n->resolved.background_image[sizeof(n->resolved.background_image) - 1] = 0;
        n->resolved.has_background_image = (n->background_image_override[0] != 0) ? TRUE : FALSE;
    }

    gui_node_t* c = n->first_child;
    while (c != NULL_POINTER)
    {
        _scene_style_internal__resolve_recursive(c);
        c = c->next_sibling;
    }
}

void scene_style__resolve_tree(gui_node_t* root)
{
    if (root == NULL_POINTER)
    {
        return;
    }
    _scene_style_internal__resolve_recursive(root);
}

/**
 * @brief host-side color overrides applied AFTER style resolve + animator each frame. Survive rule re-registration and hot reload (neither touches the override flags), unlike direct mutations to node->style[] which the resolver's memset wipes. Clear by explicitly calling scene__clear_background_color_override. Typical use: theme toggles that want to recolor specific nodes at runtime.
 *
 * @param gui_node_t* n -> Target node.
 * @param gui_color_t c -> Replaces both resolved.background_color and has_background_color.
 *
 * @return void
 */
void scene__set_background_color_override(gui_node_t* n, gui_color_t c)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    n->background_color_override     = c;
    n->has_background_color_override = TRUE;
}

/**
 * @brief scene__clear_background_color_override
 *
 * @return void
 */
void scene__clear_background_color_override(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    n->has_background_color_override = FALSE;
}

/**
 * @brief background-image override. Same story as the color overrides: applied AFTER style resolve + animator, so it sticks across `.style` reloads and rule re-evaluation. Pass an empty / NULL `name` to clear (or call scene__clear_background_image_override). Typical use: a settings-app changes the wallpaper filename and meek-shell calls this on its window node every tick to apply the new selection without re-emitting `.style` rules.
 *
 * @param gui_node_t* n -> Target node.
 * @param char* name -> Image filename (resolved against the asset dir at render time).
 *
 * @return void
 */
void scene__set_background_image_override(gui_node_t* n, char* name)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    stdlib__memset(n->background_image_override, 0, sizeof(n->background_image_override));
    if (name != NULL_POINTER && name[0] != 0)
    {
        size_t n_bytes = stdlib__strlen(name);
        if (n_bytes >= sizeof(n->background_image_override))
        {
            n_bytes = sizeof(n->background_image_override) - 1;
        }
        stdlib__memcpy(n->background_image_override, name, n_bytes);
        n->background_image_override[n_bytes] = 0;
    }
    n->has_background_image_override = TRUE;
}

/**
 * @brief scene__clear_background_image_override
 *
 * @return void
 */
void scene__clear_background_image_override(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    n->has_background_image_override = FALSE;
    n->background_image_override[0]  = 0;
}

/**
 * @brief scene__set_font_color_override
 *
 * @return void
 */
void scene__set_font_color_override(gui_node_t* n, gui_color_t c)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    n->font_color_override     = c;
    n->has_font_color_override = TRUE;
}

/**
 * @brief scene__clear_font_color_override
 *
 * @return void
 */
void scene__clear_font_color_override(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    n->has_font_color_override = FALSE;
}
