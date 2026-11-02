#include "ui/ui_connection.h"
#include "ui/ui_headers.h"
#include "network/endpoint.h"
#include "session/session.h"
#include "crypto/crypto.h"
#include "platform/memory.h"

static int64 g_connection_key_count;

// declarations
static gui_node_t* _ui_connection_internal__node(const char* prefix, int64 index);
static void _ui_connection_internal__result(const char* text);
static void _ui_connection_internal__add(gui_event_t* event);
static void _ui_connection_internal__remove(gui_event_t* event);
static void _ui_connection_internal__reveal(gui_event_t* event);

/**
 * @brief finds one scene-owned key control without retaining pointers across hot reload.
 * @param const char* prefix -> control ID prefix
 * @param int64 index -> bounded key slot
 * @return gui_node_t* -> borrowed node, or NULL
 */
static gui_node_t* _ui_connection_internal__node(const char* prefix, int64 index)
{
    char id[64] = {0};
    clib__string_format(id, sizeof(id), "%s-%lld", prefix, index);
    return scene__find_by_id(id);
}

/**
 * @brief reports a form validation problem without discarding its draft.
 * @param const char* text -> nonsecret user-facing explanation
 * @return void
 */
static void _ui_connection_internal__result(const char* text)
{
    gui_node_t* node = scene__find_by_id("login-result");
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
}

/**
 * @brief restores row visibility and byte limits after loading the editable layout.
 * @return boole -> TRUE when all controls exist and accept their limits
 */
boole desktop_ui_connection__refresh(void)
{
    gui_node_t* row = NULL;
    gui_node_t* input = NULL;
    gui_node_t* add = scene__find_by_id("server-key-add");
    gui_node_t* identity = scene__find_by_id("identity-string");
    gui_node_t* reveal = scene__find_by_id("identity-reveal");
    int64 index = 0;
    boole complete = add != NULL;
    if (identity && reveal)
    {
        identity->input_password = reveal->value == 0;
    }
    for (index = 0; index < DESKTOP_SERVER_KEY_LIMIT; index++)
    {
        row = _ui_connection_internal__node("server-key-row", index);
        input = _ui_connection_internal__node("server-key-value", index);
        if (!row || !input || !scene__set_text_limit(input, sizeof(((desktop_connection_t*)0)->server_keys) - 1)
            || !scene__reserve_text(input, sizeof(((desktop_connection_t*)0)->server_keys) - 1))
        {
            complete = FALSE;
            continue;
        }
        clib__string_copy(row->klass, sizeof(row->klass), index < g_connection_key_count ? "row server-key-row" : "row server-key-row hidden");
        clib__null_memory(&row->style, sizeof(row->style));
        clib__null_memory(&row->resolved, sizeof(row->resolved));
    }
    if (add)
    {
        add->state = g_connection_key_count >= DESKTOP_SERVER_KEY_LIMIT ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;
    }
    return complete;
}

/**
 * @brief appends one empty key row without changing saved connection details.
 * @param gui_event_t* event -> unused add-button event
 * @return void
 */
static void _ui_connection_internal__add(gui_event_t* event)
{
    gui_node_t* input = NULL;
    (void)event;
    if (g_connection_key_count >= DESKTOP_SERVER_KEY_LIMIT)
    {
        return;
    }
    input = _ui_connection_internal__node("server-key-value", g_connection_key_count);
    if (!input)
    {
        return;
    }
    g_connection_key_count++;
    desktop_ui_connection__refresh();
    scene__set_focus(input);
}

/**
 * @brief removes a key and preserves the order of the remaining encryption layers.
 * @param gui_event_t* event -> row remove-button event
 * @return void
 */
static void _ui_connection_internal__remove(gui_event_t* event)
{
    gui_node_t* input = NULL;
    gui_node_t* next = NULL;
    gui_node_t* button = NULL;
    int64 index = 0;
    int64 selected = -1;
    if (!event || !event->sender)
    {
        return;
    }
    for (index = 0; index < g_connection_key_count; index++)
    {
        button = _ui_connection_internal__node("server-key-remove", index);
        if (button == event->sender)
        {
            selected = index;
            break;
        }
    }
    if (selected < 0)
    {
        return;
    }
    // Reserve every destination before changing any row, so allocation failure retains all keys.
    for (index = selected; index + 1 < g_connection_key_count; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        next = _ui_connection_internal__node("server-key-value", index + 1);
        if (!input || !next || !scene__reserve_text(input, next->text_len))
        {
            _ui_connection_internal__result("Could not remove the key. Your keys are unchanged.");
            return;
        }
    }
    scene__set_focus(NULL);
    for (index = selected; index < g_connection_key_count; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        next = index + 1 < g_connection_key_count ? _ui_connection_internal__node("server-key-value", index + 1) : NULL;
        desktop_memory__erase(input->text, input->text_capacity);
        scene__set_text(input, next ? next->text : "", next ? next->text_len : 0);
    }
    g_connection_key_count--;
    desktop_ui_connection__refresh();
}

/**
 * @brief wipes unsaved keys when switching to a different bookmarked server.
 * @return void
 */
void desktop_ui_connection__clear_keys(void)
{
    gui_node_t* input = NULL;
    int64 index = 0;
    for (index = 0; index < DESKTOP_SERVER_KEY_LIMIT; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        if (input)
        {
            desktop_memory__erase(input->text, input->text_capacity);
            scene__set_text(input, "", 0);
        }
    }
    g_connection_key_count = 0;
    desktop_ui_connection__refresh();
}

/**
 * @brief restores saved encryption layers after validating every row and its capacity.
 * @param const char* keys -> borrowed newline-separated key strings; never modified
 * @return boole -> TRUE on success; malformed row boundaries preserve the current rows
 */
boole desktop_ui_connection__load_keys(const char* keys)
{
    int64 length = clib__string_length(keys, sizeof(((desktop_connection_t*)0)->server_keys));
    int64 starts[DESKTOP_SERVER_KEY_LIMIT] = {0};
    int64 lengths[DESKTOP_SERVER_KEY_LIMIT] = {0};
    int64 count = 0;
    int64 start = 0;
    int64 index = 0;
    gui_node_t* input = NULL;
    gui_node_t* type = NULL;
    if (length < 0)
    {
        return FALSE;
    }
    for (index = 0; length && index <= length; index++)
    {
        if (keys[index] == '\r')
        {
            return FALSE;
        }
        if (index == length || keys[index] == '\n')
        {
            if (index == start || count == DESKTOP_SERVER_KEY_LIMIT)
            {
                return FALSE;
            }
            input = _ui_connection_internal__node("server-key-value", count);
            type = _ui_connection_internal__node("server-key-type", count);
            if (!input || !type || !scene__reserve_text(input, index - start))
            {
                return FALSE;
            }
            starts[count] = start;
            lengths[count++] = index - start;
            start = index + 1;
        }
    }
    desktop_ui_connection__clear_keys();
    for (index = 0; index < count; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        type = _ui_connection_internal__node("server-key-type", index);
        if (!scene__set_text(input, keys + starts[index], lengths[index]))
        {
            desktop_ui_connection__clear_keys();
            return FALSE;
        }
        type->value = 0;
    }
    g_connection_key_count = count;
    return desktop_ui_connection__refresh();
}

/**
 * @brief serializes ordered AES-256 key rows into the existing protocol input.
 * @param char* output -> caller-owned buffer, cleared on failure
 * @param uint64 capacity -> buffer capacity including NUL
 * @return boole -> TRUE for valid nonempty rows within the total byte limit
 */
boole desktop_ui_connection__keys(char* output, uint64 capacity)
{
    gui_node_t* input = NULL;
    gui_node_t* type = NULL;
    int64 index = 0;
    if (!output || !capacity)
    {
        return FALSE;
    }
    desktop_memory__erase(output, capacity);
    for (index = 0; index < g_connection_key_count; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        type = _ui_connection_internal__node("server-key-type", index);
        if (!input || !type || type->value != 0 || !input->text_len
            || clib__strchr(input->text, '\r') || clib__strchr(input->text, '\n')
            || (index && !clib__string_append(output, capacity, "\n"))
            || !clib__string_append(output, capacity, input->text))
        {
            desktop_memory__erase(output, capacity);
            _ui_connection_internal__result("Fill in each key or remove its row. Keys together must fit within 2048 bytes.");
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief combines the hostname and port fields for the transport.
 * @param char* output -> caller-owned URL buffer
 * @param uint64 capacity -> output capacity including NUL
 * @return boole -> TRUE when the hostname and port are valid
 */
boole desktop_ui_connection__address(char* output, uint64 capacity)
{
    gui_node_t* host = scene__find_by_id("server-address");
    gui_node_t* port = scene__find_by_id("server-port");
    return host && port && desktop_endpoint__compose(host->text, port->text, "/", output, capacity);
}

/**
 * @brief loads the hostname and port from an existing saved URL.
 * @param const char* address -> existing ws/wss URL
 * @return boole -> TRUE when all fields were populated
 */
boole desktop_ui_connection__load_address(const char* address)
{
    desktop_endpoint_t endpoint = {0};
    gui_node_t* host = scene__find_by_id("server-address");
    gui_node_t* port = scene__find_by_id("server-port");
    char number[8] = {0};
    char normalized[1024] = {0};
    if (!host || !port || !desktop_endpoint__parse(address, &endpoint))
    {
        return FALSE;
    }
    clib__string_format(number, sizeof(number), "%u", (unsigned int)endpoint.port);
    if (!desktop_endpoint__compose(endpoint.host, number, "/", normalized, sizeof(normalized))
        || !scene__reserve_text(host, clib__strlen(endpoint.host))
        || !scene__reserve_text(port, clib__strlen(number)))
    {
        return FALSE;
    }
    return scene__set_text(host, endpoint.host, clib__strlen(endpoint.host))
        && scene__set_text(port, number, clib__strlen(number));
}

/**
 * @brief reveals the identity on request, enabling normal selected-text copying.
 * @param gui_event_t* event -> show-identity checkbox event
 * @return void
 */
static void _ui_connection_internal__reveal(gui_event_t* event)
{
    gui_node_t* input = scene__find_by_id("identity-string");
    if (input && event && event->sender)
    {
        input->input_password = event->sender->value == 0;
    }
}

/**
 * @brief registers local connection editor handlers before parsing the scene.
 * @return void
 */
void desktop_ui_connection__initialize(void)
{
    scene__register_handler("on_server_key_add", _ui_connection_internal__add);
    scene__register_handler("on_server_key_remove", _ui_connection_internal__remove);
    scene__register_handler("on_identity_reveal", _ui_connection_internal__reveal);
}

/**
 * @brief wipes editable secrets before the scene releases its buffers, including revealed identities.
 * @return void
 */
void desktop_ui_connection__shutdown(void)
{
    gui_node_t* input = scene__find_by_id("identity-string");
    int64 index = 0;
    if (input)
    {
        desktop_memory__erase(input->text, input->text_capacity);
    }
    for (index = 0; index < DESKTOP_SERVER_KEY_LIMIT; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        if (input)
        {
            desktop_memory__erase(input->text, input->text_capacity);
        }
    }
    g_connection_key_count = 0;
}
