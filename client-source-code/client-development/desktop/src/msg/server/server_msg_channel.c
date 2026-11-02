#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief validates and inserts one channel record, preserving server IDs and parentage.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const cJSON* data -> borrowed server state record
 * @return boole -> 1 when the bounded state accepted this channel
 */
boole server_msg__channel__read_channel(desktop_session_t* session, const cJSON* data)
{
    desktop_channel_t channel = {0};
    int64 slot = -1;
    int64 index = 0;
    int64 value = 0;
    boole editing = clib__strcmp(server_msg__read_string(data, "type"), "channel_edit") == 0;
    const char* names[4] = {"required_talk_power", "required_message_power", "required_picture_power", "required_stream_power"};

    channel.id = server_msg__read_number(data, "channel_id");
    channel.icon = -1;
    slot = desktop_session__channel(session, channel.id);
    if (editing && slot >= 0)
    {
        channel = session->model.channels[slot];
    }
    else
    {
        channel.parent = server_msg__read_number(data, "parent_channel_id");
    }
    channel.password = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_using_password"));
    channel.audio = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_audio_enabled"));
    channel.video = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_video_stream_enabled"));
    channel.icon = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "has_channel_icon"))
        ? server_msg__read_number(data, "channel_icon_id") : -1;
    channel.temporary = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_temp_channel"));
    channel.limited = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_client_limit_active"));
    value = server_msg__read_number(data, "max_client_count");
    if (value > 2147483647)
    {
        return 0;
    }
    channel.capacity = value < 0 ? 0 : (int)value;
    for (index = 0; index < 4; index++)
    {
        value = server_msg__read_number(data, names[index]);
        if (value > 1000)
        {
            return 0;
        }
        channel.powers[index] = value < 0 ? 0 : (int)value;
    }
    if (channel.id < 0 || !clib__string_copy(channel.name, sizeof(channel.name), server_msg__read_string(data, editing ? "channel_name" : "name"))
        || !clib__string_copy(channel.description, sizeof(channel.description), server_msg__read_string(data, editing ? "channel_description" : "description"))
        || (slot < 0 && session->model.channel_count == DESKTOP_SESSION_CHANNELS))
    {
        return 0;
    }
    if (slot < 0)
    {
        slot = (int)session->model.channel_count++;
    }
    channel.revision = ++session->next_channel_revision;
    session->model.channels[slot] = channel;
    return 1;
}
