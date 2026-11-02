#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"

// declarations
static int64 _client_msg_actions_internal__target(desktop_session_t* session, const desktop_action_t* action);

/**
 * @brief rechecks a captured target on the worker before building any outgoing action.
 * @param desktop_session_t* session -> worker-owned connection
 * @param const desktop_action_t* action -> copied GUI request
 * @return int64 -> current client slot, or -1 after a move, disconnect or identity change
 */
static int64 _client_msg_actions_internal__target(desktop_session_t* session, const desktop_action_t* action)
{
    int64 slot = desktop_session__client(session, action->peer);
    if (slot < 0 || session->model.clients[slot].serial != action->serial
        || session->model.clients[slot].channel != action->channel
        || (action->peer == session->model.local_id && action->type != DESKTOP_ACTION_ACCESS
            && !(action->type >= DESKTOP_ACTION_GROUP_ADD && action->type <= DESKTOP_ACTION_CHANNEL_GROUP)))
    {
        desktop_session__detail(session, "This client changed channel or disconnected. Select them again.", 0);
        return -1;
    }
    return slot;
}

/**
 * @brief sends existing server actions without predicting permission rules on the client.
 * @param desktop_session_t* session -> authenticated worker-owned connection
 * @param const desktop_action_t* action -> captured target and bounded values
 * @return void
 */
void client_msg__actions__send(desktop_session_t* session, const desktop_action_t* action)
{
    cJSON* message = NULL;
    boole complete = 0;
    int64 slot = -1;
    uint64 required = 0;
    const char* name = NULL;
    const desktop_client_t* target = NULL;

    if (session->model.idle && action->type != DESKTOP_ACTION_RETURN && action->type < DESKTOP_ACTION_IGNORE)
    {
        desktop_session__detail(session, "Return from idle mode before using this action.", 0);
        return;
    }
    if (action->type >= DESKTOP_ACTION_POKE)
    {
        slot = _client_msg_actions_internal__target(session, action);
        if (slot < 0)
        {
            return;
        }
        target = &session->model.clients[slot];
    }
    if (action->type == DESKTOP_ACTION_IGNORE || action->type == DESKTOP_ACTION_VOLUME)
    {
        if (action->type == DESKTOP_ACTION_IGNORE && (action->value == 0 || action->value == 1))
        {
            session->model.clients[slot].ignored = action->value != 0;
        }
        else if (action->type == DESKTOP_ACTION_VOLUME && action->value >= 0 && action->value <= 200)
        {
            session->model.clients[slot].volume_offset = action->value - 100;
            desktop_voice__channel_changed(session->voice);
        }
        return;
    }
    if (action->type >= DESKTOP_ACTION_KICK_SERVER && action->type <= DESKTOP_ACTION_CHANNEL_GROUP
        && (!desktop_session__allows(&session->model, target, action->type, action->value)
            || (action->type >= DESKTOP_ACTION_GROUP_ADD && action->catalog_revision != session->model.tree_revision)))
    {
        desktop_session__detail(session, "The group or your permissions changed. Open the menu again.", 0);
        return;
    }
    switch (action->type)
    {
        case DESKTOP_ACTION_RENAME:
            if (!action->text[0] || clib__strlen(action->text) > 100)
            {
                desktop_session__detail(session, "Enter a name between 1 and 100 UTF-8 bytes.", 0);
                return;
            }
            message = client_msg__delivery__new_message("change_client_username");
            complete = message && cJSON_AddNumberToObject(message, "client_id", (double)session->model.local_id)
                && cJSON_AddStringToObject(message, "new_username", action->text);
            break;
        case DESKTOP_ACTION_IDLE:
            message = client_msg__delivery__new_message("go_to_idle_mode_request");
            complete = message != NULL;
            break;
        case DESKTOP_ACTION_RETURN:
            if (!session->model.idle)
            {
                return;
            }
            message = client_msg__delivery__new_message("come_back_from_idle_mode_request");
            complete = message && (action->value >= 0 ? cJSON_AddNumberToObject(message, "channel_id", (double)action->value)
                : cJSON_AddNullToObject(message, "channel_id"));
            break;
        case DESKTOP_ACTION_POKE:
            if (!action->text[0] || target->music)
            {
                return;
            }
            message = client_msg__delivery__new_message("poke_client");
            complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id)
                && cJSON_AddStringToObject(message, "poke_message", action->text);
            break;
        case DESKTOP_ACTION_SERVER_INFO:
            message = client_msg__delivery__new_message("load_server_info");
            complete = message != NULL;
            break;
        case DESKTOP_ACTION_ACCESS:
            message = client_msg__delivery__new_message("channel_access_request");
            complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id)
                && cJSON_AddNumberToObject(message, "channel_id", (double)target->channel)
                && cJSON_AddStringToObject(message, "identity", session->client_keys[slot]);
            break;
        case DESKTOP_ACTION_MOVE:
            required = DESKTOP_ACCESS_MOVE;
            name = "move";
            if (action->value == target->channel || desktop_session__channel(session, action->value) < 0)
            {
                return;
            }
            break;
        case DESKTOP_ACTION_KICK_CHANNEL:
            required = DESKTOP_ACCESS_KICK_CHANNEL;
            name = "kick_from_channel";
            if (target->channel == 0)
            {
                return;
            }
            break;
        case DESKTOP_ACTION_MUTE_CHANNEL:
            required = DESKTOP_ACCESS_MUTE_CHANNEL;
            name = "mute";
            if (action->value < 0 || action->value > 86400)
            {
                return;
            }
            break;
        case DESKTOP_ACTION_STOP_STREAM:
            required = DESKTOP_ACCESS_STOP_STREAM;
            name = "stop_stream";
            break;
        case DESKTOP_ACTION_KICK_SERVER:
        case DESKTOP_ACTION_BAN:
            message = client_msg__delivery__new_message(action->type == DESKTOP_ACTION_BAN ? "ban" : "kick");
            complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id);
            break;
        case DESKTOP_ACTION_GROUP_ADD:
        case DESKTOP_ACTION_GROUP_REMOVE:
            message = client_msg__delivery__new_message(action->type == DESKTOP_ACTION_GROUP_ADD ? "add_group_to_client" : "remove_group_from_client");
            complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id)
                && cJSON_AddNumberToObject(message, "group_id", (double)action->value);
            break;
        case DESKTOP_ACTION_CHANNEL_GROUP:
            message = client_msg__delivery__new_message("moderate_client");
            complete = message && cJSON_AddStringToObject(message, "action", "channel_group")
                && cJSON_AddNumberToObject(message, "client_id", (double)target->id)
                && cJSON_AddNumberToObject(message, "channel_id", (double)target->channel)
                && cJSON_AddStringToObject(message, "identity", session->client_keys[slot])
                && cJSON_AddNumberToObject(message, "role", (double)action->value);
            break;
        default:
            return;
    }
    if (required)
    {
        if (!target->access_known || !(target->access & required) || target->idle || target->music)
        {
            desktop_session__detail(session, "Your permissions do not allow this action for this client.", 0);
            return;
        }
        message = client_msg__delivery__new_message("moderate_client");
        complete = message && cJSON_AddStringToObject(message, "action", name)
            && cJSON_AddStringToObject(message, "identity", session->client_keys[slot])
            && cJSON_AddNumberToObject(message, "client_id", (double)target->id)
            && cJSON_AddNumberToObject(message, "channel_id", (double)target->channel);
        if (action->type == DESKTOP_ACTION_MOVE)
        {
            complete = complete && cJSON_AddNumberToObject(message, "destination_channel_id", (double)action->value);
        }
        else if (action->type == DESKTOP_ACTION_MUTE_CHANNEL)
        {
            complete = complete && cJSON_AddNumberToObject(message, "duration_seconds", (double)action->value);
        }
    }
    if (client_msg__delivery__send(session, message, complete) && action->type != DESKTOP_ACTION_ACCESS && action->type != DESKTOP_ACTION_SERVER_INFO)
    {
        desktop_session__detail(session, action->type == DESKTOP_ACTION_POKE ? "Poke sent." : "Request sent. Waiting for the server.", 0);
    }
}
