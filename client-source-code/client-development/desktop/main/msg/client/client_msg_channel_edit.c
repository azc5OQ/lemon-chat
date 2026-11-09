#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"

// declarations
static boole _client_msg_channel_edit_internal__allowed(const desktop_channel_t *current, const desktop_channel_request_t *request);

/**
 * @brief checks changed fields against the server's current hints; the server remains authoritative.
 * @param const desktop_channel_t* current -> current worker-owned channel
 * @param const desktop_channel_request_t* request -> explicit saved draft
 * @return boole -> nonzero when every changed field is currently permitted
 */
static boole _client_msg_channel_edit_internal__allowed(const desktop_channel_t *current, const desktop_channel_request_t *request)
{
    const desktop_channel_t *draft = &request->channel;
    uint64 changed = 0;
    changed |= clib__strcmp(current->name, draft->name) ? DESKTOP_CHANNEL_NAME : 0;
    changed |= clib__strcmp(current->description, draft->description) ? DESKTOP_CHANNEL_DESCRIPTION : 0;
    changed |= request->replace_password ? DESKTOP_CHANNEL_PASSWORD : 0;
    changed |= clib__memcmp(current->powers, draft->powers, sizeof(current->powers)) ? DESKTOP_CHANNEL_POWERS : 0;
    changed |= current->audio != draft->audio || current->video != draft->video || current->limited != draft->limited || current->capacity != draft->capacity ? DESKTOP_CHANNEL_OPTIONS : 0;
    if (current->temporary != draft->temporary && !(draft->temporary ? current->can_temporary : current->can_permanent))
    {
        return 0;
    }
    return current->access_known && !(changed & ~current->access);
}

/**
 * @brief sends channel creation, an explicit edit, deletion or an access query using existing messages.
 * @param desktop_session_t* session -> authenticated worker-owned connection
 * @param const desktop_channel_request_t* request -> copied draft with original channel revision
 * @return void
 */
void client_msg__channel__send_change(desktop_session_t *session, const desktop_channel_request_t *request)
{
    int64 slot = desktop_session__channel(session, request->channel.id);
    const desktop_channel_t *current = slot >= 0 ? &session->model.channels[slot] : NULL;
    const desktop_channel_t *draft = &request->channel;
    cJSON *message = NULL_POINTER;
    boole complete = 0;
    int64 index = 0;
    const char *type = NULL_POINTER;
    const char *names[4] = { "required_talk_power", "required_message_power", "required_picture_power", "required_stream_power" };
    if (!current || current->revision != draft->revision || session->model.idle)
    {
        desktop_session__detail(session, "This channel changed or was removed. Reopen its settings before saving.", 0);
        return;
    }
    if (request->action == DESKTOP_CHANNEL_ACCESS)
    {
        message = client_msg__delivery__new_message("channel_access_request");
        complete = message && cJSON_AddNumberToObject(message, "channel_id", (double)current->id);
        client_msg__delivery__send(session, message, complete);
        return;
    }
    if (request->action == DESKTOP_CHANNEL_REMOVE)
    {
        if (!current->id || !current->access_known || !(current->access & DESKTOP_CHANNEL_DELETE))
        {
            desktop_session__detail(session, "This channel cannot be deleted with your current permissions.", 0);
            return;
        }
        message = client_msg__delivery__new_message("delete_channel_request");
        complete = message && cJSON_AddNumberToObject(message, "channel_id", (double)current->id);
        client_msg__delivery__send(session, message, complete);
        return;
    }
    if (request->action == DESKTOP_CHANNEL_CREATE)
    {
        if (current->temporary || (!session->model.can_create_channel && !session->model.can_create_temp_channel))
        {
            desktop_session__detail(session, "You cannot create a channel here with your current permissions.", 0);
            return;
        }
        type = "create_channel_request";
    }
    else if (request->action == DESKTOP_CHANNEL_EDIT)
    {
        if (!_client_msg_channel_edit_internal__allowed(current, request))
        {
            desktop_session__detail(session, "Your permissions do not allow these channel changes.", 0);
            return;
        }
        type = "edit_channel_request";
    }
    else
    {
        return;
    }
    if (!draft->name[0] || clib__utf8_string_length_check_max_length(draft->name, 50) < 0 || clib__utf8_string_length_check_max_length(request->password, 30) < 0 || draft->capacity < 0 || (draft->limited && !draft->capacity))
    {
        desktop_session__detail(session, "Use a name up to 50 characters, a password up to 30, and a positive member limit.", 0);
        return;
    }
    for (index = 0; index < 4; index++)
    {
        if (draft->powers[index] < 0 || draft->powers[index] > 1000)
        {
            desktop_session__detail(session, "Channel power requirements must be between 0 and 1000.", 0);
            return;
        }
    }
    if (session->model.server_capacity > 0 && draft->capacity > session->model.server_capacity)
    {
        desktop_session__detail(session, "The channel member limit cannot exceed the server's slot limit.", 0);
        return;
    }
    message = client_msg__delivery__new_message(type);
    complete = message && cJSON_AddNumberToObject(message, request->action == DESKTOP_CHANNEL_CREATE ? "parent_channel_id" : "channel_id", (double)current->id) && cJSON_AddStringToObject(message, "channel_name", draft->name) && cJSON_AddStringToObject(message, "channel_description", draft->description) && cJSON_AddBoolToObject(message, "is_audio_enabled", draft->audio != 0) && cJSON_AddBoolToObject(message, "is_video_stream_enabled", draft->video != 0) && cJSON_AddBoolToObject(message, "is_client_limit_active", draft->limited != 0) && cJSON_AddNumberToObject(message, "max_client_count", draft->capacity);
    if (request->action == DESKTOP_CHANNEL_CREATE || request->replace_password)
    {
        complete = complete && cJSON_AddStringToObject(message, "channel_password", request->password);
    }
    if (request->action == DESKTOP_CHANNEL_EDIT && current->temporary != draft->temporary)
    {
        complete = complete && cJSON_AddBoolToObject(message, "is_temp_channel", draft->temporary != 0);
    }
    for (index = 0; index < 4; index++)
    {
        complete = complete && cJSON_AddNumberToObject(message, names[index], draft->powers[index]);
    }
    if (client_msg__delivery__send(session, message, complete))
    {
        desktop_session__detail(session, "Channel changes sent. Waiting for the server.", 0);
    }
}
