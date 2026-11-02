// motion.c - CSS value cascade, custom properties and keyframe timelines.
// Nodes keep their existing widget contracts; motion changes resolved styles.
#include "motion.h"
#include "scene.h"
#include "font.h"
#include "widget_registry.h"
#include "parser_style.h"
#include "clib/stdlib.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"

#define _MOTION_INTERNAL__VALUE_CAP 1024
#define _MOTION_INTERNAL__MAX_VALUES 48
#define _MOTION_INTERNAL__MAX_ANIMATIONS 8
#define _MOTION_INTERNAL__STATE_BUCKETS 2048
#define _MOTION_INTERNAL__MATCH_RULE_CAP 1024
#define _MOTION_INTERNAL__MATCH_NODE_CAP 8192

typedef struct _motion_internal__decl_t
{
    char name[48];                        // 0x0
    char value[_MOTION_INTERNAL__VALUE_CAP]; // 0x30
    struct _motion_internal__decl_t* next; // 0x430
    boole inherits;                       // 0x438, registered custom properties only
    int id;                               // 0x43C, motion__prop_id(name), assigned at parse time
} _motion_internal__decl_t;

typedef struct _motion_internal__rule_t
{
    char selector[256];                   // 0x0
    _motion_internal__decl_t* declarations; // 0x100
    struct _motion_internal__rule_t* next; // 0x108
    int specificity;                     // 0x110
    int index;                           // 0x114
} _motion_internal__rule_t;

typedef struct _motion_internal__selector_key_t
{
    gui_node_t* node; // 0x0
    gui_node_t* parent; // 0x8
    gui_node_t* first_child; // 0x10
    gui_node_t* next_sibling; // 0x18
    boole checked;
    gui_node_state_e state; // 0x20
    gui_node_type_e type; // 0x24
    char id[64]; // 0x28
    char klass[64]; // 0x68
    char name[256]; // 0xA8
} _motion_internal__selector_key_t;

typedef struct _motion_internal__match_cache_t
{
    _motion_internal__selector_key_t key; // 0x0
    uint64 generation; // 0x1A8
    uint64 matches[_MOTION_INTERNAL__MATCH_RULE_CAP / 64]; // 0x1B0
    // the tree shape of the last snapshot (entries are in pre-order): a
    // state change re-matches this entry, its ancestors and its subtree
    int parent_index;
    int subtree;
    boole stale;
} _motion_internal__match_cache_t;

typedef struct _motion_internal__frame_t
{
    char name[64];                         // 0x0
    float offset;                         // 0x40
    _motion_internal__decl_t* declarations; // 0x48
    struct _motion_internal__frame_t* next; // 0x50
} _motion_internal__frame_t;

typedef struct _motion_internal__value_t
{
    int id;                                // 0x0
    char value[_MOTION_INTERNAL__VALUE_CAP]; // 0x4
} _motion_internal__value_t;

typedef struct _motion_internal__state_t
{
    gui_node_t* node;                      // 0x0
    struct _motion_internal__state_t* next; // 0x8
    float age;                            // 0x10
    int count;                            // 0x14
    // slot[id] is the index + 1 of the value with that known id, 0 when
    // absent: a has / value query is one array read instead of a scan
    ubyte slot[64];                       // 0x18
    _motion_internal__value_t values[_MOTION_INTERNAL__MAX_VALUES]; // 0x58
    struct _motion_internal__state_t* hash_next;
} _motion_internal__state_t;

// interned property names: index = id; the known ones sit at their
// enum values, custom names follow as the stylesheet introduces them
static char _motion_internal__prop_names[MOTION_PROP__CAP][48];
static boole _motion_internal__prop_custom[MOTION_PROP__CAP];
static int _motion_internal__prop_count = 0;
static const char* _motion_internal__known_names[MOTION_PROP__KNOWN_COUNT] = { "transform", "transform-origin", "filter", "backdrop-filter", "mask", "mask-image", "mask-size", "mask-position", "mask-repeat", "mask-composite", "clip-path", "border-radius", "mix-blend-mode", "background", "background-image", "background-size", "background-position", "background-repeat", "background-clip", "-webkit-background-clip", "background-color", "color", "font-color", "opacity", "width", "height", "top", "left", "border-color", "text-shadow", "-webkit-text-stroke", "pointer-events", "letter-spacing", "font-variation-settings", "animation", "animation-name", "animation-duration", "animation-delay", "animation-direction", "animation-iteration-count", "animation-fill-mode", "animation-timing-function", "animation-timeline", "animation-range" };

static _motion_internal__rule_t* _motion_internal__rules = NULL_POINTER;
static _motion_internal__frame_t* _motion_internal__frames = NULL_POINTER;
static _motion_internal__state_t* _motion_internal__states = NULL_POINTER;
static _motion_internal__state_t* _motion_internal__state_buckets[_MOTION_INTERNAL__STATE_BUCKETS];
static _motion_internal__decl_t* _motion_internal__defaults = NULL_POINTER;
static boole _motion_internal__reduce = FALSE;
static boole _motion_internal__active = FALSE;
static boole _motion_internal__compositor_required = FALSE;
static boole _motion_internal__animating = FALSE;
static _motion_internal__match_cache_t* _motion_internal__matches = NULL_POINTER;
static int _motion_internal__match_capacity = 0;
static int _motion_internal__match_count = 0;
static int _motion_internal__match_cursor = 0;
static int _motion_internal__rule_count = 0;
static uint64 _motion_internal__match_generation = 0;
static gui_node_t* _motion_internal__match_focus = NULL_POINTER;
static boole _motion_internal__match_enabled = FALSE;
static boole _motion_internal__match_changed = FALSE;
// nodes whose hover / pressed state changed since the last tick: their
// selector matches (and their ancestors' :has and their descendants'
// combinators) are recomputed; a structural change (class, id, tree
// shape, type) still invalidates every node
#define _MOTION_INTERNAL__STALE_CAP 32
static int _motion_internal__stale_indices[_MOTION_INTERNAL__STALE_CAP];
static int _motion_internal__stale_count = 0;

static void _motion_internal__copy(char* out, int64 cap, const char* begin, const char* end);
static const char* _motion_internal__space(const char* p);
static const char* _motion_internal__end(const char* p, char delimiter);
static const char* _motion_internal__close(const char* p);
static void _motion_internal__blank(char* out, int64 begin, int64 end);
static boole _motion_internal__extended(const char* name, const char* value);
static _motion_internal__decl_t* _motion_internal__declarations(const char* start, const char* end, const char* source, char* filtered, boole keyframe);
static void _motion_internal__free_declarations(_motion_internal__decl_t* declarations);
static const char* _motion_internal__decl_value(_motion_internal__decl_t* declarations, const char* name);
static const char* _motion_internal__decl_value_id(_motion_internal__decl_t* declarations, int id);
static void _motion_internal__intern_known(void);
static void _motion_internal__read_frames(const char* name, const char* start, const char* end);
static _motion_internal__state_t* _motion_internal__state(gui_node_t* node, boole create);
static void _motion_internal__set(_motion_internal__state_t* state, int id, const char* value);
static const char* _motion_internal__raw(gui_node_t* node, int id);
static boole _motion_internal__compound(gui_node_t* node, const char* selector);
static boole _motion_internal__descendant(gui_node_t* node, const char* selector);
static void _motion_internal__expand(gui_node_t* node, const char* value, char* out, int64 cap, int depth);
static float _motion_internal__expression(const char** text, float reference);
static float _motion_internal__product(const char** text, float reference);
static float _motion_internal__atom(const char** text, float reference);
static float _motion_internal__length(gui_node_t* node, const char* value, float reference);
static void _motion_internal__blend(const char* a, const char* b, float t, char* out, int64 cap);
static float _motion_internal__ease(float t, const char* easing);
static float _motion_internal__timeline(gui_node_t* node, const char* timeline);
static void _motion_internal__animate(_motion_internal__state_t* state);
static void _motion_internal__visit(gui_node_t* node, float delta);
static boole _motion_internal__snapshot(gui_node_t* node, int parent_index);
static void _motion_internal__mark_stale(void);

/**
 * @brief copy and trim a bounded source span
 * @param char* out -> destination
 * @param int64 cap -> capacity including terminator
 * @param const char* begin -> first byte
 * @param const char* end -> one past last byte
 * @return void
 */
static void _motion_internal__copy(char* out, int64 cap, const char* begin, const char* end)
{
    int64 length = 0;

    if (cap <= 0)
    {
        return;
    }
    while (begin < end && *begin <= ' ')
    {
        begin++;
    }
    while (end > begin && end[-1] <= ' ')
    {
        end--;
    }
    length = end - begin;
    if (length >= cap)
    {
        length = cap - 1;
        log_warn("%s", "CSS motion value exceeds its storage capacity");
    }
    stdlib__memcpy(out, begin, length);
    out[length] = 0;
}

/**
 * @brief skip CSS whitespace and block comments
 * @param const char* p -> cursor
 * @return const char* -> next token or terminator
 */
static const char* _motion_internal__space(const char* p)
{
    while (*p != 0)
    {
        if (*p <= ' ')
        {
            p++;
        }
        else if (p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (*p != 0 && (p[0] != '*' || p[1] != '/'))
            {
                p++;
            }
            if (*p != 0)
            {
                p += 2;
            }
        }
        else
        {
            break;
        }
    }
    return p;
}

/**
 * @brief find a delimiter outside quotes, comments and function parentheses
 * @param const char* p -> cursor
 * @param char delimiter -> desired delimiter
 * @return const char* -> delimiter or terminator
 */
static const char* _motion_internal__end(const char* p, char delimiter)
{
    int depth = 0;
    char quote = 0;

    while (*p != 0)
    {
        if (quote != 0)
        {
            if (*p == '\\' && p[1] != 0)
            {
                p += 2;
                continue;
            }
            if (*p == quote)
            {
                quote = 0;
            }
        }
        else if (*p == '\'' || *p == '"')
        {
            quote = *p;
        }
        else if (p[0] == '/' && p[1] == '*')
        {
            p = _motion_internal__space(p);
            continue;
        }
        else if (depth == 0 && (*p == delimiter || *p == '}'))
        {
            return p;
        }
        else if (*p == '(')
        {
            depth++;
        }
        else if (*p == ')')
        {
            depth--;
        }
        p++;
    }
    return p;
}

/**
 * @brief find the closing brace of one CSS block
 * @param const char* p -> opening brace
 * @return const char* -> matching brace or terminator
 */
static const char* _motion_internal__close(const char* p)
{
    int depth = 1;
    char quote = 0;

    p++;
    while (*p != 0 && depth > 0)
    {
        if (quote != 0)
        {
            if (*p == '\\' && p[1] != 0)
            {
                p += 2;
                continue;
            }
            if (*p == quote)
            {
                quote = 0;
            }
        }
        else if (*p == '\'' || *p == '"')
        {
            quote = *p;
        }
        else if (p[0] == '/' && p[1] == '*')
        {
            p = _motion_internal__space(p);
            continue;
        }
        else if (*p == '{')
        {
            depth++;
        }
        else if (*p == '}')
        {
            depth--;
            if (depth == 0)
            {
                return p;
            }
        }
        p++;
    }
    return p;
}

/**
 * @brief hide an extended construct from the legacy parser while retaining line numbers
 * @param char* out -> mutable stylesheet
 * @param int64 begin -> first byte
 * @param int64 end -> one past last byte
 * @return void
 */
static void _motion_internal__blank(char* out, int64 begin, int64 end)
{
    int64 i = 0;

    for (i = begin; i < end; i++)
    {
        if (out[i] != '\n' && out[i] != '\r')
        {
            out[i] = ' ';
        }
    }
}

/**
 * @brief identify declarations evaluated by the motion and paint layers
 * @param const char* name -> property name
 * @param const char* value -> declaration value
 * @return boole -> TRUE for an extended declaration
 */
static boole _motion_internal__extended(const char* name, const char* value)
{
    const char* properties[] = {"transform", "transform-origin", "filter", "backdrop-filter", "mask", "mask-repeat", "mask-image", "mask-size", "mask-position", "mask-composite", "clip-path", "background-position", "background-clip", "background-repeat", "mix-blend-mode", "text-shadow", "-webkit-text-stroke", "-webkit-background-clip", "pointer-events", "font-variation-settings", "letter-spacing", "animation-name", "animation-duration", "animation-delay", "animation-direction", "animation-iteration-count", "animation-fill-mode", "animation-timing-function", "animation-timeline", "animation-range"};
    int i = 0;

    if (stdlib__strncmp(name, "navigation-", 11) == 0)
    {
        return TRUE;
    }
    if (name[0] == '-' && name[1] == '-')
    {
        return TRUE;
    }
    if (stdlib__strcmp(name, "animation") == 0)
    {
        return (boole)((value[0] < '0' || value[0] > '9') && value[0] != '.');
    }
    if (stdlib__strcmp(name, "border-radius") == 0 && (stdlib__strchr(value, '/') != NULL_POINTER || stdlib__strchr(value, '%') != NULL_POINTER))
    {
        return TRUE;
    }
    if (stdlib__strcmp(name, "background-size") == 0)
    {
        return (boole)((value[0] >= '0' && value[0] <= '9') || stdlib__strstr(value, "calc(") != NULL_POINTER);
    }
    for (i = 0; i < (int)(sizeof(properties) / sizeof(properties[0])); i++)
    {
        if (stdlib__strcmp(name, properties[i]) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief collect declarations and remove extended paint values from a parser copy
 * @param const char* start -> block contents
 * @param const char* end -> closing brace
 * @param const char* source -> complete input for offsets
 * @param char* filtered -> optional mutable parser copy
 * @param boole keyframe -> retain every keyframe declaration
 * @return _motion_internal__decl_t* -> owned declaration list
 */
static _motion_internal__decl_t* _motion_internal__declarations(const char* start, const char* end, const char* source, char* filtered, boole keyframe)
{
    _motion_internal__decl_t* first = NULL_POINTER;
    _motion_internal__decl_t** tail = &first;
    _motion_internal__decl_t* declaration = NULL_POINTER;
    const char* p = start;
    const char* colon = NULL_POINTER;
    const char* finish = NULL_POINTER;
    const char* nested = NULL_POINTER;
    char name[48];
    char value[_MOTION_INTERNAL__VALUE_CAP];
    boole extended = FALSE;
    boole paint = FALSE;

    stdlib__null_memory(name, sizeof(name));
    stdlib__null_memory(value, sizeof(value));
    while ((p = _motion_internal__space(p)) < end && *p != 0)
    {
        colon = _motion_internal__end(p, ':');
        finish = _motion_internal__end(p, ';');
        nested = _motion_internal__end(p, '{');
        if (nested < finish && nested < end && *nested == '{')
        {
            p = _motion_internal__close(nested);
            if (*p != 0)
            {
                p++;
            }
            continue;
        }
        if (colon >= end || *colon != ':' || finish <= colon)
        {
            break;
        }
        if (finish > end)
        {
            finish = end;
        }
        _motion_internal__copy(name, sizeof(name), p, colon);
        _motion_internal__copy(value, sizeof(value), colon + 1, finish);
        extended = _motion_internal__extended(name, value);
        paint = (boole)(stdlib__strcmp(name, "background") == 0 || stdlib__strcmp(name, "background-image") == 0 || stdlib__strcmp(name, "background-color") == 0 || stdlib__strcmp(name, "color") == 0 || stdlib__strcmp(name, "font-color") == 0 || stdlib__strcmp(name, "opacity") == 0);
        // Timeline evaluation does not itself require a pixel surface. Keep a
        // conservative native path for declarations applied to resolved styles;
        // backgrounds, group opacity and every other extension retain the
        // existing compositor, including effects declared inside keyframes.
        if ((extended == TRUE && stdlib__strcmp(name, "animation") != 0 &&
             stdlib__strncmp(name, "animation-", 10) != 0 && stdlib__strncmp(name, "navigation-", 11) != 0) ||
            stdlib__strcmp(name, "background") == 0 || stdlib__strcmp(name, "background-image") == 0 ||
            stdlib__strcmp(name, "opacity") == 0 || stdlib__strcmp(name, "border-radius") == 0 ||
            stdlib__strstr(value, "var(") != NULL_POINTER ||
            (keyframe == TRUE && stdlib__strcmp(name, "background-color") != 0 &&
             stdlib__strcmp(name, "color") != 0 && stdlib__strcmp(name, "font-color") != 0 &&
             stdlib__strcmp(name, "border-color") != 0 && stdlib__strcmp(name, "width") != 0 &&
             stdlib__strcmp(name, "height") != 0 && stdlib__strcmp(name, "left") != 0 &&
             stdlib__strcmp(name, "top") != 0))
        {
            _motion_internal__compositor_required = TRUE;
        }
        if (extended == TRUE || paint == TRUE || keyframe == TRUE)
        {
            declaration = GUI_CALLOC_T(1, sizeof(*declaration), MM_TYPE__GENERIC);
            if (declaration == NULL_POINTER)
            {
                break;
            }
            stdlib__strcpy_safe(declaration->name, sizeof(declaration->name), name);
            stdlib__strcpy_safe(declaration->value, sizeof(declaration->value), value);
            declaration->id = motion__prop_id(name);
            *tail = declaration;
            tail = &declaration->next;
            if (filtered != NULL_POINTER)
            {
                _motion_internal__blank(filtered, p - source, finish - source + ((*finish == ';') ? 1 : 0));
            }
        }
        if (extended == TRUE || stdlib__strstr(value, "repeating-") != NULL_POINTER || stdlib__strstr(value, "var(") != NULL_POINTER || (paint == TRUE && *_motion_internal__end(value, ',') == ','))
        {
            _motion_internal__active = TRUE;
        }
        p = finish + ((*finish == ';') ? 1 : 0);
    }
    return first;
}

/**
 * @brief release an owned declaration list
 * @param _motion_internal__decl_t* declarations -> list head
 * @return void
 */
static void _motion_internal__free_declarations(_motion_internal__decl_t* declarations)
{
    _motion_internal__decl_t* next = NULL_POINTER;

    while (declarations != NULL_POINTER)
    {
        next = declarations->next;
        GUI_FREE(declarations);
        declarations = next;
    }
}

/**
 * @brief find the last declaration of a named property
 * @param _motion_internal__decl_t* declarations -> declaration list
 * @param const char* name -> property name
 * @return const char* -> borrowed value or NULL
 */
static const char* _motion_internal__decl_value(_motion_internal__decl_t* declarations, const char* name)
{
    const char* result = NULL_POINTER;

    while (declarations != NULL_POINTER)
    {
        if (stdlib__strcmp(declarations->name, name) == 0)
        {
            result = declarations->value;
        }
        declarations = declarations->next;
    }
    return result;
}

static const char* _motion_internal__decl_value_id(_motion_internal__decl_t* declarations, int id)
{
    const char* result = NULL_POINTER;

    while (declarations != NULL_POINTER)
    {
        if (declarations->id == id)
        {
            result = declarations->value;
        }
        declarations = declarations->next;
    }
    return result;
}

// the known names take their enum values so a stylesheet's ids and the
// consumers' constants agree
static void _motion_internal__intern_known(void)
{
    int i = 0;

    if (_motion_internal__prop_count > 0)
    {
        return;
    }
    for (i = 0; i < MOTION_PROP__KNOWN_COUNT; i++)
    {
        stdlib__strcpy_safe(_motion_internal__prop_names[i], sizeof(_motion_internal__prop_names[i]), _motion_internal__known_names[i]);
        _motion_internal__prop_custom[i] = FALSE;
    }
    _motion_internal__prop_count = MOTION_PROP__KNOWN_COUNT;
}

/**
 * @brief the id of a property name, interning it on first sight (parse time; the per-frame path only carries ids)
 * @param const char* name -> property name, `--*` for a custom property
 * @return int -> the id, or -1 when the table is full
 */
int motion__prop_id(const char* name)
{
    int i = 0;

    _motion_internal__intern_known();
    if (name == NULL_POINTER || name[0] == 0)
    {
        return -1;
    }
    for (i = 0; i < _motion_internal__prop_count; i++)
    {
        if (stdlib__strcmp(_motion_internal__prop_names[i], name) == 0)
        {
            return i;
        }
    }
    if (_motion_internal__prop_count >= MOTION_PROP__CAP)
    {
        log_error("%s %s", "CSS motion property table full, ignoring", name);
        return -1;
    }
    stdlib__strcpy_safe(_motion_internal__prop_names[_motion_internal__prop_count], sizeof(_motion_internal__prop_names[0]), name);
    _motion_internal__prop_custom[_motion_internal__prop_count] = (boole)(name[0] == '-' && name[1] == '-');
    _motion_internal__prop_count++;
    return _motion_internal__prop_count - 1;
}

/**
 * @brief release all stylesheet-owned motion data and per-node computed values
 * @return void
 */
void motion__reset(void)
{
    _motion_internal__rule_t* rule = NULL_POINTER;
    _motion_internal__frame_t* frame = NULL_POINTER;
    _motion_internal__state_t* state = NULL_POINTER;

    while (_motion_internal__rules != NULL_POINTER)
    {
        rule = _motion_internal__rules;
        _motion_internal__rules = rule->next;
        _motion_internal__free_declarations(rule->declarations);
        GUI_FREE(rule);
    }
    while (_motion_internal__frames != NULL_POINTER)
    {
        frame = _motion_internal__frames;
        _motion_internal__frames = frame->next;
        _motion_internal__free_declarations(frame->declarations);
        GUI_FREE(frame);
    }
    while (_motion_internal__states != NULL_POINTER)
    {
        state = _motion_internal__states;
        _motion_internal__states = state->next;
        GUI_FREE(state);
    }
    _motion_internal__free_declarations(_motion_internal__defaults);
    _motion_internal__defaults = NULL_POINTER;
    stdlib__null_memory(_motion_internal__state_buckets, sizeof(_motion_internal__state_buckets));
    _motion_internal__active = FALSE;
    _motion_internal__compositor_required = FALSE;
    _motion_internal__animating = FALSE;
    GUI_FREE(_motion_internal__matches);
    _motion_internal__matches = NULL_POINTER;
    _motion_internal__match_capacity = 0;
    _motion_internal__match_count = 0;
    _motion_internal__match_cursor = 0;
    _motion_internal__rule_count = 0;
    _motion_internal__match_generation = 0;
    _motion_internal__match_focus = NULL_POINTER;
    _motion_internal__match_enabled = FALSE;
}

/**
 * @brief read percentage blocks belonging to one named keyframe animation
 * @param const char* name -> animation name
 * @param const char* start -> first keyframe block
 * @param const char* end -> enclosing closing brace
 * @return void
 */
static void _motion_internal__read_frames(const char* name, const char* start, const char* end)
{
    const char* p = start;
    const char* open = NULL_POINTER;
    const char* close = NULL_POINTER;
    const char* part = NULL_POINTER;
    const char* comma = NULL_POINTER;
    _motion_internal__frame_t* frame = NULL_POINTER;
    _motion_internal__frame_t** tail = &_motion_internal__frames;
    char offset[32];

    stdlib__null_memory(offset, sizeof(offset));
    while (*tail != NULL_POINTER)
    {
        tail = &(*tail)->next;
    }
    while ((p = _motion_internal__space(p)) < end)
    {
        open = _motion_internal__end(p, '{');
        if (open >= end || *open != '{')
        {
            break;
        }
        close = _motion_internal__close(open);
        part = p;
        while (part < open)
        {
            comma = _motion_internal__end(part, ',');
            if (comma > open)
            {
                comma = open;
            }
            _motion_internal__copy(offset, sizeof(offset), part, comma);
            frame = GUI_CALLOC_T(1, sizeof(*frame), MM_TYPE__GENERIC);
            if (frame == NULL_POINTER)
            {
                return;
            }
            stdlib__strcpy_safe(frame->name, sizeof(frame->name), name);
            frame->offset = (stdlib__strcmp(offset, "to") == 0) ? 1.0f : motion__number(offset, 1.0f);
            frame->declarations = _motion_internal__declarations(open + 1, close, start, NULL_POINTER, TRUE);
            *tail = frame;
            tail = &frame->next;
            part = comma + 1;
        }
        p = close + ((*close == '}') ? 1 : 0);
    }
}

/**
 * @brief separate extended CSS constructs from the existing layout-style parser
 * @param const char* bytes -> stylesheet input
 * @param int64 length -> input byte length
 * @return char* -> owned filtered copy, or NULL when legacy parsing is sufficient
 */
char* motion__prepare_styles(const char* bytes, int64 length)
{
    char* source = NULL_POINTER;
    char* filtered = NULL_POINTER;
    const char* p = NULL_POINTER;
    const char* open = NULL_POINTER;
    const char* close = NULL_POINTER;
    const char* value = NULL_POINTER;
    _motion_internal__rule_t* rule = NULL_POINTER;
    _motion_internal__rule_t** tail = &_motion_internal__rules;
    _motion_internal__decl_t* declarations = NULL_POINTER;
    _motion_internal__decl_t* initial = NULL_POINTER;
    char header[256];
    char name[64];

    stdlib__null_memory(header, sizeof(header));
    stdlib__null_memory(name, sizeof(name));
    motion__reset();
    source = GUI_MALLOC_T(length + 1, MM_TYPE__GENERIC);
    filtered = GUI_MALLOC_T(length + 1, MM_TYPE__GENERIC);
    if (source == NULL_POINTER || filtered == NULL_POINTER)
    {
        GUI_FREE(source);
        GUI_FREE(filtered);
        return NULL_POINTER;
    }
    stdlib__memcpy(source, bytes, length);
    source[length] = 0;
    stdlib__memcpy(filtered, source, length + 1);
    p = source;
    while (*(p = _motion_internal__space(p)) != 0)
    {
        open = _motion_internal__end(p, '{');
        if (*open != '{')
        {
            break;
        }
        close = _motion_internal__close(open);
        if (*close != '}')
        {
            log_error("%s", "unterminated CSS motion block");
            break;
        }
        _motion_internal__copy(header, sizeof(header), p, open);
        if (stdlib__strncmp(header, "@keyframes", 10) == 0)
        {
            _motion_internal__read_frames(_motion_internal__space(header + 10), open + 1, close);
            _motion_internal__blank(filtered, p - source, close - source + 1);
            _motion_internal__active = TRUE;
        }
        else if (stdlib__strncmp(header, "@property", 9) == 0)
        {
            declarations = _motion_internal__declarations(open + 1, close, source, NULL_POINTER, TRUE);
            value = _motion_internal__decl_value(declarations, "initial-value");
            if (value != NULL_POINTER)
            {
                initial = GUI_CALLOC_T(1, sizeof(*initial), MM_TYPE__GENERIC);
                if (initial != NULL_POINTER)
                {
                    stdlib__strcpy_safe(initial->name, sizeof(initial->name), _motion_internal__space(header + 9));
                    stdlib__strcpy_safe(initial->value, sizeof(initial->value), value);
                    initial->id = motion__prop_id(initial->name);
                    value = _motion_internal__decl_value(declarations, "inherits");
                    initial->inherits = (boole)(value != NULL_POINTER && stdlib__strcmp(value, "true") == 0);
                    initial->next = _motion_internal__defaults;
                    _motion_internal__defaults = initial;
                }
            }
            _motion_internal__free_declarations(declarations);
            _motion_internal__blank(filtered, p - source, close - source + 1);
            _motion_internal__active = TRUE;
        }
        else if (header[0] != '@')
        {
            declarations = _motion_internal__declarations(open + 1, close, source, filtered, FALSE);
            if (declarations != NULL_POINTER)
            {
                rule = GUI_CALLOC_T(1, sizeof(*rule), MM_TYPE__GENERIC);
                if (rule != NULL_POINTER)
                {
                    stdlib__strcpy_safe(rule->selector, sizeof(rule->selector), header);
                    rule->specificity = (stdlib__strchr(header, '#') != NULL_POINTER) ? 2 : ((stdlib__strchr(header, '.') != NULL_POINTER) ? 1 : 0);
                    rule->index = _motion_internal__rule_count++;
                    rule->declarations = declarations;
                    *tail = rule;
                    tail = &rule->next;
                }
                else
                {
                    _motion_internal__free_declarations(declarations);
                }
            }
            if (stdlib__strstr(header, ":has(") != NULL_POINTER || stdlib__strcmp(header, ":root") == 0 || _motion_internal__space(filtered + (open - source) + 1) >= filtered + (close - source))
            {
                _motion_internal__blank(filtered, p - source, close - source + 1);
                if (stdlib__strstr(header, ":has(") != NULL_POINTER || stdlib__strcmp(header, ":root") == 0)
                {
                    _motion_internal__active = TRUE;
                }
            }
        }
        p = close + 1;
    }
    GUI_FREE(source);
    if (_motion_internal__active == FALSE)
    {
        motion__reset();
        GUI_FREE(filtered);
        return NULL_POINTER;
    }
    return filtered;
}

/**
 * @brief find or allocate per-node timeline state
 * @param gui_node_t* node -> existing scene node
 * @param boole create -> permit allocation
 * @return _motion_internal__state_t* -> state or NULL
 */
static _motion_internal__state_t* _motion_internal__state(gui_node_t* node, boole create)
{
    nuint bucket = (((nuint)node >> 4) ^ ((nuint)node >> 17)) & (_MOTION_INTERNAL__STATE_BUCKETS - 1);
    _motion_internal__state_t* state = _motion_internal__state_buckets[bucket];

    while (state != NULL_POINTER && state->node != node)
    {
        state = state->hash_next;
    }
    if (state == NULL_POINTER && create == TRUE)
    {
        state = GUI_CALLOC_T(1, sizeof(*state), MM_TYPE__GENERIC);
        if (state != NULL_POINTER)
        {
            state->node = node;
            state->hash_next = _motion_internal__state_buckets[bucket];
            _motion_internal__state_buckets[bucket] = state;
            state->next = _motion_internal__states;
            _motion_internal__states = state;
        }
    }
    return state;
}

/**
 * @brief release timeline state when a scene node is destroyed
 * @param gui_node_t* node -> node being removed
 * @return void
 */
void motion__forget(gui_node_t* node)
{
    _motion_internal__state_t** link = &_motion_internal__states;
    nuint bucket = (((nuint)node >> 4) ^ ((nuint)node >> 17)) & (_MOTION_INTERNAL__STATE_BUCKETS - 1);
    _motion_internal__state_t** hash_link = &_motion_internal__state_buckets[bucket];
    _motion_internal__state_t* state = NULL_POINTER;

    while (*link != NULL_POINTER)
    {
        if ((*link)->node == node)
        {
            state = *link;
            *link = state->next;
            while (*hash_link != NULL_POINTER && *hash_link != state)
            {
                hash_link = &(*hash_link)->hash_next;
            }
            if (*hash_link == state)
            {
                *hash_link = state->hash_next;
            }
            GUI_FREE(state);
            return;
        }
        link = &(*link)->next;
    }
}

/**
 * @brief assign a computed CSS value with last-declaration precedence
 * @param _motion_internal__state_t* state -> node state
 * @param const char* name -> property name
 * @param const char* value -> raw computed expression
 * @return void
 */
static void _motion_internal__set(_motion_internal__state_t* state, int id, const char* value)
{
    int i = 0;

    if (id < 0)
    {
        return;
    }
    if (id < (int)sizeof(state->slot) && state->slot[id] != 0)
    {
        i = state->slot[id] - 1;
    }
    else
    {
        for (i = 0; i < state->count; i++)
        {
            if (state->values[i].id == id)
            {
                break;
            }
        }
    }
    if (i == _MOTION_INTERNAL__MAX_VALUES)
    {
        log_error("%s", "CSS motion property capacity exceeded");
        return;
    }
    if (i == state->count)
    {
        state->count++;
        if (id < (int)sizeof(state->slot))
        {
            state->slot[id] = (ubyte)(i + 1);
        }
    }
    state->values[i].id = id;
    stdlib__strcpy_safe(state->values[i].value, sizeof(state->values[i].value), value);
}

/**
 * @brief look up a raw property, inheriting custom properties through the scene tree
 * @param gui_node_t* node -> target node
 * @param const char* name -> property name
 * @return const char* -> borrowed computed expression or NULL
 */
static const char* _motion_internal__raw(gui_node_t* node, int id)
{
    _motion_internal__state_t* state = NULL_POINTER;
    _motion_internal__decl_t* registration = _motion_internal__defaults;
    boole custom = FALSE;
    int i = 0;

    if (id < 0 || id >= _motion_internal__prop_count)
    {
        return NULL_POINTER;
    }
    custom = _motion_internal__prop_custom[id];
    while (registration != NULL_POINTER && registration->id != id)
    {
        registration = registration->next;
    }
    while (node != NULL_POINTER)
    {
        state = _motion_internal__state(node, FALSE);
        if (state != NULL_POINTER)
        {
            if (id < (int)sizeof(state->slot))
            {
                if (state->slot[id] != 0)
                {
                    return state->values[state->slot[id] - 1].value;
                }
            }
            else
            {
                for (i = 0; i < state->count; i++)
                {
                    if (state->values[i].id == id)
                    {
                        return state->values[i].value;
                    }
                }
            }
        }
        if (custom == FALSE || (registration != NULL_POINTER && registration->inherits == FALSE))
        {
            break;
        }
        node = node->parent;
    }
    return registration != NULL_POINTER ? registration->value : NULL_POINTER;
}

/**
 * @brief query whether a node has an extended property
 * @param gui_node_t* node -> target node
 * @param const char* property -> property name
 * @return boole -> TRUE when assigned
 */
boole motion__has_id(gui_node_t* node, int id)
{
    return (boole)(_motion_internal__active == TRUE && _motion_internal__raw(node, id) != NULL_POINTER);
}

boole motion__has(gui_node_t* node, const char* property)
{
    if (_motion_internal__active == FALSE)
    {
        return FALSE;
    }
    return motion__has_id(node, motion__prop_id(property));
}

/**
 * @brief match a compound selector, including hover, focus, has and nth-child
 * @param gui_node_t* node -> candidate node
 * @param const char* selector -> single compound selector
 * @return boole -> TRUE when matched
 */
static boole _motion_internal__compound(gui_node_t* node, const char* selector)
{
    const char* p = selector;
    const char* begin = NULL_POINTER;
    const char* end = NULL_POINTER;
    const char* found = NULL_POINTER;
    gui_node_t* sibling = NULL_POINTER;
    char token[128];
    char kind = 0;
    int index = 1;
    int depth = 0;

    stdlib__null_memory(token, sizeof(token));
    if (node == NULL_POINTER)
    {
        return FALSE;
    }
    while (*p != 0)
    {
        kind = *p;
        if (kind == '.' || kind == '#' || kind == ':')
        {
            p++;
        }
        begin = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '*')
        {
            p++;
        }
        if (p == begin)
        {
            return FALSE;
        }
        _motion_internal__copy(token, sizeof(token), begin, p);
        if (kind == '.')
        {
            found = node->klass;
            while ((found = stdlib__strstr(found, token)) != NULL_POINTER)
            {
                if ((found == node->klass || found[-1] == ' ') && (found[stdlib__strlen(token)] == 0 || found[stdlib__strlen(token)] == ' '))
                {
                    break;
                }
                found++;
            }
            if (found == NULL_POINTER)
            {
                return FALSE;
            }
        }
        else if (kind == '#')
        {
            if (stdlib__strcmp(node->id, token) != 0)
            {
                return FALSE;
            }
        }
        else if (kind == ':')
        {
            if (stdlib__strcmp(token, "hover") == 0 && node->state != GUI_NODE_STATE__HOVER && node->state != GUI_NODE_STATE__PRESSED)
            {
                return FALSE;
            }
            if ((stdlib__strcmp(token, "pressed") == 0 || stdlib__strcmp(token, "active") == 0) && node->state != GUI_NODE_STATE__PRESSED)
            {
                return FALSE;
            }
            if (stdlib__strcmp(token, "checked") == 0 && ((node->type != GUI_NODE_TYPE__OPTION && node->type != GUI_NODE_TYPE__CHECKBOX && node->type != GUI_NODE_TYPE__RADIO) || node->value < 0.5f))
            {
                return FALSE;
            }
            if (stdlib__strcmp(token, "focus") == 0 && scene__focus() != node)
            {
                return FALSE;
            }
            if (stdlib__strcmp(token, "root") == 0 && node != scene__root())
            {
                return FALSE;
            }
            if (*p == '(')
            {
                begin = ++p;
                depth = 1;
                while (*p != 0 && depth > 0)
                {
                    if (*p == '(')
                    {
                        depth++;
                    }
                    else if (*p == ')')
                    {
                        depth--;
                    }
                    if (depth > 0)
                    {
                        p++;
                    }
                }
                end = p;
                if (stdlib__strcmp(token, "nth-child") == 0)
                {
                    if (node->parent != NULL_POINTER)
                    {
                        sibling = node->parent->first_child;
                        while (sibling != NULL_POINTER && sibling != node)
                        {
                            index++;
                            sibling = sibling->next_sibling;
                        }
                    }
                    if (index != (int)motion__number(begin, 1.0f))
                    {
                        return FALSE;
                    }
                }
                else if (stdlib__strcmp(token, "has") == 0)
                {
                    _motion_internal__copy(token, sizeof(token), begin, end);
                    if (_motion_internal__descendant(node, token) == FALSE)
                    {
                        return FALSE;
                    }
                }
                else
                {
                    return FALSE;
                }
                if (*p == ')')
                {
                    p++;
                }
            }
            else if (stdlib__strcmp(token, "hover") != 0 && stdlib__strcmp(token, "pressed") != 0 && stdlib__strcmp(token, "active") != 0 && stdlib__strcmp(token, "focus") != 0 && stdlib__strcmp(token, "root") != 0 && stdlib__strcmp(token, "checked") != 0)
            {
                return FALSE;
            }
        }
        else if (stdlib__strcmp(token, "*") != 0 && stdlib__strcmp(widget_registry__type_name(node->type), token) != 0)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief test descendants for a relational selector
 * @param gui_node_t* node -> subtree owner
 * @param const char* selector -> target selector
 * @return boole -> TRUE when a descendant matches
 */
static boole _motion_internal__descendant(gui_node_t* node, const char* selector)
{
    gui_node_t* child = node->first_child;

    while (child != NULL_POINTER)
    {
        if (motion__matches(child, selector) == TRUE || _motion_internal__descendant(child, selector) == TRUE)
        {
            return TRUE;
        }
        child = child->next_sibling;
    }
    return FALSE;
}

/**
 * @brief match selector lists and descendant combinators on existing nodes
 * @param gui_node_t* node -> candidate
 * @param const char* selector -> CSS selector text
 * @return boole -> TRUE when matched
 */
boole motion__matches(gui_node_t* node, const char* selector)
{
    const char* p = selector;
    const char* comma = NULL_POINTER;
    const char* split = NULL_POINTER;
    const char* cursor = NULL_POINTER;
    gui_node_t* parent = NULL_POINTER;
    char part[256];
    char ancestor[256];
    int depth = 0;

    stdlib__null_memory(part, sizeof(part));
    stdlib__null_memory(ancestor, sizeof(ancestor));
    while (*p != 0)
    {
        comma = _motion_internal__end(p, ',');
        _motion_internal__copy(part, sizeof(part), p, comma);
        split = NULL_POINTER;
        depth = 0;
        for (cursor = part; *cursor != 0; cursor++)
        {
            if (*cursor == '(')
            {
                depth++;
            }
            else if (*cursor == ')')
            {
                depth--;
            }
            else if (depth == 0 && *cursor == ' ')
            {
                split = cursor;
            }
        }
        if (split == NULL_POINTER)
        {
            if (_motion_internal__compound(node, part) == TRUE)
            {
                return TRUE;
            }
        }
        else if (_motion_internal__compound(node, split + 1) == TRUE)
        {
            _motion_internal__copy(ancestor, sizeof(ancestor), part, split);
            parent = node->parent;
            while (parent != NULL_POINTER)
            {
                if (motion__matches(parent, ancestor) == TRUE)
                {
                    return TRUE;
                }
                parent = parent->parent;
            }
        }
        p = comma + ((*comma == ',') ? 1 : 0);
    }
    return FALSE;
}

/**
 * @brief format a finite animation number without locale-sensitive libc formatting
 * @param float value -> number
 * @param char* out -> destination
 * @param int64 capacity -> destination size
 * @return void
 */
void motion__format(float value, char* out, int64 capacity)
{
    char digits[32];
    int64 scaled = 0;
    int64 integer = 0;
    int64 fraction = 0;
    int count = 0;
    int at = 0;
    int i = 0;
    int64 divisor = 10000;

    stdlib__null_memory(digits, sizeof(digits));
    if (capacity <= 0)
    {
        return;
    }
    out[0] = 0;
    if (value < 0.0f && at + 1 < capacity)
    {
        out[at++] = '-';
        value = -value;
    }
    value = stdlib__fminf(value, 1000000000.0f);
    scaled = (int64)((double)value * 100000.0 + 0.5);
    integer = stdlib__divmod64(scaled, 100000, &fraction);
    do
    {
        int64 digit;
        integer = stdlib__divmod64(integer, 10, &digit);
        digits[count++] = (char)('0' + digit);
    } while (integer > 0 && count < 31);
    for (i = count - 1; i >= 0 && at + 1 < capacity; i--)
    {
        out[at++] = digits[i];
    }
    if (fraction != 0 && at + 1 < capacity)
    {
        out[at++] = '.';
        while (divisor > 0 && at + 1 < capacity)
        {
            out[at++] = (char)('0' + stdlib__divmod64(fraction, divisor, &fraction));
            divisor = stdlib__div64(divisor, 10);
        }
        while (at > 0 && out[at - 1] == '0')
        {
            at--;
        }
    }
    out[at] = 0;
}

/**
 * @brief evaluate a numeric atom with CSS units and nested parentheses
 * @param const char** text -> advancing expression cursor
 * @param float reference -> value represented by 100 percent
 * @return float -> evaluated number
 */
static float _motion_internal__atom(const char** text, float reference)
{
    const char* p = _motion_internal__space(*text);
    char* end = NULL_POINTER;
    float value = 0.0f;
    float sign = 1.0f;

    if (*p == '+' || *p == '-')
    {
        sign = (*p == '-') ? -1.0f : 1.0f;
        p = _motion_internal__space(p + 1);
    }
    if (stdlib__strncmp(p, "calc(", 5) == 0)
    {
        p += 4;
    }
    if (*p == '(')
    {
        p++;
        value = _motion_internal__expression(&p, reference);
        if (*p == ')')
        {
            p++;
        }
    }
    else
    {
        value = stdlib__strtof(p, &end);
        if (end == p)
        {
            *text = p;
            return 0.0f;
        }
        p = end;
        if (*p == '%')
        {
            value *= reference / 100.0f;
            p++;
        }
        else if (stdlib__strncmp(p, "turn", 4) == 0)
        {
            value *= 360.0f;
            p += 4;
        }
        else if (stdlib__strncmp(p, "rad", 3) == 0)
        {
            value *= 180.0f / 3.14159265358979f;
            p += 3;
        }
        else if (stdlib__strncmp(p, "grad", 4) == 0)
        {
            value *= 0.9f;
            p += 4;
        }
        else
        {
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            {
                p++;
            }
        }
    }
    *text = p;
    return sign * value;
}

/**
 * @brief evaluate multiplication and division
 * @param const char** text -> expression cursor
 * @param float reference -> percentage base
 * @return float -> product
 */
static float _motion_internal__product(const char** text, float reference)
{
    float value = 0.0f;
    float rhs = 0.0f;
    char operation = 0;
    const char* p = *text;

    value = _motion_internal__atom(&p, reference);
    p = _motion_internal__space(p);
    while (*p == '*' || *p == '/')
    {
        operation = *p++;
        rhs = _motion_internal__atom(&p, reference);
        value = (operation == '*') ? value * rhs : ((rhs != 0.0f) ? value / rhs : 0.0f);
        p = _motion_internal__space(p);
    }
    *text = p;
    return value;
}

/**
 * @brief evaluate addition and subtraction
 * @param const char** text -> expression cursor
 * @param float reference -> percentage base
 * @return float -> expression value
 */
static float _motion_internal__expression(const char** text, float reference)
{
    float value = 0.0f;
    float rhs = 0.0f;
    char operation = 0;
    const char* p = *text;

    value = _motion_internal__product(&p, reference);
    p = _motion_internal__space(p);
    while (*p == '+' || *p == '-')
    {
        operation = *p++;
        rhs = _motion_internal__product(&p, reference);
        value += (operation == '+') ? rhs : -rhs;
        p = _motion_internal__space(p);
    }
    *text = p;
    return value;
}

/**
 * @brief evaluate a CSS length, angle or calc expression
 * @param const char* value -> expression text
 * @param float percent_reference -> value represented by 100 percent
 * @return float -> evaluated number
 */
float motion__number(const char* value, float percent_reference)
{
    if (value == NULL_POINTER)
    {
        return 0.0f;
    }
    return _motion_internal__expression(&value, percent_reference);
}

/**
 * @brief resolve an animated length using the node's font metrics
 * @param gui_node_t* node -> font and custom-property scope
 * @param const char* value -> resolved CSS length
 * @param float reference -> percentage base in logical pixels
 * @return float -> length in logical pixels
 */
static float _motion_internal__length(gui_node_t* node, const char* value, float reference)
{
    float result = motion__number(value, reference);
    gui_font_t* font = NULL_POINTER;

    if (stdlib__strstr(value, "ch") != NULL_POINTER)
    {
        font = font__at(node->resolved.font_family, node->resolved.font_size * scene__scale());
        result *= (font != NULL_POINTER) ? font__measure(font, "0") / scene__scale() : node->resolved.font_size * 0.5f;
    }
    else if (stdlib__strstr(value, "em") != NULL_POINTER)
    {
        result *= node->resolved.font_size;
    }
    return result;
}

/**
 * @brief resolve var and calc expressions with a bounded recursion depth
 * @param gui_node_t* node -> custom-property scope
 * @param const char* value -> input expression
 * @param char* out -> output buffer
 * @param int64 cap -> output capacity
 * @param int depth -> cycle guard
 * @return void
 */
static void _motion_internal__expand(gui_node_t* node, const char* value, char* out, int64 cap, int depth)
{
    const char* p = value;
    const char* begin = NULL_POINTER;
    const char* replacement = NULL_POINTER;
    const char* comma = NULL_POINTER;
    char inner[_MOTION_INTERNAL__VALUE_CAP];
    char expanded[_MOTION_INTERNAL__VALUE_CAP];
    char number[64];
    int nested = 0;
    int64 at = 0;
    int64 length = 0;
    boole variable = FALSE;

    // written before every read (a terminating writer), so one terminator
    // replaces the full clear on this per-node path
    inner[0] = 0;
    expanded[0] = 0;
    number[0] = 0;
    if (cap <= 0)
    {
        return;
    }
    out[0] = 0;
    if (depth > 16 || value == NULL_POINTER)
    {
        return;
    }
    while (*p != 0 && at + 1 < cap)
    {
        variable = (boole)(stdlib__strncmp(p, "var(", 4) == 0);
        if (variable == TRUE || stdlib__strncmp(p, "calc(", 5) == 0)
        {
            p += (variable == TRUE) ? 4 : 5;
            begin = p;
            nested = 1;
            while (*p != 0 && nested > 0)
            {
                if (*p == '(')
                {
                    nested++;
                }
                else if (*p == ')')
                {
                    nested--;
                }
                if (nested > 0)
                {
                    p++;
                }
            }
            _motion_internal__copy(inner, sizeof(inner), begin, p);
            if (*p == ')')
            {
                p++;
            }
            if (variable == TRUE)
            {
                comma = _motion_internal__end(inner, ',');
                _motion_internal__copy(number, sizeof(number), inner, comma);
                // a var() name is interned like a declared one; the
                // table is deduplicated, so an unknown name costs one entry
                replacement = _motion_internal__raw(node, motion__prop_id(number));
                if (replacement == NULL_POINTER)
                {
                    replacement = (*comma == ',') ? comma + 1 : "0";
                }
                _motion_internal__expand(node, replacement, expanded, sizeof(expanded), depth + 1);
            }
            else
            {
                _motion_internal__expand(node, inner, expanded, sizeof(expanded), depth + 1);
                if (stdlib__strchr(expanded, '%') != NULL_POINTER && stdlib__strstr(expanded, "px") != NULL_POINTER)
                {
                    stdlib__strcpy_safe(inner, sizeof(inner), expanded);
                    stdlib__strcpy_safe(expanded, sizeof(expanded), "calc(");
                    stdlib__strcpy_safe(expanded + 5, sizeof(expanded) - 6, inner);
                    length = (int64)stdlib__strlen(expanded);
                    expanded[length++] = ')';
                    expanded[length] = 0;
                }
                else
                {
                    motion__format(motion__number(expanded, 100.0f), number, sizeof(number));
                    replacement = (stdlib__strchr(expanded, '%') != NULL_POINTER) ? "%" : ((stdlib__strstr(expanded, "deg") != NULL_POINTER || stdlib__strstr(expanded, "turn") != NULL_POINTER || stdlib__strstr(expanded, "rad") != NULL_POINTER) ? "deg" : ((stdlib__strstr(expanded, "px") != NULL_POINTER) ? "px" : ((stdlib__strstr(expanded, "ms") != NULL_POINTER) ? "ms" : ((stdlib__strchr(expanded, 's') != NULL_POINTER) ? "s" : ""))));
                    stdlib__strcpy_safe(expanded, sizeof(expanded), number);
                    length = (int64)stdlib__strlen(expanded);
                    stdlib__strcpy_safe(expanded + length, sizeof(expanded) - length, replacement);
                }
            }
            length = (int64)stdlib__strlen(expanded);
            if (length > cap - at - 1)
            {
                length = cap - at - 1;
            }
            stdlib__memcpy(out + at, expanded, length);
            at += length;
        }
        else
        {
            out[at++] = *p++;
        }
    }
    out[at] = 0;
}

/**
 * @brief read an extended value after variable and calc substitution
 * @param gui_node_t* node -> target node
 * @param const char* property -> property name
 * @param char* out -> destination
 * @param int64 capacity -> destination capacity
 * @return boole -> TRUE when the property exists
 */
boole motion__value_id(gui_node_t* node, int id, char* out, int64 capacity)
{
    const char* raw = NULL_POINTER;

    if (capacity > 0)
    {
        out[0] = 0;
    }
    if (_motion_internal__active == FALSE)
    {
        return FALSE;
    }
    raw = _motion_internal__raw(node, id);
    if (raw == NULL_POINTER)
    {
        return FALSE;
    }
    _motion_internal__expand(node, raw, out, capacity, 0);
    return TRUE;
}

boole motion__value(gui_node_t* node, const char* property, char* out, int64 capacity)
{
    if (capacity > 0)
    {
        out[0] = 0;
    }
    if (_motion_internal__active == FALSE)
    {
        return FALSE;
    }
    return motion__value_id(node, motion__prop_id(property), out, capacity);
}

/**
 * @brief interpolate numeric CSS value lists with matching grammar
 * @param const char* a -> starting expression
 * @param const char* b -> ending expression
 * @param float t -> interpolation factor
 * @param char* out -> resulting expression
 * @param int64 cap -> destination size
 * @return void
 */
static void _motion_internal__blend(const char* a, const char* b, float t, char* out, int64 cap)
{
    const char* pa = a;
    const char* pb = b;
    char* ea = NULL_POINTER;
    char* eb = NULL_POINTER;
    float va = 0.0f;
    float vb = 0.0f;
    int64 at = 0;
    int64 length = 0;
    char number[64];
    gui_color_t ca;
    gui_color_t cb;
    float channels[4];
    int i = 0;
    const char* unit_a = NULL_POINTER;
    const char* unit_b = NULL_POINTER;
    const char* unit = NULL_POINTER;
    int64 unit_length_a = 0;
    int64 unit_length_b = 0;

    stdlib__null_memory(number, sizeof(number));
    stdlib__null_memory(&ca, sizeof(ca));
    stdlib__null_memory(&cb, sizeof(cb));
    stdlib__null_memory(channels, sizeof(channels));
    if (a == NULL_POINTER || b == NULL_POINTER || cap <= 0)
    {
        return;
    }
    if (*a == '#' && *b == '#' && parser_style__parse_color(a, &ca) == TRUE && parser_style__parse_color(b, &cb) == TRUE)
    {
        channels[0] = (ca.r + (cb.r - ca.r) * t) * 255.0f;
        channels[1] = (ca.g + (cb.g - ca.g) * t) * 255.0f;
        channels[2] = (ca.b + (cb.b - ca.b) * t) * 255.0f;
        channels[3] = ca.a + (cb.a - ca.a) * t;
        stdlib__strcpy_safe(out, cap, "rgba(");
        at = (int64)stdlib__strlen(out);
        for (i = 0; i < 4; i++)
        {
            motion__format(channels[i], number, sizeof(number));
            stdlib__strcpy_safe(out + at, cap - at, number);
            at = (int64)stdlib__strlen(out);
            if (at + 1 < cap)
            {
                out[at++] = (i == 3) ? ')' : ',';
                out[at] = 0;
            }
        }
        return;
    }
    while (*pa != 0 && *pb != 0 && at + 1 < cap)
    {
        va = stdlib__strtof(pa, &ea);
        vb = stdlib__strtof(pb, &eb);
        if (ea != pa && eb != pb)
        {
            unit_a = ea;
            unit_b = eb;
            while ((*ea >= 'a' && *ea <= 'z') || *ea == '%')
            {
                ea++;
            }
            while ((*eb >= 'a' && *eb <= 'z') || *eb == '%')
            {
                eb++;
            }
            unit_length_a = ea - unit_a;
            unit_length_b = eb - unit_b;
            unit = unit_a;
            if (unit_length_a != unit_length_b || stdlib__strncmp(unit_a, unit_b, unit_length_a) != 0)
            {
                if (va == 0.0f && unit_length_a == 0)
                {
                    unit = unit_b;
                    unit_length_a = unit_length_b;
                }
                else if (vb != 0.0f || unit_length_b != 0)
                {
                    stdlib__strcpy_safe(out, cap, (t < 0.5f) ? a : b);
                    return;
                }
            }
            motion__format(va + (vb - va) * t, number, sizeof(number));
            length = (int64)stdlib__strlen(number);
            if (at + length + unit_length_a >= cap)
            {
                break;
            }
            stdlib__memcpy(out + at, number, length);
            at += length;
            stdlib__memcpy(out + at, unit, unit_length_a);
            at += unit_length_a;
            pa = ea;
            pb = eb;
        }
        else if (*pa <= ' ' || *pb <= ' ')
        {
            pa = _motion_internal__space(pa);
            pb = _motion_internal__space(pb);
            if (at > 0 && out[at - 1] != ' ' && out[at - 1] != '(' && out[at - 1] != ',')
            {
                out[at++] = ' ';
            }
        }
        else if (*pa == *pb)
        {
            out[at++] = *pa++;
            pb++;
        }
        else
        {
            stdlib__strcpy_safe(out, cap, (t < 0.5f) ? a : b);
            return;
        }
    }
    out[at] = 0;
    if (*pa != 0 || *pb != 0)
    {
        stdlib__strcpy_safe(out, cap, (t < 0.5f) ? a : b);
    }
}

/**
 * @brief evaluate CSS easing keywords, cubic-bezier and steps
 * @param float t -> normalized input progress
 * @param const char* easing -> timing-function expression
 * @return float -> eased progress, including permitted cubic overshoot
 */
static float _motion_internal__ease(float t, const char* easing)
{
    float x1 = 0.25f;
    float y1 = 0.1f;
    float x2 = 0.25f;
    float y2 = 1.0f;
    float low = 0.0f;
    float high = 1.0f;
    float u = 0.0f;
    float x = 0.0f;
    float steps = 1.0f;
    float* params[4];
    const char* p = easing;
    char* end = NULL_POINTER;
    int i = 0;

    stdlib__null_memory(params, sizeof(params));
    t = stdlib__fminf(1.0f, stdlib__fmaxf(0.0f, t));
    if (stdlib__strcmp(easing, "linear") == 0)
    {
        return t;
    }
    if (stdlib__strncmp(easing, "steps(", 6) == 0 || stdlib__strncmp(easing, "step-", 5) == 0)
    {
        steps = (easing[4] == 's') ? stdlib__fmaxf(1.0f, motion__number(easing + 6, 1.0f)) : 1.0f;
        u = stdlib__floorf(t * steps);
        if (stdlib__strstr(easing, "start") != NULL_POINTER)
        {
            u += 1.0f;
        }
        return stdlib__fminf(1.0f, u / steps);
    }
    if (stdlib__strcmp(easing, "ease-in") == 0)
    {
        x1 = 0.42f;
        y1 = 0.0f;
        x2 = 1.0f;
    }
    else if (stdlib__strcmp(easing, "ease-out") == 0)
    {
        x1 = 0.0f;
        y1 = 0.0f;
        x2 = 0.58f;
    }
    else if (stdlib__strcmp(easing, "ease-in-out") == 0)
    {
        x1 = 0.42f;
        y1 = 0.0f;
        x2 = 0.58f;
    }
    else if (stdlib__strncmp(easing, "cubic-bezier(", 13) == 0)
    {
        params[0] = &x1;
        params[1] = &y1;
        params[2] = &x2;
        params[3] = &y2;
        p += 13;
        for (i = 0; i < 4; i++)
        {
            p = _motion_internal__space(p);
            *params[i] = stdlib__strtof(p, &end);
            p = _motion_internal__space(end);
            if (*p == ',')
            {
                p++;
            }
        }
    }
    for (i = 0; i < 22; i++)
    {
        u = (low + high) * 0.5f;
        x = 3.0f * (1.0f - u) * (1.0f - u) * u * x1 + 3.0f * (1.0f - u) * u * u * x2 + u * u * u;
        if (x < t)
        {
            low = u;
        }
        else
        {
            high = u;
        }
    }
    return 3.0f * (1.0f - u) * (1.0f - u) * u * y1 + 3.0f * (1.0f - u) * u * u * y2 + u * u * u;
}

/**
 * @brief calculate scroll or view timeline progress from laid-out scene boxes
 * @param gui_node_t* node -> animation subject
 * @param const char* timeline -> scroll() or view() expression
 * @return float -> progress between zero and one
 */
static float _motion_internal__timeline(gui_node_t* node, const char* timeline)
{
    gui_node_t* viewport = node->parent;
    float range = 0.0f;
    float progress = 0.0f;

    while (viewport != NULL_POINTER && viewport->parent != NULL_POINTER && viewport->resolved.overflow_y == GUI_OVERFLOW__VISIBLE)
    {
        viewport = viewport->parent;
    }
    if (stdlib__strstr(timeline, "root") != NULL_POINTER || viewport == NULL_POINTER)
    {
        viewport = scene__root();
    }
    if (viewport == NULL_POINTER)
    {
        return 0.0f;
    }
    if (stdlib__strncmp(timeline, "view", 4) == 0)
    {
        range = viewport->bounds.h + node->bounds.h;
        progress = (range > 0.0f) ? (viewport->bounds.y + viewport->bounds.h - node->bounds.y) / range : 0.0f;
    }
    else
    {
        range = viewport->content_h - viewport->bounds.h;
        progress = (range > 0.0f) ? viewport->scroll_y / range : 0.0f;
    }
    return stdlib__fminf(1.0f, stdlib__fmaxf(0.0f, progress));
}

/**
 * @brief apply named CSS animations in list order to a node's computed values
 * @param _motion_internal__state_t* state -> node timeline and value table
 * @return void
 */
static void _motion_internal__animate(_motion_internal__state_t* state)
{
    char animations[_MOTION_INTERNAL__VALUE_CAP];
    char item[512];
    char token[128];
    char name[64];
    char easing[128];
    char timeline[128];
    char from[_MOTION_INTERNAL__VALUE_CAP];
    char to[_MOTION_INTERNAL__VALUE_CAP];
    char mixed[_MOTION_INTERNAL__VALUE_CAP];
    int properties[_MOTION_INTERNAL__MAX_VALUES];
    const char* p = NULL_POINTER;
    const char* finish = NULL_POINTER;
    const char* cursor = NULL_POINTER;
    const char* next = NULL_POINTER;
    const char* a = NULL_POINTER;
    const char* b = NULL_POINTER;
    const char* raw = NULL_POINTER;
    char* number_end = NULL_POINTER;
    _motion_internal__frame_t* frame = NULL_POINTER;
    _motion_internal__decl_t* declaration = NULL_POINTER;
    float duration = 0.0f;
    float delay = 0.0f;
    float iterations = 1.0f;
    float value = 0.0f;
    float cycle = 0.0f;
    float progress = 0.0f;
    float low = 0.0f;
    float high = 1.0f;
    float factor = 0.0f;
    int times = 0;
    int count = 0;
    int i = 0;
    int j = 0;
    int animation = 0;
    boole alternate = FALSE;
    boole reverse = FALSE;
    boole forwards = FALSE;
    boole backwards = FALSE;
    boole running = FALSE;

    stdlib__null_memory(animations, sizeof(animations));
    stdlib__null_memory(item, sizeof(item));
    stdlib__null_memory(token, sizeof(token));
    stdlib__null_memory(name, sizeof(name));
    stdlib__null_memory(easing, sizeof(easing));
    stdlib__null_memory(timeline, sizeof(timeline));
    stdlib__null_memory(from, sizeof(from));
    stdlib__null_memory(to, sizeof(to));
    stdlib__null_memory(mixed, sizeof(mixed));
    stdlib__null_memory(properties, sizeof(properties));
    if (motion__value_id(state->node, MOTION_PROP__ANIMATION, animations, sizeof(animations)) == FALSE)
    {
        if (motion__value_id(state->node, MOTION_PROP__ANIMATION_NAME, animations, sizeof(animations)) == FALSE)
        {
            return;
        }
    }
    motion__value_id(state->node, MOTION_PROP__ANIMATION_TIMELINE, timeline, sizeof(timeline));
    p = animations;
    while (*p != 0 && animation++ < _MOTION_INTERNAL__MAX_ANIMATIONS)
    {
        finish = _motion_internal__end(p, ',');
        _motion_internal__copy(item, sizeof(item), p, finish);
        p = finish + ((*finish == ',') ? 1 : 0);
        name[0] = 0;
        stdlib__strcpy_safe(easing, sizeof(easing), "ease");
        duration = 0.0f;
        delay = 0.0f;
        iterations = 1.0f;
        times = 0;
        alternate = FALSE;
        reverse = FALSE;
        forwards = FALSE;
        backwards = FALSE;
        cursor = item;
        while (*(cursor = _motion_internal__space(cursor)) != 0)
        {
            next = _motion_internal__end(cursor, ' ');
            _motion_internal__copy(token, sizeof(token), cursor, next);
            cursor = next;
            value = stdlib__strtof(token, &number_end);
            if (number_end != token && (*number_end == 's' || stdlib__strncmp(number_end, "ms", 2) == 0))
            {
                value *= (*number_end == 's') ? 1000.0f : 1.0f;
                if (times++ == 0)
                {
                    duration = value;
                }
                else
                {
                    delay = value;
                }
            }
            else if (number_end != token && *number_end == 0)
            {
                iterations = value;
            }
            else if (stdlib__strcmp(token, "infinite") == 0)
            {
                iterations = -1.0f;
            }
            else if (stdlib__strcmp(token, "alternate") == 0 || stdlib__strcmp(token, "alternate-reverse") == 0)
            {
                alternate = TRUE;
                reverse = (boole)(stdlib__strcmp(token, "alternate-reverse") == 0);
            }
            else if (stdlib__strcmp(token, "reverse") == 0)
            {
                reverse = TRUE;
            }
            else if (stdlib__strcmp(token, "forwards") == 0 || stdlib__strcmp(token, "both") == 0)
            {
                forwards = TRUE;
                backwards = (boole)(stdlib__strcmp(token, "both") == 0);
            }
            else if (stdlib__strcmp(token, "backwards") == 0)
            {
                backwards = TRUE;
            }
            else if (stdlib__strncmp(token, "ease", 4) == 0 || stdlib__strcmp(token, "linear") == 0 || stdlib__strncmp(token, "steps(", 6) == 0 || stdlib__strncmp(token, "step-", 5) == 0 || stdlib__strncmp(token, "cubic-bezier(", 13) == 0)
            {
                stdlib__strcpy_safe(easing, sizeof(easing), token);
            }
            else if (stdlib__strcmp(token, "normal") != 0 && stdlib__strcmp(token, "none") != 0)
            {
                stdlib__strcpy_safe(name, sizeof(name), token);
            }
        }
        if (motion__value_id(state->node, MOTION_PROP__ANIMATION_DURATION, token, sizeof(token)) == TRUE)
        {
            duration = motion__number(token, 1.0f) * ((stdlib__strstr(token, "ms") != NULL_POINTER) ? 1.0f : 1000.0f);
        }
        if (motion__value_id(state->node, MOTION_PROP__ANIMATION_DELAY, token, sizeof(token)) == TRUE)
        {
            delay = motion__number(token, 1.0f) * ((stdlib__strstr(token, "ms") != NULL_POINTER) ? 1.0f : 1000.0f);
        }
        if (_motion_internal__reduce == TRUE)
        {
            duration = stdlib__fmaxf(duration, 40000.0f);
        }
        running = TRUE;
        if (name[0] != 0 && (timeline[0] == 0 || stdlib__strcmp(timeline, "auto") == 0) && (state->age < delay || (duration > 0.0f && (iterations < 0.0f || state->age - delay < duration * iterations))))
        {
            _motion_internal__animating = TRUE;
        }
        if (timeline[0] != 0 && stdlib__strcmp(timeline, "auto") != 0)
        {
            cycle = 0.0f;
            progress = _motion_internal__timeline(state->node, timeline);
        }
        else if (state->age < delay)
        {
            cycle = 0.0f;
            progress = 0.0f;
            running = backwards;
        }
        else
        {
            value = (duration > 0.0f) ? (state->age - delay) / duration : 1.0f;
            cycle = stdlib__floorf(value);
            progress = value - cycle;
            if (iterations >= 0.0f && value >= iterations)
            {
                cycle = stdlib__fmaxf(0.0f, stdlib__ceilf(iterations) - 1.0f);
                progress = iterations - cycle;
                running = forwards;
            }
        }
        if (running == FALSE || name[0] == 0)
        {
            continue;
        }
        if ((alternate == TRUE && (((uint64)(int64)cycle & 1) != 0)) != (reverse == TRUE))
        {
            progress = 1.0f - progress;
        }
        count = 0;
        for (frame = _motion_internal__frames; frame != NULL_POINTER; frame = frame->next)
        {
            if (stdlib__strcmp(frame->name, name) != 0)
            {
                continue;
            }
            for (declaration = frame->declarations; declaration != NULL_POINTER; declaration = declaration->next)
            {
                for (i = 0; i < count && properties[i] != declaration->id; i++)
                {
                }
                if (i == count && count < _MOTION_INTERNAL__MAX_VALUES)
                {
                    properties[count++] = declaration->id;
                }
            }
        }
        for (j = 0; j < count; j++)
        {
            raw = _motion_internal__raw(state->node, properties[j]);
            a = raw;
            b = raw;
            low = 0.0f;
            high = 1.0f;
            for (frame = _motion_internal__frames; frame != NULL_POINTER; frame = frame->next)
            {
                if (stdlib__strcmp(frame->name, name) != 0)
                {
                    continue;
                }
                raw = _motion_internal__decl_value_id(frame->declarations, properties[j]);
                if (raw == NULL_POINTER)
                {
                    continue;
                }
                if (frame->offset <= progress && frame->offset >= low)
                {
                    low = frame->offset;
                    a = raw;
                }
                if (frame->offset >= progress && frame->offset <= high)
                {
                    high = frame->offset;
                    b = raw;
                }
            }
            if (a == NULL_POINTER)
            {
                a = (properties[j] == MOTION_PROP__OPACITY) ? "1" : "0";
                if (properties[j] == MOTION_PROP__BACKGROUND_POSITION || properties[j] == MOTION_PROP__MASK_POSITION)
                {
                    a = "0px 0px";
                }
                else if (properties[j] == MOTION_PROP__FILTER)
                {
                    a = "hue-rotate(0deg)";
                }
                else if (properties[j] == MOTION_PROP__TRANSFORM)
                {
                    a = "rotate(0deg)";
                }
            }
            if (b == NULL_POINTER)
            {
                b = a;
            }
            _motion_internal__expand(state->node, a, from, sizeof(from), 0);
            _motion_internal__expand(state->node, b, to, sizeof(to), 0);
            factor = (high > low) ? (progress - low) / (high - low) : 1.0f;
            factor = _motion_internal__ease(factor, easing);
            _motion_internal__blend(from, to, factor, mixed, sizeof(mixed));
            _motion_internal__set(state, properties[j], mixed);
        }
    }
}

/**
 * @brief detect changes to every input used by selector matching
 * @param gui_node_t* node -> subtree root
 * @return boole -> FALSE when the bounded cache cannot cover this tree
 */
static boole _motion_internal__snapshot(gui_node_t* node, int parent_index)
{
    _motion_internal__selector_key_t key;
    _motion_internal__selector_key_t* previous = NULL_POINTER;
    _motion_internal__match_cache_t* grown = NULL_POINTER;
    gui_node_t* child = NULL_POINTER;
    int capacity = 0;
    int index = _motion_internal__match_cursor;
    boole state_only = FALSE;

    if (_motion_internal__match_cursor >= _MOTION_INTERNAL__MATCH_NODE_CAP)
    {
        return FALSE;
    }
    if (_motion_internal__match_cursor >= _motion_internal__match_capacity)
    {
        capacity = (_motion_internal__match_capacity == 0) ? 128 : _motion_internal__match_capacity * 2;
        grown = GUI_REALLOC(_motion_internal__matches, (int64)capacity * sizeof(*grown));
        if (grown == NULL_POINTER)
        {
            return FALSE;
        }
        stdlib__null_memory(grown + _motion_internal__match_capacity, (int64)(capacity - _motion_internal__match_capacity) * sizeof(*grown));
        _motion_internal__matches = grown;
        _motion_internal__match_capacity = capacity;
    }
    stdlib__null_memory(&key, sizeof(key));
    key.node = node;
    key.parent = node->parent;
    key.first_child = node->first_child;
    key.next_sibling = node->next_sibling;
    key.state = node->state;
    key.checked = (boole)((node->type == GUI_NODE_TYPE__OPTION || node->type == GUI_NODE_TYPE__CHECKBOX || node->type == GUI_NODE_TYPE__RADIO) && node->value >= 0.5f);
    key.type = node->type;
    stdlib__memcpy(key.id, node->id, sizeof(key.id));
    stdlib__memcpy(key.klass, node->klass, sizeof(key.klass));
    stdlib__strcpy_safe(key.name, sizeof(key.name), widget_registry__type_name(node->type));
    previous = &_motion_internal__matches[index].key;
    if (stdlib__memcmp(previous, &key, sizeof(key)) != 0)
    {
        // only the interaction state differs: a local invalidation is enough
        state_only = (boole)(previous->node == key.node && previous->parent == key.parent && previous->first_child == key.first_child && previous->next_sibling == key.next_sibling && previous->type == key.type && stdlib__memcmp(previous->id, key.id, sizeof(key.id)) == 0 && stdlib__memcmp(previous->klass, key.klass, sizeof(key.klass)) == 0 && stdlib__memcmp(previous->name, key.name, sizeof(key.name)) == 0);
        if (state_only == TRUE && _motion_internal__stale_count < _MOTION_INTERNAL__STALE_CAP)
        {
            _motion_internal__stale_indices[_motion_internal__stale_count++] = index;
        }
        else
        {
            _motion_internal__match_changed = TRUE;
        }
        *previous = key;
    }
    _motion_internal__matches[index].parent_index = parent_index;
    _motion_internal__match_cursor++;
    for (child = node->first_child; child != NULL_POINTER; child = child->next_sibling)
    {
        if (_motion_internal__snapshot(child, index) == FALSE)
        {
            return FALSE;
        }
    }
    _motion_internal__matches[index].subtree = _motion_internal__match_cursor - index;
    return TRUE;
}

/**
 * @brief flag the entries a state change can affect: the node, its ancestors (:has) and its subtree (descendant combinators)
 * @return void
 */
static void _motion_internal__mark_stale(void)
{
    int i = 0;
    int index = 0;
    int end = 0;
    int walk = 0;

    for (i = 0; i < _motion_internal__stale_count; i++)
    {
        index = _motion_internal__stale_indices[i];
        end = index + _motion_internal__matches[index].subtree;
        for (walk = index; walk < end && walk < _motion_internal__match_cursor; walk++)
        {
            _motion_internal__matches[walk].stale = TRUE;
        }
        for (walk = _motion_internal__matches[index].parent_index; walk >= 0; walk = _motion_internal__matches[walk].parent_index)
        {
            _motion_internal__matches[walk].stale = TRUE;
        }
    }
}

/**
 * @brief cascade cached selector matches and advance every node's timeline
 * @param gui_node_t* node -> subtree root
 * @param float delta -> elapsed milliseconds, including for offscreen nodes
 * @return void
 */
static void _motion_internal__visit(gui_node_t* node, float delta)
{
    _motion_internal__state_t* state = NULL_POINTER;
    _motion_internal__rule_t* rule = NULL_POINTER;
    _motion_internal__decl_t* declaration = NULL_POINTER;
    gui_node_t* child = NULL_POINTER;
    char value[_MOTION_INTERNAL__VALUE_CAP];
    int tier = 0;
    _motion_internal__match_cache_t* cache = NULL_POINTER;
    boole matched = FALSE;
    boole had_state = FALSE;

    value[0] = 0;
    if (_motion_internal__match_enabled == TRUE)
    {
        cache = &_motion_internal__matches[_motion_internal__match_cursor++];
        if (cache->generation != _motion_internal__match_generation || cache->stale == TRUE)
        {
            stdlib__null_memory(cache->matches, sizeof(cache->matches));
            for (rule = _motion_internal__rules; rule != NULL_POINTER; rule = rule->next)
            {
                if (motion__matches(node, rule->selector) == TRUE)
                {
                    cache->matches[rule->index / 64] |= (uint64)1 << (rule->index % 64);
                }
            }
            cache->generation = _motion_internal__match_generation;
            cache->stale = FALSE;
        }
    }
    state = _motion_internal__state(node, FALSE);
    had_state = (boole)(state != NULL_POINTER);
    if (state != NULL_POINTER)
    {
        state->count = 0;
        stdlib__null_memory(state->slot, sizeof(state->slot));
    }
    for (tier = 0; tier < 3; tier++)
    {
        for (rule = _motion_internal__rules; rule != NULL_POINTER; rule = rule->next)
        {
            if (tier != rule->specificity)
            {
                continue;
            }
            matched = (cache != NULL_POINTER) ? (boole)((cache->matches[rule->index / 64] & ((uint64)1 << (rule->index % 64))) != 0) : motion__matches(node, rule->selector);
            if (matched == FALSE)
            {
                continue;
            }
            if (state == NULL_POINTER)
            {
                state = _motion_internal__state(node, TRUE);
            }
            if (state == NULL_POINTER)
            {
                break;
            }
            for (declaration = rule->declarations; declaration != NULL_POINTER; declaration = declaration->next)
            {
                _motion_internal__set(state, declaration->id, declaration->value);
            }
        }
    }
    if (state != NULL_POINTER)
    {
        // Newly visible/restarted items must display their first keyframe.
        // Time spent drawing the preceding page is not their animation age.
        if (had_state == TRUE)
        {
            state->age += delta;
        }
        _motion_internal__animate(state);
        if (motion__value_id(node, MOTION_PROP__BACKGROUND_COLOR, value, sizeof(value)) == TRUE)
        {
            node->resolved.has_background_color = parser_style__parse_color(value, &node->resolved.background_color);
        }
        if (motion__value_id(node, MOTION_PROP__OPACITY, value, sizeof(value)) == TRUE)
        {
            node->resolved.opacity = motion__number(value, 1.0f);
            node->resolved.has_opacity = TRUE;
        }
        if (motion__value_id(node, MOTION_PROP__COLOR, value, sizeof(value)) == TRUE || motion__value_id(node, MOTION_PROP__FONT_COLOR, value, sizeof(value)) == TRUE)
        {
            node->resolved.has_font_color = parser_style__parse_color(value, &node->resolved.font_color);
        }
        if (motion__value_id(node, MOTION_PROP__WIDTH, value, sizeof(value)) == TRUE)
        {
            node->resolved.size_w = _motion_internal__length(node, value, (node->parent != NULL_POINTER) ? node->parent->bounds.w / scene__scale() : 0.0f);
            node->resolved.width_pct = 0.0f;
            node->resolved.size_w_explicit = TRUE;
        }
        if (motion__value_id(node, MOTION_PROP__HEIGHT, value, sizeof(value)) == TRUE)
        {
            node->resolved.size_h = _motion_internal__length(node, value, (node->parent != NULL_POINTER) ? node->parent->bounds.h / scene__scale() : 0.0f);
            node->resolved.height_pct = 0.0f;
            node->resolved.size_h_explicit = TRUE;
        }
        if (motion__value_id(node, MOTION_PROP__BORDER_COLOR, value, sizeof(value)) == TRUE)
        {
            node->resolved.has_border_color = parser_style__parse_color(value, &node->resolved.border_color);
        }
        if (motion__value_id(node, MOTION_PROP__TOP, value, sizeof(value)) == TRUE)
        {
            node->resolved.inset_t = _motion_internal__length(node, value, (node->parent != NULL_POINTER) ? node->parent->bounds.h / scene__scale() : 0.0f);
            node->resolved.inset_t_pct = 0.0f;
            node->resolved.inset_t_set = TRUE;
        }
        if (motion__value_id(node, MOTION_PROP__LEFT, value, sizeof(value)) == TRUE)
        {
            node->resolved.inset_l = _motion_internal__length(node, value, (node->parent != NULL_POINTER) ? node->parent->bounds.w / scene__scale() : 0.0f);
            node->resolved.inset_l_pct = 0.0f;
            node->resolved.inset_l_set = TRUE;
        }
    }
    if (node->resolved.has_font_color == FALSE && node->parent != NULL_POINTER)
    {
        node->resolved.font_color = node->parent->resolved.font_color;
        node->resolved.has_font_color = node->parent->resolved.has_font_color;
    }
    for (child = node->first_child; child != NULL_POINTER; child = child->next_sibling)
    {
        _motion_internal__visit(child, delta);
    }
}

/**
 * @brief advance CSS keyframes after normal state transitions and before layout
 * @return void
 */
void motion__tick(void)
{
    gui_node_t* root = scene__root();

    _motion_internal__animating = FALSE;
    if (_motion_internal__active == TRUE && root != NULL_POINTER)
    {
        // Animation values change every tick; selector membership usually does
        // not. Compare the exact selector inputs once for the whole tree. Any
        // change invalidates all memberships, covering :has, ancestors, focus
        // and sibling order without pausing or approximating any timeline.
        _motion_internal__match_cursor = 0;
        _motion_internal__stale_count = 0;
        _motion_internal__match_changed = (boole)(_motion_internal__match_focus != scene__focus());
        _motion_internal__match_focus = scene__focus();
        _motion_internal__match_enabled = (boole)(_motion_internal__rule_count <= _MOTION_INTERNAL__MATCH_RULE_CAP && _motion_internal__snapshot(root, -1) == TRUE);
        if (_motion_internal__match_changed == TRUE || _motion_internal__match_count != _motion_internal__match_cursor || _motion_internal__match_enabled == FALSE)
        {
            _motion_internal__match_generation++;
        }
        else if (_motion_internal__stale_count > 0)
        {
            _motion_internal__mark_stale();
        }
        _motion_internal__match_count = _motion_internal__match_cursor;
        _motion_internal__match_cursor = 0;
        _motion_internal__visit(root, (float)scene__frame_delta_ms());
    }
}

/**
 * @brief set the host's reduced-motion preference
 * @param boole enabled -> slow ambient animations when TRUE
 * @return void
 */
void motion__set_reduced_motion(boole enabled)
{
    _motion_internal__reduce = enabled;
}

/**
 * @brief query the reduced-motion preference
 * @return boole -> current preference
 */
boole motion__reduced_motion(void)
{
    return _motion_internal__reduce;
}

/**
 * @brief report whether the loaded stylesheet needs extended rendering
 * @return boole -> TRUE when CSS motion or paint extensions are present
 */
boole motion__active(void)
{
    return _motion_internal__active;
}

boole motion__needs_compositor(void)
{
    return (boole)(_motion_internal__active == TRUE && _motion_internal__compositor_required == TRUE);
}

/**
 * @brief report whether keyframe timelines need further frames
 * @return boole -> TRUE when the stylesheet defines animations
 */
boole motion__animating(void)
{
    return _motion_internal__animating;
}
