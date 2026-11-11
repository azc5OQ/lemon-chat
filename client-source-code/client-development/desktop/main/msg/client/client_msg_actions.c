#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"

// declarations
static int64 _client_msg_actions_internal__target(desktop_session_t *session, const desktop_action_t *action);

/**
 * @brief rechecks a captured target on the worker before building any outgoing action.
 * @param desktop_session_t* session -> worker-owned connection
 * @param const desktop_action_t* action -> copied GUI request
 * @return int64 -> current client slot, or -1 after a move, disconnect or identity change
 */
static int64 _client_msg_actions_internal__target(desktop_session_t *session, const desktop_action_t *action)
{
    int64 slot = desktop_session__client(session, action->peer);

    // refuse a changed target
    if (slot < 0 || session->model.clients[slot].serial != action->serial || session->model.clients[slot].channel != action->channel || (action->peer == session->model.local_id && action->type != DESKTOP_ACTION_ACCESS && !(action->type >= DESKTOP_ACTION_GROUP_ADD && action->type <= DESKTOP_ACTION_CHANNEL_GROUP)))
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
void client_msg__actions__send(desktop_session_t *session, const desktop_action_t *action)
{
    cJSON *message = NULL_POINTER;
    boole complete = 0;
    int64 slot = -1;
    uint64 required = 0;
    const char *name = NULL_POINTER;
    const desktop_client_t *target = NULL_POINTER;

    // refuse actions while idle
    if (session->model.idle && action->type != DESKTOP_ACTION_RETURN && action->type < DESKTOP_ACTION_IGNORE)
    {
        desktop_session__detail(session, "Return from idle mode before using this action.", 0);
        return;
    }

    // find the target client
    if (action->type >= DESKTOP_ACTION_POKE)
    {
        slot = _client_msg_actions_internal__target(session, action);
        if (slot < 0)
        {
            return;
        }
        target = &session->model.clients[slot];
    }

    // store ignore or volume
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

    // check permissions and revision
    if (action->type >= DESKTOP_ACTION_KICK_SERVER && action->type <= DESKTOP_ACTION_CHANNEL_GROUP && (!desktop_session__allows(&session->model, target, action->type, action->value) || (action->type >= DESKTOP_ACTION_GROUP_ADD && action->catalog_revision != session->model.tree_revision)))
    {
        desktop_session__detail(session, "The group or your permissions changed. Open the menu again.", 0);
        return;
    }

    // build the request
    switch (action->type)
    {
    case DESKTOP_ACTION_RENAME:
        // check the new name
        if (!action->text[0] || clib__strlen(action->text) > 100)
        {
            desktop_session__detail(session, "Enter a name between 1 and 100 UTF-8 bytes.", 0);
            return;
        }

        // build the rename request
        message = client_msg__delivery__new_message("change_client_username");
        complete = message && cJSON_AddNumberToObject(message, "client_id", (double)session->model.local_id) && cJSON_AddStringToObject(message, "new_username", action->text);
        break;
    case DESKTOP_ACTION_IDLE:
        // build the idle request
        message = client_msg__delivery__new_message("go_to_idle_mode_request");
        complete = message != NULL;
        break;
    case DESKTOP_ACTION_RETURN:
        // skip when not idle
        if (!session->model.idle)
        {
            return;
        }

        // build the return request
        message = client_msg__delivery__new_message("come_back_from_idle_mode_request");
        complete = message && (action->value >= 0 ? cJSON_AddNumberToObject(message, "channel_id", (double)action->value) : cJSON_AddNullToObject(message, "channel_id"));
        break;
    case DESKTOP_ACTION_POKE:
        // check text and target
        if (!action->text[0] || target->music)
        {
            return;
        }

        // build the poke request
        message = client_msg__delivery__new_message("poke_client");
        complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id) && cJSON_AddStringToObject(message, "poke_message", action->text);
        break;
    case DESKTOP_ACTION_SERVER_INFO:
        // build the info request
        message = client_msg__delivery__new_message("load_server_info");
        complete = message != NULL;
        break;
    case DESKTOP_ACTION_ACCESS:
        // build the access request
        message = client_msg__delivery__new_message("channel_access_request");
        complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id) && cJSON_AddNumberToObject(message, "channel_id", (double)target->channel) && cJSON_AddStringToObject(message, "identity", session->client_keys[slot]);
        break;
    case DESKTOP_ACTION_MOVE:
        // prepare the move action
        required = DESKTOP_ACCESS_MOVE;
        name = "move";
        if (action->value == target->channel || desktop_session__channel(session, action->value) < 0)
        {
            return;
        }
        break;
    case DESKTOP_ACTION_KICK_CHANNEL:
        // prepare the channel kick
        required = DESKTOP_ACCESS_KICK_CHANNEL;
        name = "kick_from_channel";
        if (target->channel == 0)
        {
            return;
        }
        break;
    case DESKTOP_ACTION_MUTE_CHANNEL:
        // prepare the channel mute
        required = DESKTOP_ACCESS_MUTE_CHANNEL;
        name = "mute";
        if (action->value < 0 || action->value > 86400)
        {
            return;
        }
        break;
    case DESKTOP_ACTION_STOP_STREAM:
        // prepare the stream stop
        required = DESKTOP_ACCESS_STOP_STREAM;
        name = "stop_stream";
        break;
    case DESKTOP_ACTION_KICK_SERVER:
    case DESKTOP_ACTION_BAN:
        // build kick or ban
        message = client_msg__delivery__new_message(action->type == DESKTOP_ACTION_BAN ? "ban" : "kick");
        complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id);
        break;
    case DESKTOP_ACTION_GROUP_ADD:
    case DESKTOP_ACTION_GROUP_REMOVE:
        // build the group request
        message = client_msg__delivery__new_message(action->type == DESKTOP_ACTION_GROUP_ADD ? "add_group_to_client" : "remove_group_from_client");
        complete = message && cJSON_AddNumberToObject(message, "client_id", (double)target->id) && cJSON_AddNumberToObject(message, "group_id", (double)action->value);
        break;
    case DESKTOP_ACTION_CHANNEL_GROUP:
        // build channel group request
        message = client_msg__delivery__new_message("moderate_client");
        complete = message && cJSON_AddStringToObject(message, "action", "channel_group") && cJSON_AddNumberToObject(message, "client_id", (double)target->id) && cJSON_AddNumberToObject(message, "channel_id", (double)target->channel) && cJSON_AddStringToObject(message, "identity", session->client_keys[slot]) && cJSON_AddNumberToObject(message, "role", (double)action->value);
        break;
    default:
        return;
    }

    // build the moderation request
    if (required)
    {
        // check the target access
        if (!target->access_known || !(target->access & required) || target->idle || target->music)
        {
            desktop_session__detail(session, "Your permissions do not allow this action for this client.", 0);
            return;
        }

        // create the message
        message = client_msg__delivery__new_message("moderate_client");
        complete = message && cJSON_AddStringToObject(message, "action", name) && cJSON_AddStringToObject(message, "identity", session->client_keys[slot]) && cJSON_AddNumberToObject(message, "client_id", (double)target->id) && cJSON_AddNumberToObject(message, "channel_id", (double)target->channel);

        // add the action value
        if (action->type == DESKTOP_ACTION_MOVE)
        {
            complete = complete && cJSON_AddNumberToObject(message, "destination_channel_id", (double)action->value);
        }
        else if (action->type == DESKTOP_ACTION_MUTE_CHANNEL)
        {
            complete = complete && cJSON_AddNumberToObject(message, "duration_seconds", (double)action->value);
        }
    }

    // send the request
    if (client_msg__delivery__send(session, message, complete) && action->type != DESKTOP_ACTION_ACCESS && action->type != DESKTOP_ACTION_SERVER_INFO)
    {
        desktop_session__detail(session, action->type == DESKTOP_ACTION_POKE ? "Poke sent." : "Request sent. Waiting for the server.", 0);
    }
}
