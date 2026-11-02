#include "scene_text.h"
#include "clipboard.h"
#include "clib/memory_manager.h"
#include "clib/stdlib.h"

static void scene_text__clear(char* text, int64 length)
{
    volatile char* bytes = text;
    int64 index = 0;
    for (index = 0; index < length; index++)
    {
        bytes[index] = 0;
    }
}

static void scene_text__wipe(gui_node_t* node)
{
    // Revealing a password changes its appearance, not its storage lifetime.
    scene_text__clear(node->text, node->text_capacity);
}

boole scene__reserve_text(gui_node_t* node, int64 length)
{
    if (node == NULL_POINTER || length < 0 || length > node->text_limit)
    {
        return FALSE;
    }
    if (length < node->text_capacity)
    {
        return TRUE;
    }
    int64 capacity = node->text_capacity;
    while (capacity <= length)
    {
        capacity = capacity > (node->text_limit + 1) / 2 ? node->text_limit + 1 : capacity * 2;
    }
    char* grown = GUI_CALLOC((size_t)capacity, 1);
    if (grown == NULL_POINTER)
    {
        return FALSE;
    }
    // Include direct legacy writes within the existing buffer as well.
    stdlib__memcpy(grown, node->text, stdlib__strlen(node->text) + 1);
    scene_text__wipe(node);
    if (node->text != node->text_inline)
    {
        GUI_FREE(node->text);
    }
    node->text = grown;
    node->text_capacity = capacity;
    return TRUE;
}

boole scene__set_text_limit(gui_node_t* node, int64 limit)
{
    if (node == NULL_POINTER || limit < 0 || limit > MEEK_UI_MAX_TEXT_BYTES ||
        (int64)stdlib__strlen(node->text) > limit)
    {
        return FALSE;
    }
    node->text_limit = limit;
    return TRUE;
}

void scene_text__release(gui_node_t* node)
{
    scene_text__wipe(node);
    if (node->text != node->text_inline)
    {
        GUI_FREE(node->text);
    }
    scene_text__clear(node->text_inline, sizeof(node->text_inline));
    node->text = node->text_inline;
    node->text_capacity = sizeof(node->text_inline);
    node->text[0] = 0;
    node->text_len = 0;
}

void scene_text__move(gui_node_t* destination, gui_node_t* source)
{
    scene_text__release(destination);
    destination->text_limit = source->text_limit;
    destination->text_len = (int64)stdlib__strlen(source->text);
    if (source->text == source->text_inline)
    {
        stdlib__memcpy(destination->text, source->text, (size_t)destination->text_len + 1);
        scene_text__wipe(source);
    }
    else
    {
        destination->text = source->text;
        destination->text_capacity = source->text_capacity;
    }
    scene_text__clear(source->text_inline, sizeof(source->text_inline));
    source->text = source->text_inline;
    source->text_capacity = sizeof(source->text_inline);
    source->text_len = 0;
    source->text[0] = 0;
}

int scene_text__encode(uint cp, char encoded[4])
{
    if (cp < 32 || cp == 127 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    {
        return 0;
    }
    if (cp < 0x80)
    {
        encoded[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        encoded[0] = (char)(0xc0 | (cp >> 6));
        encoded[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000)
    {
        encoded[0] = (char)(0xe0 | (cp >> 12));
        encoded[1] = (char)(0x80 | ((cp >> 6) & 63));
        encoded[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    encoded[0] = (char)(0xf0 | (cp >> 18));
    encoded[1] = (char)(0x80 | ((cp >> 12) & 63));
    encoded[2] = (char)(0x80 | ((cp >> 6) & 63));
    encoded[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static uint scene_text__fingerprint(gui_node_t* n)
{
    uint hash = 2166136261u;
    for (int64 i = 0; i < n->text_len; i++)
    {
        hash = (hash ^ (ubyte)n->text[i]) * 16777619u;
    }
    return hash;
}

scene_text_state_t* scene_text__state(gui_node_t* n)
{
    scene_text_state_t* state = scene__widget_state(n);
    if (state == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    n->text[n->text_capacity - 1] = 0;
    n->text_len = (int64)stdlib__strlen(n->text);
    uint hash = scene_text__fingerprint(n);
    // Hosts can replace text directly (e.g. clearing a submitted password).
    if (state->initialized == FALSE || state->fingerprint != hash)
    {
        state->caret = n->text_len;
        state->anchor = state->caret;
        state->fingerprint = hash;
        state->initialized = TRUE;
        state->reveal_caret = TRUE;
        state->has_preferred_x = FALSE;
        state->prefer_line_end = FALSE;
    }
    return state;
}

boole scene__set_text(gui_node_t* n, const char* text, int64 length)
{
    scene_text_state_t* state = NULL_POINTER;
    if (n == NULL_POINTER || length < 0 || length > n->text_limit || (text == NULL_POINTER && length != 0))
    {
        return FALSE;
    }
    for (int64 i = 0; i < length;)
    {
        ubyte first = (ubyte)text[i];
        int bytes = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                                : first >= 0xe0 && first <= 0xef ? 3
                                : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (bytes == 0 || bytes > length - i)
        {
            return FALSE;
        }
        uint cp = bytes == 1 ? first : first & (0x7f >> bytes);
        for (int j = 1; j < bytes; j++)
        {
            ubyte next = (ubyte)text[i + j];
            if ((next & 0xc0) != 0x80)
            {
                return FALSE;
            }
            cp = (cp << 6) | (next & 63);
        }
        if (cp == 0 || (bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) ||
            (bytes == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        {
            return FALSE;
        }
        if (n->type == GUI_NODE_TYPE__INPUT && (cp < 32 || cp == 127))
        {
            return FALSE;
        }
        i += bytes;
    }
    if (n->type == GUI_NODE_TYPE__INPUT || n->type == GUI_NODE_TYPE__TEXTAREA)
    {
        state = n->user_data;
    }
    if (scene__reserve_text(n, length) == FALSE)
    {
        return FALSE;
    }
    if (length > 0)
    {
        stdlib__memmove(n->text, text, (size_t)length);
    }
    scene_text__clear(n->text + length, n->text_capacity - length);
    n->text_len = length;
    n->layout_memo_frame = ~(uint64)0;
    if (state != NULL_POINTER)
    {
        state->caret = length;
        state->anchor = length;
        state->fingerprint = scene_text__fingerprint(n);
        state->initialized = TRUE;
        state->reveal_caret = TRUE;
        state->has_preferred_x = FALSE;
        state->prefer_line_end = FALSE;
        state->scroll_x = 0.0f;
    }
    return TRUE;
}

int64 scene_text__previous(gui_node_t* n, int64 at)
{
    if (at > 0)
    {
        at--;
        while (at > 0 && ((ubyte)n->text[at] & 0xc0) == 0x80)
        {
            at--;
        }
    }
    return at;
}

int64 scene_text__next(gui_node_t* n, int64 at)
{
    if (at < n->text_len)
    {
        at++;
        while (at < n->text_len && ((ubyte)n->text[at] & 0xc0) == 0x80)
        {
            at++;
        }
    }
    return at;
}

static void scene_text__changed(gui_node_t* n, scene_text_state_t* state)
{
    state->fingerprint = scene_text__fingerprint(n);
    state->reveal_caret = TRUE;
    state->has_preferred_x = FALSE;
    state->prefer_line_end = FALSE;
    n->layout_memo_frame = ~(uint64)0;
    scene__dispatch_change(n, (float)n->text_len);
}

void scene_text__replace(gui_node_t* n, scene_text_state_t* state, const char* bytes, int64 length)
{
    int64 start = state->caret < state->anchor ? state->caret : state->anchor;
    int64 end = state->caret > state->anchor ? state->caret : state->anchor;
    if ((start == end && length == 0) ||
        scene__reserve_text(n, n->text_len - (end - start) + length) == FALSE)
    {
        return;
    }
    stdlib__memmove(n->text + start + length, n->text + end, (size_t)(n->text_len - end + 1));
    if (length)
    {
        stdlib__memcpy(n->text + start, bytes, (size_t)length);
    }
    n->text_len += length - (end - start);
    scene_text__clear(n->text + n->text_len, n->text_capacity - n->text_len);
    state->caret = start + length;
    state->anchor = state->caret;
    scene_text__changed(n, state);
}

static void scene_text__paste(gui_node_t* n, scene_text_state_t* state)
{
    int64 scratch_size = n->text_limit + 1;
    char* scratch = GUI_CALLOC((size_t)scratch_size, 2);
    if (scratch == NULL_POINTER)
    {
        return;
    }
    char* pasted = scratch;
    char* filtered = scratch + scratch_size;
    int64 used = 0;
    int64 selected = state->caret > state->anchor ? state->caret - state->anchor : state->anchor - state->caret;
    int64 capacity = n->text_limit - n->text_len + selected;
    if (clipboard__read(pasted, scratch_size) == FALSE)
    {
        goto cleanup;
    }
    pasted[scratch_size - 1] = 0;
    for (int64 i = 0; pasted[i] != 0;)
    {
        ubyte first = (ubyte)pasted[i];
        int bytes = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                                   : first >= 0xe0 && first <= 0xef   ? 3
                                   : first >= 0xf0 && first <= 0xf4   ? 4
                                                                      : 0;
        uint cp = bytes == 1 ? first : first & (0x7f >> bytes);
        boole valid = bytes != 0;
        for (int j = 1; j < bytes; j++)
        {
            if (i + j >= scratch_size || ((ubyte)pasted[i + j] & 0xc0) != 0x80)
            {
                valid = FALSE;
                break;
            }
            cp = (cp << 6) | ((ubyte)pasted[i + j] & 63);
        }
        if (valid == FALSE)
        {
            i++;
            continue;
        }
        i += bytes;
        if ((bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000))
        {
            continue;
        }
        // Flatten clipboard line breaks for a single-line edit; CRLF is one space.
        if (cp == '\r' || cp == '\n' || cp == '\t')
        {
            if (cp == '\r' && pasted[i] == '\n')
            {
                i++;
            }
            cp = n->type == GUI_NODE_TYPE__TEXTAREA ? (cp == '\t' ? '\t' : '\n') : ' ';
        }
        char encoded[4] = {0};
        if (n->type == GUI_NODE_TYPE__TEXTAREA && (cp == '\n' || cp == '\t'))
        {
            encoded[0] = (char)cp;
            bytes = 1;
        }
        else
        {
            bytes = scene_text__encode(cp, encoded);
        }
        if (used + bytes > capacity)
        {
            scene_text__clear(encoded, sizeof(encoded));
            break;
        }
        stdlib__memcpy(filtered + used, encoded, (size_t)bytes);
        scene_text__clear(encoded, sizeof(encoded));
        used += bytes;
    }
    if (used)
    {
        scene_text__replace(n, state, filtered, used);
    }
cleanup:
    // A failed host read can still have written sensitive bytes.
    ;
    volatile char* clear = scratch;
    for (int64 i = 0; i < scratch_size * 2; i++)
    {
        clear[i] = 0;
    }
    GUI_FREE(scratch);
}

void scene_text__on_char(gui_node_t* n, uint codepoint)
{
    scene_text_state_t* state = scene_text__state(n);
    char encoded[4] = {0};
    int bytes = scene_text__encode(codepoint, encoded);
    uint modifiers = scene__get_active_modifiers();
    // AltGr uses Ctrl+Alt and must still be able to enter printable characters.
    if (state != NULL_POINTER && bytes && (!(modifiers & 4) || (modifiers & 8)))
    {
        scene_text__replace(n, state, encoded, bytes);
    }
    scene_text__clear(encoded, sizeof(encoded));
}

void scene_text__on_key(gui_node_t* n, int64 vk, boole down)
{
    if (down == FALSE)
    {
        return;
    }
    scene_text_state_t* state = scene_text__state(n);
    if (state == NULL_POINTER)
    {
        return;
    }
    uint modifiers = scene__get_active_modifiers();
    boole ctrl = (modifiers & 4) != 0 && !(modifiers & 8);
    boole shift = (modifiers & 1) != 0;
    int64 start = state->caret < state->anchor ? state->caret : state->anchor;
    int64 end = state->caret > state->anchor ? state->caret : state->anchor;
    if (ctrl && vk == 'A')
    {
        state->anchor = 0;
        state->caret = n->text_len;
        state->reveal_caret = TRUE;
        state->has_preferred_x = FALSE;
        state->prefer_line_end = FALSE;
    }
    else if ((ctrl && (vk == 'C' || vk == 'X' || vk == 0x2d)) || (shift && vk == 0x2e))
    {
        if (!n->input_password && end > start && clipboard__write(n->text + start, end - start))
        {
            if (vk == 'X' || (shift && vk == 0x2e))
            {
                scene_text__replace(n, state, "", 0);
            }
        }
    }
    else if ((ctrl && vk == 'V') || (shift && vk == 0x2d))
    {
        scene_text__paste(n, state);
    }
    else if (vk == 0x25 || vk == 0x27 || vk == 0x24 || vk == 0x23)
    {
        if (vk == 0x24)
        {
            state->caret = 0;
        }
        else if (vk == 0x23)
        {
            state->caret = n->text_len;
        }
        else if (!shift && end > start)
        {
            state->caret = vk == 0x25 ? start : end;
        }
        else
        {
            state->caret = vk == 0x25 ? scene_text__previous(n, state->caret) : scene_text__next(n, state->caret);
        }
        if (!shift)
        {
            state->anchor = state->caret;
        }
        state->reveal_caret = TRUE;
        state->has_preferred_x = FALSE;
        state->prefer_line_end = FALSE;
    }
    else if (vk == 0x08 || vk == 0x2e)
    {
        if (start == end)
        {
            state->anchor = vk == 0x08 ? scene_text__previous(n, state->caret) : scene_text__next(n, state->caret);
        }
        scene_text__replace(n, state, "", 0);
    }
    else if (vk == 0x0d || vk == 0x0a)
    {
        if (n->type == GUI_NODE_TYPE__TEXTAREA)
        {
            if (!ctrl)
            {
                scene_text__replace(n, state, "\n", 1);
            }
        }
        else
        {
            scene__set_focus(NULL_POINTER);
            scene__hide_keyboard();
        }
    }
}

