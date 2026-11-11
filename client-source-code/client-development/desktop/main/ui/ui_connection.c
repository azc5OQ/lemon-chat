#include "ui/ui_connection.h"
#include "ui/ui_headers.h"
#include "ui/ui_layout.h"
#include "network/endpoint.h"
#include "session/session.h"
#include "crypto/crypto_session.h"
#include "attachments/attachments.h"
#include "attachments/attachment_file.h"
#include "platform/memory.h"
#include "widgets/widget_filepicker.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>
#include "../../third-party/dave-g-json/cJSON.h"

static const char *g_connection_ciphers[] = { "aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80" };

static int64 g_connection_key_count;
static desktop_settings_t *g_connection_settings;
static boole g_identity_export;
static char g_identity_selected_path[DESKTOP_ATTACHMENT_PATH];

// declarations
static gui_node_t *_ui_connection_internal__node(const char *prefix, int64 index);
static void _ui_connection_internal__result(const char *text);
static void _ui_connection_internal__add(gui_event_t *event);
static void _ui_connection_internal__remove(gui_event_t *event);
static void _ui_connection_internal__reveal(gui_event_t *event);
static void _ui_connection_internal__advanced(gui_event_t *event);
static void _ui_connection_internal__text(const char *id, const char *text);
static void _ui_connection_internal__panel(const char *id, boole visible);
static void _ui_connection_internal__identity_open(gui_event_t *event);
static void _ui_connection_internal__identity_close(gui_event_t *event);
static void _ui_connection_internal__identity_pick(gui_event_t *event);
static void _ui_connection_internal__identity_submit(gui_event_t *event);
static boole _ui_connection_internal__identity_file_valid(const char *identity);
static void _ui_connection_internal__settings_open(gui_event_t *event);
static void _ui_connection_internal__settings_close(gui_event_t *event);
static void _ui_connection_internal__settings_save(gui_event_t *event);

/** @brief offers Disconnect only for a live connection, or Cancel for a pending attempt. */
void desktop_ui_connection__state(boole connected, boole busy)
{
    gui_node_t *disconnect = scene__find_by_id("connection-disconnect");
    gui_node_t *connect = scene__find_by_id("connection-connect");

    // update the disconnect button
    if (disconnect)
    {
        clib__string_copy(disconnect->klass, sizeof(disconnect->klass), connected || busy ? "" : "hidden");
        if (clib__strcmp(disconnect->text, connected ? "Disconnect" : "Cancel") != 0)
        {
            _ui_connection_internal__text("connection-disconnect", connected ? "Disconnect" : "Cancel");
        }
    }

    // update the connect button
    if (connect)
    {
        if (connected || busy)
        {
            connect->state = GUI_NODE_STATE__DISABLED;
        }
        else if (connect->state == GUI_NODE_STATE__DISABLED)
        {
            connect->state = GUI_NODE_STATE__DEFAULT;
        }
    }
}

/** @brief restores the saved identity, preserving older valid RSA sizes in the selector. */
void desktop_ui_connection__apply_identity(void)
{
    gui_node_t *select = scene__find_by_id("identity-bits");
    gui_node_t *option = NULL_POINTER;
    gui_node_t *reveal = scene__find_by_id("identity-reveal");
    gui_node_t *identity = scene__find_by_id("identity-string");
    char number[16] = { 0 };
    int64 index = 0;

    // check the settings
    if (!g_connection_settings || !select)
    {
        return;
    }

    // fill the identity field
    _ui_connection_internal__text("identity-string", g_connection_settings->identity);

    // hide the identity text
    if (reveal && identity)
    {
        reveal->value = 0;
        identity->input_password = TRUE;
    }

    // select the key size
    clib__string_format(number, sizeof(number), "%lld", g_connection_settings->key_bits ? g_connection_settings->key_bits : 2048);
    for (option = select->first_child; option; option = option->next_sibling)
    {
        if (clib__strcmp(option->text, number) == 0)
        {
            select->value = (float)index;
            return;
        }
        index++;
    }
    // existing identities can use any multiple of 256 in the supported range.
    // changing that size would derive a different public key
    option = scene__node_new(GUI_NODE_TYPE__OPTION);
    if (option)
    {
        scene__set_text(option, number, clib__strlen(number));
        scene__add_child(select, option);
        select->value = (float)index;
    }
}

static void _ui_connection_internal__settings_open(gui_event_t *event)
{
    (void)event;

    // open the identity panel
    desktop_ui_connection__apply_identity();
    _ui_connection_internal__text("identity-result", "Identity changes apply to your next connection.");
    _ui_connection_internal__panel("identity-panel", TRUE);
}

static void _ui_connection_internal__settings_close(gui_event_t *event)
{
    (void)event;

    // close both panels
    _ui_connection_internal__panel("identity-file-panel", FALSE);
    _ui_connection_internal__panel("identity-panel", FALSE);

    // restore the saved identity
    desktop_ui_connection__apply_identity();
}

static void _ui_connection_internal__settings_save(gui_event_t *event)
{
    gui_node_t *identity = scene__find_by_id("identity-string");
    gui_node_t *select = scene__find_by_id("identity-bits");
    gui_node_t *option = NULL_POINTER;
    desktop_settings_t settings = { 0 };
    char *end = NULL_POINTER;
    long bits = 0;
    int64 index = 0;
    (void)event;

    // check the controls
    if (!g_connection_settings || !identity || !select)
    {
        return;
    }

    // read the key size
    option = select->first_child;
    for (index = 0; option && index < (int64)select->value; index++)
    {
        option = option->next_sibling;
    }
    if (option)
    {
        bits = clib__strtol(option->text, &end, 10);
    }

    // check the key size
    if (!end || *end || bits < 2048 || bits > 8192 || bits % 256)
    {
        _ui_connection_internal__text("identity-result", "Choose a supported RSA key size.");
        return;
    }

    // check the identity
    if (identity->text[0] && !desktop_ui_connection__identity_valid(identity->text))
    {
        _ui_connection_internal__text("identity-result", "An identity needs 199 to 512 printable characters. Leave it empty to create one.");
        return;
    }

    // save the identity
    settings = *g_connection_settings;
    if (!clib__string_copy(settings.identity, sizeof(settings.identity), identity->text))
    {
        _ui_connection_internal__text("identity-result", "The identity is too long.");
    }
    else
    {
        settings.key_bits = bits;
        if (desktop_settings__save(&settings))
        {
            *g_connection_settings = settings;
            _ui_connection_internal__text("identity-result", "Identity saved. It will be used on your next connection.");
        }
        else
        {
            _ui_connection_internal__text("identity-result", "Could not save the identity. Check the settings directory permissions.");
        }
    }

    // erase the local copy
    desktop_memory__erase(&settings, sizeof(settings));
}

/**
 * @brief copies plain text into a connection control.
 * @param const char* id -> existing control ID
 * @param const char* text -> copied text
 * @return void
 */
static void _ui_connection_internal__text(const char *id, const char *text)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
}

/**
 * @brief shows or hides the identity file window without blocking the chat.
 * @param const char* id -> panel ID
 * @param boole visible -> desired visibility
 * @return void
 */
static void _ui_connection_internal__panel(const char *id, boole visible)
{
    gui_node_t *node = scene__find_by_id((char *)id);

    // change the visibility
    if (node)
    {
        clib__string_copy(node->klass, sizeof(node->klass), visible ? "column panel" : "column panel hidden");
        if (visible)
        {
            desktop_ui_layout__raise(node);
        }
    }
}

/**
 * @brief finds one scene-owned key control without retaining pointers across hot reload.
 * @param const char* prefix -> control ID prefix
 * @param int64 index -> bounded key slot
 * @return gui_node_t* -> borrowed node, or NULL
 */
static gui_node_t *_ui_connection_internal__node(const char *prefix, int64 index)
{
    char id[64] = { 0 };
    clib__string_format(id, sizeof(id), "%s-%lld", prefix, index);
    return scene__find_by_id(id);
}

/**
 * @brief reports a form validation problem without discarding its draft.
 * @param const char* text -> nonsecret user-facing explanation
 * @return void
 */
static void _ui_connection_internal__result(const char *text)
{
    gui_node_t *node = scene__find_by_id("login-result");
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
}

/**
 * @brief checks the identity string format shared by browser and desktop clients.
 * @param const char* identity -> terminated identity string
 * @return boole -> TRUE for a browser-compatible printable ASCII identity that fits desktop storage
 */
boole desktop_ui_connection__identity_valid(const char *identity)
{
    int64 length = clib__string_length(identity, sizeof(((desktop_connection_t *)0)->identity));
    int64 index = 0;

    // check the length
    if (length < 199)
    {
        return FALSE;
    }

    // check every character
    for (index = 0; index < length; index++)
    {
        if ((ubyte)identity[index] < 33 || (ubyte)identity[index] > 126)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief checks the stricter length accepted by the browser .lmn file importer.
 * @param const char* identity -> already decoded identity string
 * @return boole -> TRUE for a portable 199 or 200 byte identity file
 */
static boole _ui_connection_internal__identity_file_valid(const char *identity)
{
    int64 length = clib__string_length(identity, 201);
    return (length == 199 || length == 200) && desktop_ui_connection__identity_valid(identity);
}

/**
 * @brief decodes the browser .lmn format without changing the selected identity on failure.
 * @param const ubyte* bytes -> exact file bytes
 * @param uint64 size -> file size
 * @param char* identity -> cleared destination
 * @param uint64 capacity -> destination capacity including NUL
 * @return boole -> TRUE when the trimmed raw identity is compatible with both clients
 */
boole desktop_ui_connection__identity_decode(const ubyte *bytes, uint64 size, char *identity, uint64 capacity)
{
    uint64 start = 0;
    uint64 end = size;

    // check arguments
    if (!bytes || !identity || !capacity || capacity > SIZE_MAX || capacity > INT64_MAX || size > 4096)
    {
        return FALSE;
    }

    // erase the output
    desktop_memory__erase(identity, capacity);

    // skip byte order mark
    if (size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
    {
        start = 3;
    }

    // skip spaces around text
    while (start < end && clib__isspace(bytes[start]))
    {
        start++;
    }
    while (end > start && clib__isspace(bytes[end - 1]))
    {
        end--;
    }

    // check and copy identity
    if ((end - start != 199 && end - start != 200) || clib__memchr(bytes + start, 0, (int64)(end - start)) || !clib__string_copy_n(identity, capacity, (const char *)bytes + start, end - start) || !_ui_connection_internal__identity_file_valid(identity))
    {
        desktop_memory__erase(identity, capacity);
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief restores row visibility and byte limits after loading the editable layout.
 * @return boole -> TRUE when all controls exist and accept their limits
 */
boole desktop_ui_connection__refresh(void)
{
    gui_node_t *row = NULL_POINTER;
    gui_node_t *input = NULL_POINTER;
    gui_node_t *add = scene__find_by_id("server-key-add");
    gui_node_t *identity = scene__find_by_id("identity-string");
    gui_node_t *reveal = scene__find_by_id("identity-reveal");
    int64 index = 0;
    boole complete = add != NULL && identity != NULL && scene__set_text_limit(identity, sizeof(((desktop_connection_t *)0)->identity) - 1) && scene__reserve_text(identity, sizeof(((desktop_connection_t *)0)->identity) - 1);

    // set identity text visibility
    if (identity && reveal)
    {
        identity->input_password = reveal->value == 0;
    }

    // update the key rows
    for (index = 0; index < DESKTOP_SERVER_KEY_LIMIT; index++)
    {
        // find the row controls
        row = _ui_connection_internal__node("server-key-row", index);
        input = _ui_connection_internal__node("server-key-value", index);

        // set the text limits
        if (!row || !input || !scene__set_text_limit(input, sizeof(((desktop_connection_t *)0)->server_keys) - 1) || !scene__reserve_text(input, sizeof(((desktop_connection_t *)0)->server_keys) - 1))
        {
            complete = FALSE;
            continue;
        }

        // show or hide row
        clib__string_copy(row->klass, sizeof(row->klass), index < g_connection_key_count ? "row server-key-row" : "row server-key-row hidden");
        clib__null_memory(&row->style, sizeof(row->style));
        clib__null_memory(&row->resolved, sizeof(row->resolved));
    }

    // update the add button
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
static void _ui_connection_internal__add(gui_event_t *event)
{
    gui_node_t *input = NULL_POINTER;
    (void)event;

    // check the key limit
    if (g_connection_key_count >= DESKTOP_SERVER_KEY_LIMIT)
    {
        return;
    }

    // find the next field
    input = _ui_connection_internal__node("server-key-value", g_connection_key_count);
    if (!input)
    {
        return;
    }

    // show the new row
    g_connection_key_count++;
    desktop_ui_connection__refresh();
    scene__set_focus(input);
}

/**
 * @brief removes a key and preserves the order of the remaining encryption layers.
 * @param gui_event_t* event -> row remove-button event
 * @return void
 */
static void _ui_connection_internal__remove(gui_event_t *event)
{
    gui_node_t *input = NULL_POINTER;
    gui_node_t *next = NULL_POINTER;
    gui_node_t *button = NULL_POINTER;
    int64 index = 0;
    int64 selected = -1;

    // check arguments
    if (!event || !event->sender)
    {
        return;
    }

    // find the clicked row
    for (index = 0; index < g_connection_key_count; index++)
    {
        button = _ui_connection_internal__node("server-key-remove", index);
        if (button == event->sender)
        {
            selected = index;
            break;
        }
    }

    // nothing to remove
    if (selected < 0)
    {
        return;
    }
    // reserve every destination before changing any row, so allocation failure retains all keys
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

    // remove the focus
    scene__set_focus(NULL);

    // move later keys up
    for (index = selected; index < g_connection_key_count; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        next = index + 1 < g_connection_key_count ? _ui_connection_internal__node("server-key-value", index + 1) : NULL;
        desktop_memory__erase(input->text, input->text_capacity);
        scene__set_text(input, next ? next->text : "", next ? next->text_len : 0);
        _ui_connection_internal__node("server-key-type", index)->value = next ? _ui_connection_internal__node("server-key-type", index + 1)->value : 0;
    }

    // decrease the key count
    g_connection_key_count--;
    desktop_ui_connection__refresh();
}

/**
 * @brief wipes unsaved keys when switching to a different bookmarked server.
 * @return void
 */
void desktop_ui_connection__clear_keys(void)
{
    gui_node_t *input = NULL_POINTER;
    int64 index = 0;

    // erase every key field
    for (index = 0; index < DESKTOP_SERVER_KEY_LIMIT; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        if (input)
        {
            desktop_memory__erase(input->text, input->text_capacity);
            scene__set_text(input, "", 0);
        }
    }

    // reset the key count
    g_connection_key_count = 0;
    desktop_ui_connection__refresh();
}

/**
 * @brief restores saved encryption layers after validating every row and its capacity.
 * @param const char* keys -> borrowed newline-separated key strings; never modified
 * @return boole -> TRUE on success; malformed row boundaries preserve the current rows
 */
boole desktop_ui_connection__load_keys(const char *keys)
{
    int64 length = clib__string_length(keys, sizeof(((desktop_connection_t *)0)->server_keys));
    int64 starts[DESKTOP_SERVER_KEY_LIMIT] = { 0 };
    int64 lengths[DESKTOP_SERVER_KEY_LIMIT] = { 0 };
    int64 count = 0;
    int64 start = 0;
    int64 index = 0;
    gui_node_t *input = NULL_POINTER;
    gui_node_t *type = NULL_POINTER;
    cJSON *entry = NULL_POINTER;
    const char *password = NULL_POINTER;
    const char *cipher = NULL_POINTER;
    int selected = 0;

    // check the length
    if (length < 0)
    {
        return FALSE;
    }

    // find every key range
    for (index = 0; length && index <= length; index++)
    {
        // refuse a carriage return
        if (keys[index] == '\r')
        {
            return FALSE;
        }

        // a key ends here
        if (index == length || keys[index] == '\n')
        {
            // check key and count
            if (index == start || count == DESKTOP_SERVER_KEY_LIMIT)
            {
                return FALSE;
            }

            // reserve the field text
            input = _ui_connection_internal__node("server-key-value", count);
            type = _ui_connection_internal__node("server-key-type", count);
            if (!input || !type || !scene__reserve_text(input, index - start))
            {
                return FALSE;
            }

            // store start and length
            starts[count] = start;
            lengths[count++] = index - start;
            start = index + 1;
        }
    }

    // erase the old keys
    desktop_ui_connection__clear_keys();

    // fill every key row
    for (index = 0; index < count; index++)
    {
        // find the row controls
        input = _ui_connection_internal__node("server-key-value", index);
        type = _ui_connection_internal__node("server-key-type", index);

        // read the JSON entry
        entry = keys[starts[index]] == '{' ? cJSON_ParseWithLength(keys + starts[index], lengths[index]) : NULL;
        password = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "password"));
        cipher = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "type"));

        // find the cipher
        selected = 0;
        if (password != NULL && cipher != NULL)
        {
            for (selected = 0; selected < (int)(sizeof(g_connection_ciphers) / sizeof(g_connection_ciphers[0])); selected++)
            {
                if (clib__strcmp(cipher, g_connection_ciphers[selected]) == 0)
                {
                    break;
                }
            }
        }

        // set the key text
        if (selected == (int)(sizeof(g_connection_ciphers) / sizeof(g_connection_ciphers[0])) || !scene__set_text(input, password != NULL && cipher != NULL ? password : keys + starts[index], password != NULL && cipher != NULL ? clib__strlen(password) : lengths[index]))
        {
            crypto_session__free_json(entry);
            desktop_ui_connection__clear_keys();
            return FALSE;
        }

        // select the cipher
        type->value = selected;
        crypto_session__free_json(entry);
    }

    // show the loaded rows
    g_connection_key_count = count;
    return desktop_ui_connection__refresh();
}

/**
 * @brief serializes ordered typed key rows into the protocol input, even when hidden.
 * @param char* output -> caller-owned buffer, cleared on failure
 * @param uint64 capacity -> buffer capacity including NUL
 * @return boole -> TRUE for valid nonempty rows within the total byte limit
 */
boole desktop_ui_connection__keys(char *output, uint64 capacity)
{
    gui_node_t *input = NULL_POINTER;
    gui_node_t *type = NULL_POINTER;
    int64 index = 0;
    cJSON *entry = NULL_POINTER;
    char *encoded = NULL_POINTER;
    boole complete = FALSE;

    // check arguments
    if (!output || !capacity)
    {
        return FALSE;
    }

    // erase the output
    desktop_memory__erase(output, capacity);

    // add every key row
    for (index = 0; index < g_connection_key_count; index++)
    {
        // check one key row
        input = _ui_connection_internal__node("server-key-value", index);
        type = _ui_connection_internal__node("server-key-type", index);
        if (!input || !type || type->value < 0 || type->value > 4 || !input->text_len || clib__strchr(input->text, '\r') || clib__strchr(input->text, '\n') || (index && !clib__string_append(output, capacity, "\n")))
        {
            desktop_memory__erase(output, capacity);
            _ui_connection_internal__result("Fill in each key or remove its row. Keys together must fit within 2048 bytes.");
            return FALSE;
        }

        // append the key
        if (type->value == 0 && input->text[0] != '{')
        {
            complete = clib__string_append(output, capacity, input->text);
        }
        else
        {
            entry = cJSON_CreateObject();
            complete = entry && cJSON_AddStringToObject(entry, "type", g_connection_ciphers[(int)type->value]) && cJSON_AddStringToObject(entry, "password", input->text);
            encoded = complete ? cJSON_PrintUnformatted(entry) : NULL;
            complete = encoded && clib__string_append(output, capacity, encoded);
            crypto_session__free_text(encoded);
            crypto_session__free_json(entry);
        }

        // report the full buffer
        if (!complete)
        {
            desktop_memory__erase(output, capacity);
            _ui_connection_internal__result("The configured keys exceed the available storage.");
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
boole desktop_ui_connection__address(char *output, uint64 capacity)
{
    gui_node_t *host = scene__find_by_id("server-address");
    gui_node_t *port = scene__find_by_id("server-port");
    return host && port && desktop_endpoint__compose(host->text, port->text, "/", output, capacity);
}

/**
 * @brief loads the hostname and port from an existing saved URL.
 * @param const char* address -> existing ws/wss URL
 * @return boole -> TRUE when all fields were populated
 */
boole desktop_ui_connection__load_address(const char *address)
{
    desktop_endpoint_t endpoint = { 0 };
    gui_node_t *host = scene__find_by_id("server-address");
    gui_node_t *port = scene__find_by_id("server-port");
    char number[8] = { 0 };
    char normalized[1024] = { 0 };

    // parse the address
    if (!host || !port || !desktop_endpoint__parse(address, &endpoint))
    {
        return FALSE;
    }

    // format the port number
    clib__string_format(number, sizeof(number), "%u", (unsigned int)endpoint.port);

    // reserve the field text
    if (!desktop_endpoint__compose(endpoint.host, number, "/", normalized, sizeof(normalized)) || !scene__reserve_text(host, clib__strlen(endpoint.host)) || !scene__reserve_text(port, clib__strlen(number)))
    {
        return FALSE;
    }

    // fill both fields
    return scene__set_text(host, endpoint.host, clib__strlen(endpoint.host)) && scene__set_text(port, number, clib__strlen(number));
}

/**
 * @brief reveals the identity on request, enabling normal selected-text copying.
 * @param gui_event_t* event -> show-identity checkbox event
 * @return void
 */
static void _ui_connection_internal__reveal(gui_event_t *event)
{
    gui_node_t *input = scene__find_by_id("identity-string");
    if (input && event && event->sender)
    {
        input->input_password = event->sender->value == 0;
    }
}

/** @brief shows the key editor without changing the keys used for login. */
static void _ui_connection_internal__advanced(gui_event_t *event)
{
    gui_node_t *section = scene__find_by_id("connection-advanced");
    if (section && event && event->sender)
    {
        clib__string_copy(section->klass, sizeof(section->klass), event->sender->value != 0 ? "column" : "column hidden");
    }
}

/**
 * @brief opens import or export controls for the browser-compatible raw .lmn file.
 * @param gui_event_t* event -> Import or Export button event
 * @return void
 */
static void _ui_connection_internal__identity_open(gui_event_t *event)
{
    gui_node_t *picker = scene__find_by_id("identity-file-picker");
    gui_node_t *filename = scene__find_by_id("identity-file-name");
    gui_node_t *username = scene__find_by_id("username");
    char path[DESKTOP_ATTACHMENT_PATH] = { 0 };
    char name[256] = { 0 };
    const char *source = username && username->text[0] ? username->text : "identity";

    // choose import or export
    g_identity_export = event && event->sender && clib__strcmp(event->sender->id, "identity-export") == 0;
    desktop_memory__erase(g_identity_selected_path, sizeof(g_identity_selected_path));

    // set the start folder
    if (picker && desktop_attachment_file__home(path, sizeof(path)))
    {
        widget_filepicker__set_path(picker, path);
    }

    // build the file name
    desktop_attachment_file__name(name, sizeof(name), source);
    if (g_identity_export)
    {
        clib__string_append(name, sizeof(name), ".lmn");
    }

    // set name field state
    if (filename)
    {
        filename->state = g_identity_export ? GUI_NODE_STATE__DEFAULT : GUI_NODE_STATE__DISABLED;
    }

    // fill the panel texts
    _ui_connection_internal__text("identity-file-title", g_identity_export ? "Export identity" : "Import identity");
    _ui_connection_internal__text("identity-file-name", g_identity_export ? name : "");
    _ui_connection_internal__text("identity-file-submit", g_identity_export ? "Export" : "Import");
    _ui_connection_internal__text("identity-file-result", g_identity_export ? "Choose a folder and filename. Existing files are not overwritten." : "Choose a .lmn identity file exported by the browser or desktop client.");

    // show the panel
    _ui_connection_internal__panel("identity-file-panel", TRUE);

    // erase the local copies
    desktop_memory__erase(path, sizeof(path));
    desktop_memory__erase(name, sizeof(name));
}

/**
 * @brief closes the identity file controls and clears the selected path.
 * @param gui_event_t* event -> unused close event
 * @return void
 */
static void _ui_connection_internal__identity_close(gui_event_t *event)
{
    (void)event;

    // close the file panel
    _ui_connection_internal__panel("identity-file-panel", FALSE);

    // clear the selected file
    desktop_memory__erase(g_identity_selected_path, sizeof(g_identity_selected_path));
    _ui_connection_internal__text("identity-file-name", "");
}

/**
 * @brief retains the selected import path only while the identity window is open.
 * @param gui_event_t* event -> file-picker selection event
 * @return void
 */
static void _ui_connection_internal__identity_pick(gui_event_t *event)
{
    char name[256] = { 0 };

    // store or clear path
    if (!g_identity_export && event && !event->file.is_dir && event->file.path)
    {
        desktop_attachment_file__name(name, sizeof(name), event->file.path);
        if (clib__string_copy(g_identity_selected_path, sizeof(g_identity_selected_path), event->file.path))
        {
            _ui_connection_internal__text("identity-file-name", name);
            _ui_connection_internal__text("identity-file-result", "Ready to import the selected identity.");
        }
        else
        {
            desktop_memory__erase(g_identity_selected_path, sizeof(g_identity_selected_path));
            _ui_connection_internal__text("identity-file-result", "The selected path is too long.");
        }
    }
    else if (!g_identity_export && (!event || event->file.is_dir))
    {
        desktop_memory__erase(g_identity_selected_path, sizeof(g_identity_selected_path));
        _ui_connection_internal__text("identity-file-name", "");
    }

    // erase the local copy
    desktop_memory__erase(name, sizeof(name));
}

/**
 * @brief imports or exports the same raw identity bytes used by browser .lmn files.
 * @param gui_event_t* event -> unused submit event
 * @return void
 */
static void _ui_connection_internal__identity_submit(gui_event_t *event)
{
    gui_node_t *identity = scene__find_by_id("identity-string");
    gui_node_t *filename = scene__find_by_id("identity-file-name");
    gui_node_t *picker = scene__find_by_id("identity-file-picker");
    const char *directory = picker ? widget_filepicker__path(picker) : NULL;
    ubyte *bytes = NULL_POINTER;
    uint64 size = 0;
    int64 name_length = 0;
    char imported[sizeof(((desktop_connection_t *)0)->identity)] = { 0 };
    char name[256] = { 0 };
    char path[DESKTOP_ATTACHMENT_PATH] = { 0 };
    boole complete = FALSE;
    (void)event;

    // export or import identity
    if (g_identity_export)
    {
        // check the identity
        if (!identity || !_ui_connection_internal__identity_file_valid(identity->text))
        {
            _ui_connection_internal__text("identity-file-result", "The identity must contain 199 or 200 printable characters before it can be exported.");
            goto label_finished;
        }

        // check the file name
        desktop_attachment_file__name(name, sizeof(name), filename ? filename->text : "");
        if (!filename || !filename->text[0] || clib__strcmp(name, filename->text) != 0)
        {
            _ui_connection_internal__text("identity-file-result", "Enter a valid filename.");
            goto label_finished;
        }

        // add the file extension
        name_length = clib__string_length(name, sizeof(name));
        if (name_length < 4 || clib__strcmp(name + name_length - 4, ".lmn") != 0)
        {
            if (!clib__string_append(name, sizeof(name), ".lmn"))
            {
                _ui_connection_internal__text("identity-file-result", "The filename is too long.");
                goto label_finished;
            }
        }

        // build the file path
        if (!directory || clib__strlen(directory) + clib__strlen(name) + 2 > sizeof(path) || clib__string_format(path, sizeof(path), "%s/%s", directory, name) < 0)
        {
            _ui_connection_internal__text("identity-file-result", "The selected path is too long.");
            goto label_finished;
        }

        // write the file
        complete = desktop_attachment_file__save(path, (const ubyte *)identity->text, identity->text_len);
        _ui_connection_internal__text("identity-file-result", complete ? "Identity exported. Import this file with the same RSA key size in the other client." : "Could not create the file. Choose a new name; existing files are not overwritten.");
    }
    else
    {
        // read and decode file
        bytes = desktop_attachment_file__read(g_identity_selected_path, 4096, &size);
        if (!bytes || !desktop_ui_connection__identity_decode(bytes, size, imported, sizeof(imported)))
        {
            _ui_connection_internal__text("identity-file-result", "This is not a browser-compatible .lmn identity file.");
            goto label_finished;
        }

        // fill the identity field
        if (!identity || !scene__reserve_text(identity, clib__strlen(imported)) || !scene__set_text(identity, imported, clib__strlen(imported)))
        {
            _ui_connection_internal__text("identity-file-result", "Could not place the identity in the identity settings.");
            goto label_finished;
        }

        // close the file panel
        _ui_connection_internal__panel("identity-file-panel", FALSE);
        _ui_connection_internal__text("identity-result", "Identity imported. Choose the same RSA size as the other client, then Save identity.");
        desktop_memory__erase(g_identity_selected_path, sizeof(g_identity_selected_path));
    }
label_finished:
    // erase and free buffers
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, size);
    }
    free(bytes);
    desktop_memory__erase(imported, sizeof(imported));
    desktop_memory__erase(name, sizeof(name));
    desktop_memory__erase(path, sizeof(path));
}

/**
 * @brief registers local connection editor handlers before parsing the scene.
 * @return void
 */
void desktop_ui_connection__initialize(desktop_settings_t *settings)
{
    // store the settings
    g_connection_settings = settings;

    // register the handlers
    scene__register_handler("on_identity_settings_open", _ui_connection_internal__settings_open);
    scene__register_handler("on_identity_settings_close", _ui_connection_internal__settings_close);
    scene__register_handler("on_identity_settings_save", _ui_connection_internal__settings_save);
    scene__register_handler("on_server_key_add", _ui_connection_internal__add);
    scene__register_handler("on_server_key_remove", _ui_connection_internal__remove);
    scene__register_handler("on_identity_reveal", _ui_connection_internal__reveal);
    scene__register_handler("on_connection_advanced", _ui_connection_internal__advanced);
    scene__register_handler("on_identity_file_open", _ui_connection_internal__identity_open);
    scene__register_handler("on_identity_file_close", _ui_connection_internal__identity_close);
    scene__register_handler("on_identity_file_pick", _ui_connection_internal__identity_pick);
    scene__register_handler("on_identity_file_submit", _ui_connection_internal__identity_submit);
}

/**
 * @brief wipes editable secrets before the scene releases its buffers, including revealed identities.
 * @return void
 */
void desktop_ui_connection__shutdown(void)
{
    gui_node_t *input = scene__find_by_id("identity-string");
    int64 index = 0;

    // erase the identity field
    if (input)
    {
        desktop_memory__erase(input->text, input->text_capacity);
    }

    // erase every key field
    for (index = 0; index < DESKTOP_SERVER_KEY_LIMIT; index++)
    {
        input = _ui_connection_internal__node("server-key-value", index);
        if (input)
        {
            desktop_memory__erase(input->text, input->text_capacity);
        }
    }

    // reset the stored values
    g_connection_key_count = 0;
    g_connection_settings = NULL;
    g_identity_export = FALSE;
    desktop_memory__erase(g_identity_selected_path, sizeof(g_identity_selected_path));
}
