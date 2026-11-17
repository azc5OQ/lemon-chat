#include "../../../main/base.h"
#include "../../../main/core/channel.h"
#include "../../../main/msg/server/server_msg.h"
#include "../../../main/util.h"
#include <assert.h>

server_settings_t g_server_settings = {0};
channel_t *g_channel_array;
client_t *g_clients_array;
static cJSON *captured;

cJSON *server_msg__delivery__new_message(const char *type, cJSON **message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    *message = cJSON_AddObjectToObject(root, "message");
    return root;
}

void server_msg__delivery__send_json_to_client(client_t *client, cJSON *root)
{
    (void)client;
    cJSON_Delete(captured);
    captured = cJSON_Duplicate(root, 1);
}

void server_msg__delivery__send_text_to_client(client_t *client, char *text)
{
    (void)client;
    cJSON_Delete(captured);
    captured = cJSON_Parse(text);
}

boole channel__is_description_visible(const client_t *client, const channel_t *channel)
{
    (void)client;
    (void)channel;
    return TRUE;
}

boole channel__write_required_powers(cJSON *object, const channel_t *channel)
{
    (void)object;
    (void)channel;
    return TRUE;
}

boole util__json_add_owned_object(cJSON *object, const char *name, cJSON *child)
{
    return cJSON_AddItemToObject(object, name, child);
}

boole util__json_add_owned_array(cJSON *array, cJSON *child)
{
    return cJSON_AddItemToArray(array, child);
}

#ifdef _WIN32
// unused packet writers remain linked on this platform
uint64 channel_groups__role(const client_t *client, const channel_t *channel)
{
    assert(0);
    return 0;
}

boole channel__are_members_visible(const client_t *client, const channel_t *channel)
{
    assert(0);
    return FALSE;
}

boole channel__is_client_location_visible(const client_t *client, const client_t *subject)
{
    assert(0);
    return FALSE;
}

void util__free_json_message(cJSON *root, char *text)
{
    assert(0);
}

void server_msg__delivery__send_json_to_channel(cJSON *root, uint64 channel, client_t *except)
{
    assert(0);
}

void server_msg__delivery__send_json_to_all_clients(cJSON *root, client_t *except)
{
    assert(0);
}
#endif

static void check_parent(cJSON *object, int64 expected)
{
    char *wire = cJSON_PrintUnformatted(object);
    cJSON *decoded = cJSON_Parse(wire);
    cJSON *parent = cJSON_GetObjectItemCaseSensitive(decoded, "parent_channel_id");
    assert(cJSON_IsNumber(parent) && parent->valuedouble == (double)expected);
    cJSON_Delete(decoded);
    cJSON_free(wire);
}

int main(void)
{
    channel_t channels[2] = {0};
    client_t client = {0};
    g_channel_array = channels;
    g_clients_array = &client;
    g_server_settings.max_channel_count = 2;
    g_server_settings.max_client_count = 1;
    client.is_existing = TRUE;
    client.is_authenticated = TRUE;
    channels[0].is_existing = TRUE;
    channels[0].is_root_channel = TRUE;
    channels[0].parent_channel_id = (uint64)-1;
    channels[1].is_existing = TRUE;
    channels[1].channel_id = 1;
    channels[1].parent_channel_id = 300;
    server_msg__channel__send_list_to_single_client(&client);
    cJSON *items = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(captured, "message"), "channels");
    assert(cJSON_GetArraySize(items) == 2);
    check_parent(cJSON_GetArrayItem(items, 0), -1);
    check_parent(cJSON_GetArrayItem(items, 1), 300);
    for (int index = 0; index < 2; index++)
    {
        server_msg__channel__send_create_message_to_all_clients(&channels[index], &client);
        check_parent(cJSON_GetObjectItemCaseSensitive(captured, "message"), index == 0 ? -1 : 300);
    }
    cJSON_Delete(captured);
    puts("PASS four channel parent wire checks");
    return 0;
}
