// Retained navigation with ordinary div/button/label nodes for its styled bar.
// All allocations use scene nodes or the tracked clib widget-state allocation.
#include "widget_navigation.h"
#include "clib/stdlib.h"
#include "gui.h"
#include "motion.h"
#include "renderer.h"
#include "scene.h"
#include "third_party/log.h"
#include "types.h"
#include "widget.h"
#include "widget_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAV_DEPTH 16
#define NAV_OPTIONS 9

typedef struct _widget_navigation_internal__state_t
{
    char stack[NAV_DEPTH][64];
    int count;
    char initial[64];
    char outgoing[64];
    char base[NAV_OPTIONS][128];
    char overrides[NAV_OPTIONS][128];
    char effective[NAV_OPTIONS][128];
    boole overridden[NAV_OPTIONS];
    boole initialized;
    boole seeded;
    boole dirty;
    boole transitioning;
    boole pending_exit;
    boole reverse;
    float elapsed;
    float duration;
    float progress;
    gui_node_t* bar;
    gui_rect_t content;
} _widget_navigation_internal__state_t;

static const char* _widget_navigation_internal__names[NAV_OPTIONS] = {"mode", "transition", "duration", "easing", "direction", "placement", "separator", "trigger-disappear", "back-label"};
static const char* _widget_navigation_internal__defaults[NAV_OPTIONS] = {"none", "instant", "250ms", "ease-out", "left", "top", ">", "false", "Back"};

static gui_node_t* _widget_navigation_internal__owner(gui_node_t* n)
{
    while (n != NULL_POINTER && n->type != GUI_NODE_TYPE__NAVIGATION)
    {
        n = n->parent;
    }
    return n;
}

static _widget_navigation_internal__state_t* _widget_navigation_internal__state(gui_node_t* n)
{
    _widget_navigation_internal__state_t* s = scene__widget_state(n);
    if (s != NULL_POINTER && s->initialized == FALSE)
    {
        for (int i = 0; i < NAV_OPTIONS; i++)
        {
            stdlib__strcpy_safe(s->base[i], sizeof(s->base[i]), _widget_navigation_internal__defaults[i]);
            stdlib__strcpy_safe(s->effective[i], sizeof(s->effective[i]), s->base[i]);
        }
        s->initialized = TRUE;
        s->dirty = TRUE;
    }
    return s;
}

static int _widget_navigation_internal__option(const char* name)
{
    if (name == NULL_POINTER)
    {
        return -1;
    }
    if (stdlib__strncmp(name, "navigation-", 11) == 0)
    {
        name += 11;
    }
    for (int i = 0; i < NAV_OPTIONS; i++)
    {
        if (stdlib__strcmp(name, _widget_navigation_internal__names[i]) == 0)
        {
            return i;
        }
    }
    return -1;
}

static boole _widget_navigation_internal__valid(int option, const char* value)
{
    if (value == NULL_POINTER || stdlib__strlen(value) >= 128)
    {
        return FALSE;
    }
    switch (option)
    {
    case 0:
        return !stdlib__strcmp(value, "none") || !stdlib__strcmp(value, "breadcrumbs") || !stdlib__strcmp(value, "title");
    case 1:
        return !stdlib__strcmp(value, "instant") || !stdlib__strcmp(value, "slide") || !stdlib__strcmp(value, "push") || !stdlib__strcmp(value, "fade");
    case 2:
    {
        char* end = NULL_POINTER;
        float duration = stdlib__strtof(value, &end);
        if (!stdlib__strcmp(end, "s"))
        {
            duration *= 1000;
        }
        return end != value && duration >= 0 && duration <= 60000 && (*end == 0 || !stdlib__strcmp(end, "ms") || !stdlib__strcmp(end, "s"));
    }
    case 3:
        return !stdlib__strcmp(value, "linear") || !stdlib__strcmp(value, "ease-out") || !stdlib__strcmp(value, "ease-in") || !stdlib__strcmp(value, "ease-in-out");
    case 4:
        return !stdlib__strcmp(value, "left") || !stdlib__strcmp(value, "right") || !stdlib__strcmp(value, "up") || !stdlib__strcmp(value, "down");
    case 5:
        return !stdlib__strcmp(value, "top") || !stdlib__strcmp(value, "bottom") || !stdlib__strcmp(value, "left") || !stdlib__strcmp(value, "right");
    case 7:
        return !stdlib__strcmp(value, "true") || !stdlib__strcmp(value, "false") || !stdlib__strcmp(value, "1") || !stdlib__strcmp(value, "0");
    default:
        return TRUE;
    }
}

static gui_node_t* _widget_navigation_internal__page(gui_node_t* nav, const char* id)
{
    if (nav == NULL_POINTER || id == NULL_POINTER || id[0] == 0)
    {
        return NULL_POINTER;
    }
    for (gui_node_t* page = nav->first_child; page != NULL_POINTER; page = page->next_sibling)
    {
        if (!page->navigation_chrome && !stdlib__strcmp(page->id, id))
        {
            return page;
        }
    }
    return NULL_POINTER;
}

static void _widget_navigation_internal__lifecycle(gui_node_t* n, boole active)
{
    n->navigation_inactive = !active;
    n->navigation_suppress_disappear = !active;
    n->disappear_remaining_ms = 0;
    n->navigation_effect = FALSE;
    n->navigation_x = 0;
    n->navigation_y = 0;
    if (active)
    {
        n->appear_age_ms = 0;
        n->prev_display = GUI_DISPLAY__NONE;
        n->view_tracked = FALSE;
        n->scrolled_out = FALSE;
        motion__forget(n);
    }
    for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _widget_navigation_internal__lifecycle(c, active);
    }
}

static void _widget_navigation_internal__seed(gui_node_t* nav, _widget_navigation_internal__state_t* s)
{
    if (s->seeded)
    {
        return;
    }
    gui_node_t* page = _widget_navigation_internal__page(nav, s->initial);
    if (page == NULL_POINTER)
    {
        for (page = nav->first_child; page != NULL_POINTER && (page->navigation_chrome || !page->id[0]); page = page->next_sibling)
        {
        }
    }
    if (page != NULL_POINTER)
    {
        stdlib__strcpy_safe(s->stack[0], sizeof(s->stack[0]), page->id);
        s->count = 1;
    }
    for (gui_node_t* c = nav->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (!c->navigation_chrome)
        {
            _widget_navigation_internal__lifecycle(c, c == page);
        }
    }
    s->seeded = TRUE;
}

static void _widget_navigation_internal__options(gui_node_t* nav, _widget_navigation_internal__state_t* s)
{
    char key[80];
    char value[128];
    for (int i = 0; i < NAV_OPTIONS; i++)
    {
        stdlib__strcpy_safe(value, sizeof(value), s->base[i]);
        stdlib__strcpy_safe(key, sizeof(key), "navigation-");
        stdlib__strcat_safe(key, sizeof(key), _widget_navigation_internal__names[i]);
        char css[128] = {0};
        if (motion__value(nav, key, css, sizeof(css)))
        {
            size_t length = stdlib__strlen(css);
            if (length >= 2 && ((css[0] == '"' && css[length - 1] == '"') || (css[0] == '\'' && css[length - 1] == '\'')))
            {
                stdlib__memmove(css, css + 1, length - 2);
                css[length - 2] = 0;
            }
            if (_widget_navigation_internal__valid(i, css))
            {
                stdlib__strcpy_safe(value, sizeof(value), css);
            }
        }
        if (s->overridden[i])
        {
            stdlib__strcpy_safe(value, sizeof(value), s->overrides[i]);
        }
        if (stdlib__strcmp(value, s->effective[i]))
        {
            stdlib__strcpy_safe(s->effective[i], sizeof(s->effective[i]), value);
            s->dirty = TRUE;
        }
    }
}

static void _widget_navigation_internal__item_clicked(gui_event_t* ev)
{
    if (ev->sender->navigation_parent[0])
    {
        scene__navigation_go(ev->sender, ev->sender->navigation_parent);
    }
    else
    {
        scene__navigation_pop(ev->sender);
    }
}

static gui_node_t* _widget_navigation_internal__item(gui_node_t* bar, gui_node_type_e type, const char* klass, const char* text, const char* target)
{
    gui_node_t* n = scene__node_new(type);
    if (n == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    n->navigation_chrome = TRUE;
    stdlib__strcpy_safe(n->klass, sizeof(n->klass), klass);
    if (scene__set_text(n, text, (int64)stdlib__strlen(text)) == FALSE)
    {
        scene__node_free(n);
        return NULL_POINTER;
    }
    if (target != NULL_POINTER)
    {
        stdlib__strcpy_safe(n->navigation_parent, sizeof(n->navigation_parent), target);
        scene__set_on_click(n, "__meek_navigation_item");
    }
    scene__add_child(bar, n);
    return n;
}

static void _widget_navigation_internal__build_bar(gui_node_t* nav, _widget_navigation_internal__state_t* s)
{
    gui_node_t* previous = NULL_POINTER;
    gui_node_t* old = nav->first_child;
    while (old != NULL_POINTER)
    {
        gui_node_t* next = old->next_sibling;
        if (old->navigation_chrome)
        {
            if (previous != NULL_POINTER)
            {
                previous->next_sibling = next;
            }
            else
            {
                nav->first_child = next;
            }
            if (nav->last_child == old)
            {
                nav->last_child = previous;
            }
            nav->child_count--;
            old->parent = NULL_POINTER;
            old->next_sibling = NULL_POINTER;
            scene__node_free(old);
        }
        else
        {
            previous = old;
        }
        old = next;
    }
    s->bar = NULL_POINTER;
    s->dirty = FALSE;
    if (!stdlib__strcmp(s->effective[0], "none") || !s->count)
    {
        return;
    }
    gui_node_t* bar = scene__node_new(GUI_NODE_TYPE__DIV);
    if (bar == NULL_POINTER)
    {
        s->dirty = TRUE;
        return;
    }
    bar->navigation_chrome = TRUE;
    stdlib__strcpy_safe(bar->klass, sizeof(bar->klass), "navigation-bar");
    scene__add_child(nav, bar);
    s->bar = bar;
    gui_node_t* active = _widget_navigation_internal__page(nav, s->stack[s->count - 1]);
    if (s->count > 1)
    {
        _widget_navigation_internal__item(bar, GUI_NODE_TYPE__BUTTON, "navigation-back", s->effective[8], "");
    }
    gui_node_t* path[NAV_DEPTH];
    int length = 0;
    gui_node_t* cursor = active;
    while (cursor != NULL_POINTER && length < NAV_DEPTH)
    {
        boole repeated = FALSE;
        for (int i = 0; i < length; i++)
        {
            repeated |= path[i] == cursor;
        }
        if (repeated)
        {
            break;
        }
        path[length++] = cursor;
        if (!stdlib__strcmp(s->effective[0], "title"))
        {
            break;
        }
        cursor = _widget_navigation_internal__page(nav, cursor->navigation_parent);
    }
    for (int i = length - 1; i >= 0; i--)
    {
        gui_node_t* page = path[i];
        const char* label = page->navigation_title[0] ? page->navigation_title : page->id;
        const char* klass = i == 0 ? "navigation-item navigation-current" : "navigation-item";
        if (page->navigation_parent[0] == 0)
        {
            klass = i == 0 ? "navigation-item navigation-home navigation-current" : "navigation-item navigation-home";
        }
        _widget_navigation_internal__item(bar, GUI_NODE_TYPE__BUTTON, klass, label, page->id);
        if (i > 0)
        {
            _widget_navigation_internal__item(bar, GUI_NODE_TYPE__TEXT, "navigation-separator", s->effective[6], NULL_POINTER);
        }
    }
}

static void _widget_navigation_internal__finish(gui_node_t* nav, _widget_navigation_internal__state_t* s)
{
    for (gui_node_t* p = nav->first_child; p != NULL_POINTER; p = p->next_sibling)
    {
        if (!p->navigation_chrome)
        {
            p->navigation_effect = FALSE;
            p->navigation_x = p->navigation_y = 0;
        }
    }
    s->outgoing[0] = 0;
    s->transitioning = FALSE;
    s->pending_exit = FALSE;
}

static void _widget_navigation_internal__switch(gui_node_t* nav, _widget_navigation_internal__state_t* s, const char* previous, boole reverse)
{
    _widget_navigation_internal__finish(nav, s);
    gui_node_t* from = _widget_navigation_internal__page(nav, previous);
    gui_node_t* to = _widget_navigation_internal__page(nav, s->stack[s->count - 1]);
    if (from != NULL_POINTER)
    {
        gui_node_t* focus = scene__focus();
        gui_node_t* parent = focus;
        while (parent != NULL_POINTER && parent != from)
        {
            parent = parent->parent;
        }
        if (parent == from)
        {
            stdlib__strcpy_safe(from->navigation_focus, sizeof(from->navigation_focus), focus->id);
            scene__set_focus(NULL_POINTER);
        }
        _widget_navigation_internal__lifecycle(from, FALSE);
        stdlib__strcpy_safe(s->outgoing, sizeof(s->outgoing), from->id);
    }
    if (to != NULL_POINTER)
    {
        _widget_navigation_internal__lifecycle(to, TRUE);
        gui_node_t* restored = to->navigation_focus[0] ? scene__find_by_id(to->navigation_focus) : NULL_POINTER;
        gui_node_t* parent = restored;
        while (parent != NULL_POINTER && parent != to)
        {
            parent = parent->parent;
        }
        if (parent == to)
        {
            scene__set_focus(restored);
        }
    }
    char* end = NULL_POINTER;
    s->duration = stdlib__strtof(s->effective[2], &end);
    if (end != NULL_POINTER && !stdlib__strcmp(end, "s"))
    {
        s->duration *= 1000;
    }
    if (!stdlib__strcmp(s->effective[1], "instant") || motion__reduced_motion())
    {
        s->duration = 0;
    }
    s->elapsed = 0;
    s->progress = 0;
    s->reverse = reverse;
    s->pending_exit = from != NULL_POINTER;
    s->transitioning = from != NULL_POINTER;
    s->dirty = TRUE;
}

static float _widget_navigation_internal__exit(gui_node_t* n)
{
    gui_style_t* d = &n->style[GUI_NODE_STATE__DISAPPEAR];
    float duration = 0;
    if (d->animation_duration_ms > 0 || d->has_background_color || d->has_font_color || d->has_accent_color || d->radius > 0)
    {
        duration = d->animation_duration_ms > 0 ? d->animation_duration_ms : 200;
        n->disappear_remaining_ms = duration;
    }
    for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        float child = _widget_navigation_internal__exit(c);
        if (child > duration)
        {
            duration = child;
        }
    }
    return duration;
}

static void _widget_navigation_internal__layout(gui_node_t* nav, float x, float y, float aw, float ah, float scale)
{
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s == NULL_POINTER)
    {
        return;
    }
    _widget_navigation_internal__seed(nav, s);
    gui_style_t* style = &nav->resolved;
    float w = scene__layout_width(nav, aw, scale);
    float h = style->size_h > 0 ? style->size_h * scale : style->height_pct > 0 ? ah * style->height_pct / 100
                                                                                : ah;
    float border = scene__border_width(nav, scale);
    nav->bounds = (gui_rect_t){x, y, w, h};
    gui_rect_t content = {x + style->pad_l * scale + border, y + style->pad_t * scale + border, w - (style->pad_l + style->pad_r) * scale - 2 * border, h - (style->pad_t + style->pad_b) * scale - 2 * border};
    if (content.w < 0)
    {
        content.w = 0;
    }
    if (content.h < 0)
    {
        content.h = 0;
    }
    if (s->bar != NULL_POINTER)
    {
        gui_node_t* bar = s->bar;
        boole vertical = !stdlib__strcmp(s->effective[5], "left") || !stdlib__strcmp(s->effective[5], "right");
        float size = vertical ? (bar->resolved.size_w > 0 ? bar->resolved.size_w : 160) * scale : (bar->resolved.size_h > 0 ? bar->resolved.size_h : 44) * scale;
        float limit = vertical ? content.w : content.h;
        if (size > limit)
        {
            size = limit;
        }
        gui_rect_t area = content;
        if (vertical)
        {
            area.w = size;
            if (!stdlib__strcmp(s->effective[5], "right"))
            {
                area.x += content.w - size;
            }
            else
            {
                content.x += size;
            }
            content.w -= size;
        }
        else
        {
            area.h = size;
            if (!stdlib__strcmp(s->effective[5], "bottom"))
            {
                area.y += content.h - size;
            }
            else
            {
                content.y += size;
            }
            content.h -= size;
        }
        bar->resolved.flex_direction = vertical ? 2 : 1;
        bar->resolved.display = GUI_DISPLAY__FLEX;
        bar->resolved.size_w = area.w / scale;
        bar->resolved.size_h = area.h / scale;
        bar->resolved.width_pct = bar->resolved.height_pct = 0;
        bar->resolved.overflow_x = GUI_OVERFLOW__AUTO;
        bar->resolved.overflow_y = GUI_OVERFLOW__AUTO;
        scene__layout_node(bar, area.x, area.y, area.w, area.h);
    }
    s->content = content;
    for (gui_node_t* page = nav->first_child; page != NULL_POINTER; page = page->next_sibling)
    {
        if (page->navigation_chrome || page->resolved.display == GUI_DISPLAY__NONE)
        {
            continue;
        }
        scene__layout_node(page, content.x, content.y, content.w, content.h);
        page->navigation_effect = s->transitioning;
        page->navigation_alpha = 1;
        page->navigation_x = page->navigation_y = 0;
        if (s->transitioning)
        {
            boole incoming = !stdlib__strcmp(page->id, s->stack[s->count - 1]);
            float t = s->progress;
            if (!stdlib__strcmp(s->effective[1], "fade"))
            {
                page->navigation_alpha = incoming ? t : 1 - t;
            }
            else if (stdlib__strcmp(s->effective[1], "instant"))
            {
                float factor = incoming ? 1 - t : -t;
                if (!stdlib__strcmp(s->effective[1], "slide") && !incoming)
                {
                    factor *= 0.25f;
                }
                if (s->reverse)
                {
                    factor = -factor;
                }
                if (!stdlib__strcmp(s->effective[4], "right") || !stdlib__strcmp(s->effective[4], "down"))
                {
                    factor = -factor;
                }
                if (!stdlib__strcmp(s->effective[4], "up") || !stdlib__strcmp(s->effective[4], "down"))
                {
                    page->navigation_y = factor * content.h;
                }
                else
                {
                    page->navigation_x = factor * content.w;
                }
            }
        }
    }
}

static void _widget_navigation_internal__draw(gui_node_t* nav, float scale)
{
    scene__emit_default_bg(nav, scale);
    renderer__push_scissor(nav->bounds);
}

static void _widget_navigation_internal__draw_post(gui_node_t* nav, float scale)
{
    (void)nav;
    (void)scale;
    renderer__pop_scissor();
}

static boole _widget_navigation_internal__attribute(gui_node_t* nav, char* name, char* value)
{
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s == NULL_POINTER)
    {
        return FALSE;
    }
    if (!stdlib__strcmp(name, "initial"))
    {
        stdlib__strcpy_safe(s->initial, sizeof(s->initial), value);
        return TRUE;
    }
    int index = _widget_navigation_internal__option(name);
    if (index < 0 || !_widget_navigation_internal__valid(index, value))
    {
        return FALSE;
    }
    stdlib__strcpy_safe(s->base[index], sizeof(s->base[index]), value);
    s->dirty = TRUE;
    return TRUE;
}

static const widget_vtable_t _widget_navigation_internal__vtable = {.type_name = "navigation", .apply_attribute = _widget_navigation_internal__attribute, .layout = _widget_navigation_internal__layout, .emit_draws = _widget_navigation_internal__draw, .emit_draws_post = _widget_navigation_internal__draw_post, .state_size = sizeof(_widget_navigation_internal__state_t), .preserve_user_data = FALSE};

void widget_navigation__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__NAVIGATION, &_widget_navigation_internal__vtable);
    scene__register_handler("__meek_navigation_item", _widget_navigation_internal__item_clicked);
}

void widget_navigation__reconcile(gui_node_t* old, gui_node_t* current)
{
    _widget_navigation_internal__state_t* source = _widget_navigation_internal__state(old);
    _widget_navigation_internal__state_t* dest = _widget_navigation_internal__state(current);
    if (source == NULL_POINTER || dest == NULL_POINTER)
    {
        return;
    }
    // Keep new declarative attributes, retain runtime overrides/history by id.
    stdlib__memcpy(dest->overrides, source->overrides, sizeof(dest->overrides));
    stdlib__memcpy(dest->overridden, source->overridden, sizeof(dest->overridden));
    dest->count = 0;
    for (int i = 0; i < source->count; i++)
    {
        if (_widget_navigation_internal__page(current, source->stack[i]))
        {
            stdlib__strcpy_safe(dest->stack[dest->count++], 64, source->stack[i]);
        }
    }
    dest->seeded = dest->count > 0;
    dest->dirty = TRUE;
    if (!dest->seeded)
    {
        _widget_navigation_internal__seed(current, dest);
    }
    for (gui_node_t* p = current->first_child; p; p = p->next_sibling)
    {
        if (!p->navigation_chrome)
        {
            _widget_navigation_internal__lifecycle(p, dest->count > 0 && !stdlib__strcmp(p->id, dest->stack[dest->count - 1]));
        }
    }
}

void widget_navigation__prepare_tree(gui_node_t* root)
{
    if (root == NULL_POINTER)
    {
        return;
    }
    if (root->type == GUI_NODE_TYPE__NAVIGATION)
    {
        _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(root);
        if (s != NULL_POINTER)
        {
            _widget_navigation_internal__seed(root, s);
            _widget_navigation_internal__options(root, s);
            // A reconciled node retains the history state, but gets new children.
            gui_node_t* bar = NULL_POINTER;
            for (gui_node_t* c = root->first_child; c != NULL_POINTER; c = c->next_sibling)
            {
                if (c->navigation_chrome)
                {
                    bar = c;
                }
            }
            if (s->bar != bar)
            {
                s->bar = bar;
                s->dirty = TRUE;
            }
            if (s->dirty)
            {
                _widget_navigation_internal__build_bar(root, s);
            }
        }
    }
    for (gui_node_t* c = root->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        widget_navigation__prepare_tree(c);
    }
}

void widget_navigation__tick_tree(gui_node_t* root)
{
    if (root == NULL_POINTER || root->navigation_inactive)
    {
        return;
    }
    if (root->type == GUI_NODE_TYPE__NAVIGATION)
    {
        _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(root);
        if (s != NULL_POINTER)
        {
            if (s->pending_exit)
            {
                gui_node_t* out = _widget_navigation_internal__page(root, s->outgoing);
                if (out != NULL_POINTER && (!stdlib__strcmp(s->effective[7], "true") || !stdlib__strcmp(s->effective[7], "1")))
                {
                    float duration = _widget_navigation_internal__exit(out);
                    if (duration > s->duration)
                    {
                        s->duration = duration;
                    }
                }
                s->pending_exit = FALSE;
            }
            else if (s->transitioning)
            {
                s->elapsed += (float)scene__frame_delta_ms();
            }
            if (s->transitioning && (s->duration <= 0 || s->elapsed >= s->duration))
            {
                _widget_navigation_internal__finish(root, s);
            }
            float t = s->duration > 0 ? s->elapsed / s->duration : 1;
            if (t > 1)
            {
                t = 1;
            }
            if (!stdlib__strcmp(s->effective[3], "ease-out"))
            {
                t = 1 - (1 - t) * (1 - t) * (1 - t);
            }
            else if (!stdlib__strcmp(s->effective[3], "ease-in"))
            {
                t = t * t * t;
            }
            else if (!stdlib__strcmp(s->effective[3], "ease-in-out"))
            {
                t = t * t * (3 - 2 * t);
            }
            s->progress = t;
            for (gui_node_t* page = root->first_child; page != NULL_POINTER; page = page->next_sibling)
            {
                if (page->navigation_chrome)
                {
                    continue;
                }
                boole active = s->count > 0 && !stdlib__strcmp(page->id, s->stack[s->count - 1]);
                boole outgoing = s->transitioning && !stdlib__strcmp(page->id, s->outgoing);
                page->navigation_inactive = !active;
                page->navigation_suppress_disappear = !active;
                page->style[GUI_NODE_STATE__DEFAULT].display = page->resolved.display = (active || outgoing) ? GUI_DISPLAY__BLOCK : GUI_DISPLAY__NONE;
            }
        }
    }
    for (gui_node_t* c = root->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        widget_navigation__tick_tree(c);
    }
}

boole widget_navigation__active(gui_node_t* root)
{
    if (root == NULL_POINTER || root->navigation_inactive)
    {
        return FALSE;
    }
    if (root->type == GUI_NODE_TYPE__NAVIGATION && root->user_data != NULL_POINTER)
    {
        _widget_navigation_internal__state_t* s = root->user_data;
        if (s->transitioning)
        {
            return TRUE;
        }
    }
    for (gui_node_t* c = root->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        if (widget_navigation__active(c))
        {
            return TRUE;
        }
    }
    return FALSE;
}

void scene__navigation_push(gui_node_t* anchor, char* id)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    if (_widget_navigation_internal__page(nav, id) == NULL_POINTER)
    {
        return;
    }
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s == NULL_POINTER)
    {
        return;
    }
    _widget_navigation_internal__seed(nav, s);
    _widget_navigation_internal__options(nav, s);
    if (s->count >= NAV_DEPTH || (s->count > 0 && !stdlib__strcmp(s->stack[s->count - 1], id)))
    {
        return;
    }
    char previous[64] = {0};
    if (s->count)
    {
        stdlib__strcpy_safe(previous, sizeof(previous), s->stack[s->count - 1]);
    }
    stdlib__strcpy_safe(s->stack[s->count++], 64, id);
    _widget_navigation_internal__switch(nav, s, previous, FALSE);
}

void widget_navigation__remember_focus(gui_node_t* node)
{
    if (node == NULL_POINTER || node->id[0] == 0)
    {
        return;
    }
    gui_node_t* page = node;
    while (page->parent != NULL_POINTER && page->parent->type != GUI_NODE_TYPE__NAVIGATION)
    {
        page = page->parent;
    }
    if (page->parent != NULL_POINTER && page->navigation_chrome == FALSE)
    {
        stdlib__strcpy_safe(page->navigation_focus, sizeof(page->navigation_focus), node->id);
    }
}

void scene__navigation_pop(gui_node_t* anchor)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    if (nav == NULL_POINTER)
    {
        return;
    }
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s == NULL_POINTER)
    {
        return;
    }
    _widget_navigation_internal__seed(nav, s);
    _widget_navigation_internal__options(nav, s);
    if (s->count <= 1)
    {
        return;
    }
    char previous[64];
    stdlib__strcpy_safe(previous, sizeof(previous), s->stack[--s->count]);
    _widget_navigation_internal__switch(nav, s, previous, TRUE);
}

char* scene__navigation_peek(gui_node_t* anchor)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    if (nav == NULL_POINTER)
    {
        return "";
    }
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    return s != NULL_POINTER && s->count ? s->stack[s->count - 1] : "";
}

int64 scene__navigation_depth(gui_node_t* anchor)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    if (nav == NULL_POINTER)
    {
        return 0;
    }
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    return s != NULL_POINTER ? s->count : 0;
}

boole scene__navigation_go(gui_node_t* anchor, const char* id)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    if (_widget_navigation_internal__page(nav, id) == NULL_POINTER)
    {
        return FALSE;
    }
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s == NULL_POINTER)
    {
        return FALSE;
    }
    _widget_navigation_internal__seed(nav, s);
    _widget_navigation_internal__options(nav, s);
    if (s->count && !stdlib__strcmp(s->stack[s->count - 1], id))
    {
        return TRUE;
    }
    for (int i = s->count - 2; i >= 0; i--)
    {
        if (!stdlib__strcmp(s->stack[i], id))
        {
            char previous[64];
            stdlib__strcpy_safe(previous, sizeof(previous), s->stack[s->count - 1]);
            s->count = i + 1;
            _widget_navigation_internal__switch(nav, s, previous, TRUE);
            return TRUE;
        }
    }
    if (s->count >= NAV_DEPTH)
    {
        return FALSE;
    }
    scene__navigation_push(nav, (char*)id);
    return !stdlib__strcmp(scene__navigation_peek(nav), id);
}

boole scene__navigation_set_option(gui_node_t* anchor, const char* name, const char* value)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    int index = _widget_navigation_internal__option(name);
    if (nav == NULL_POINTER || index < 0 || (value != NULL_POINTER && !_widget_navigation_internal__valid(index, value)))
    {
        return FALSE;
    }
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s == NULL_POINTER)
    {
        return FALSE;
    }
    s->overridden[index] = value != NULL_POINTER;
    if (value != NULL_POINTER)
    {
        stdlib__strcpy_safe(s->overrides[index], sizeof(s->overrides[index]), value);
    }
    _widget_navigation_internal__options(nav, s);
    return TRUE;
}

boole scene__navigation_get_option(gui_node_t* anchor, const char* name, char* out, int64 capacity)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    int index = _widget_navigation_internal__option(name);
    if (nav == NULL_POINTER || index < 0 || out == NULL_POINTER || capacity <= 0)
    {
        return FALSE;
    }
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s == NULL_POINTER)
    {
        return FALSE;
    }
    _widget_navigation_internal__options(nav, s);
    if ((int64)stdlib__strlen(s->effective[index]) >= capacity)
    {
        return FALSE;
    }
    stdlib__strcpy_safe(out, capacity, s->effective[index]);
    return TRUE;
}

boole scene__navigation_set_page(gui_node_t* anchor, const char* id, const char* title, const char* parent_id)
{
    gui_node_t* nav = _widget_navigation_internal__owner(anchor);
    gui_node_t* page = _widget_navigation_internal__page(nav, id);
    if (page == NULL_POINTER || title == NULL_POINTER || stdlib__strlen(title) >= sizeof(page->navigation_title))
    {
        return FALSE;
    }
    if (parent_id != NULL_POINTER && parent_id[0])
    {
        gui_node_t* parent = _widget_navigation_internal__page(nav, parent_id);
        if (parent == NULL_POINTER)
        {
            return FALSE;
        }
        for (int depth = 0; parent != NULL_POINTER; depth++)
        {
            if (parent == page || depth >= NAV_DEPTH)
            {
                return FALSE;
            }
            parent = _widget_navigation_internal__page(nav, parent->navigation_parent);
        }
    }
    stdlib__strcpy_safe(page->navigation_title, sizeof(page->navigation_title), title);
    stdlib__strcpy_safe(page->navigation_parent, sizeof(page->navigation_parent), parent_id != NULL_POINTER ? parent_id : "");
    _widget_navigation_internal__state_t* s = _widget_navigation_internal__state(nav);
    if (s != NULL_POINTER)
    {
        s->dirty = TRUE;
    }
    return TRUE;
}
