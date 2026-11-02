#include "clib.h"
#include "session/session_internal.h"
#include "mbedtls/platform_util.h"

/**
 * @brief shares menu availability and worker rechecks using server-supplied group and channel rights.
 * @param const desktop_session_view_t* view -> owned snapshot
 * @param const desktop_client_t* client -> target belonging to that snapshot
 * @param desktop_action_e action -> requested group or moderation action
 * @param int64 value -> group ID or channel role for assignment
 * @return boole -> TRUE when current hints permit the action; the server still authorizes it
 */
boole desktop_session__allows(const desktop_session_view_t* view, const desktop_client_t* client, desktop_action_e action, int64 value)
{
    const desktop_tree_group_t* group = NULL;
    int64 index = 0;
    int64 kind = action == DESKTOP_ACTION_BAN ? 1 : 0;
    int64 protection = 0;
    boole negated = FALSE;
    if (!view || !client || !view->connected || view->idle || client->music)
    {
        return FALSE;
    }
    if (action == DESKTOP_ACTION_GROUP_ADD || action == DESKTOP_ACTION_GROUP_REMOVE)
    {
        if (!view->permission_system || value < 0 || value >= DESKTOP_TREE_GROUPS)
        {
            return FALSE;
        }
        group = &view->groups[value];
        return group->exists && !group->default_guest
            && (view->administrator || (client->id != view->local_id && !(client->groups[0] & 1)
                && (action == DESKTOP_ACTION_GROUP_ADD ? view->can_assign_groups && group->can_assign : view->can_remove_groups && group->can_remove)));
    }
    if (action == DESKTOP_ACTION_CHANNEL_GROUP)
    {
        return view->permission_system && !client->idle && client->access_known && value >= 0 && value < DESKTOP_TREE_CHANNEL_GROUPS
            && (value == 0 || view->channel_groups[value].exists) && (client->assignable_roles[value / 64] & (1ULL << (value % 64)));
    }
    if (action == DESKTOP_ACTION_KICK_SERVER || action == DESKTOP_ACTION_BAN)
    {
        if (client->id == view->local_id)
        {
            return FALSE;
        }
        if (view->administrator)
        {
            return TRUE;
        }
        if ((client->groups[0] & 1) || !(kind ? view->can_ban : view->can_kick))
        {
            return FALSE;
        }
        for (index = 0; index < DESKTOP_TREE_GROUPS; index++)
        {
            if (!(client->groups[index / 64] & (1ULL << (index % 64))))
            {
                continue;
            }
            group = &view->groups[index];
            if (group->negate_protection[kind])
            {
                protection = !negated || group->protection[kind] < protection ? group->protection[kind] : protection;
                negated = TRUE;
            }
            else if (!negated && group->protection[kind] > protection)
            {
                protection = group->protection[kind];
            }
        }
        return view->moderation_power[kind] >= protection;
    }
    return FALSE;
}

/**
 * @brief returns the stable attachment manager owned by the session.
 * @param desktop_session_t* session -> live GUI-owned session
 * @return desktop_attachments_t* -> borrowed manager; invalid after session destruction
 */
desktop_attachments_t* desktop_session__attachments(desktop_session_t* session)
{
    return session ? session->attachments : NULL;
}

/**
 * @brief copies a file selection into the bounded command queue; no GUI pointers cross threads.
 * @param desktop_session_t* session -> live session
 * @param const desktop_attachment_request_t* request -> destination and selected path
 * @return boole -> TRUE when queued
 */
boole desktop_session__attach(desktop_session_t* session, const desktop_attachment_request_t* request)
{
    desktop_session_command_t command = {0};
    boole queued = FALSE;
    if (!session || !request || clib__string_length(request->path, sizeof(request->path)) <= 0)
    {
        return FALSE;
    }
    command.attach = TRUE;
    command.attachment_request = *request;
    desktop_mutex__lock(session->mutex);
    if (session->running && !session->stop && session->published.connected && session->command_count < 8)
    {
        session->commands[session->command_count++] = command;
        queued = TRUE;
    }
    desktop_mutex__unlock(session->mutex);
    mbedtls_platform_zeroize(&command, sizeof(command));
    return queued;
}

/**
 * @brief queues a bounded action against the exact member and channel shown when clicked.
 * @param desktop_session_t* session -> GUI-owned connection
 * @param const desktop_action_t* action -> copied request; no caller pointers are retained
 * @return boole -> 1 when queued, zero for stale targets, invalid input or a full queue
 */
boole desktop_session__action(desktop_session_t* session, const desktop_action_t* action)
{
    desktop_session_command_t command = {0};
    int64 index = 0;
    boole queued = 0;
    boole valid = 1;
    if (!session || !action || action->type <= DESKTOP_ACTION_NONE || action->type > DESKTOP_ACTION_VOLUME
        || clib__string_length(action->text, sizeof(action->text)) < 0)
    {
        return 0;
    }
    command.action = *action;
    desktop_mutex__lock(session->mutex);
    if (action->type >= DESKTOP_ACTION_POKE)
    {
        valid = 0;
        for (index = 0; index < session->published.client_count; index++)
        {
            if (session->published.clients[index].id == action->peer
                && session->published.clients[index].serial == action->serial
                && session->published.clients[index].channel == action->channel
                && (action->peer != session->published.local_id || action->type == DESKTOP_ACTION_ACCESS
                    || (action->type >= DESKTOP_ACTION_GROUP_ADD && action->type <= DESKTOP_ACTION_CHANNEL_GROUP)))
            {
                valid = 1;
                break;
            }
        }
    }
    if (valid && session->running && !session->stop && session->published.connected && session->command_count < 8)
    {
        session->commands[session->command_count++] = command;
        queued = 1;
    }
    desktop_mutex__unlock(session->mutex);
    mbedtls_platform_zeroize(&command, sizeof(command));
    return queued;
}

/**
 * @brief queues an explicit channel save against the version originally opened by the GUI.
 * @param desktop_session_t* session -> GUI-owned connection
 * @param const desktop_channel_request_t* request -> full bounded channel draft
 * @return boole -> 1 when queued, zero when stale, invalid or unavailable
 */
boole desktop_session__channel_request(desktop_session_t* session, const desktop_channel_request_t* request)
{
    desktop_session_command_t command = {0};
    int64 index = 0;
    boole queued = 0;
    if (!session || !request || request->action <= DESKTOP_CHANNEL_NONE || request->action > DESKTOP_CHANNEL_REMOVE
        || clib__string_length(request->channel.name, sizeof(request->channel.name)) < 0
        || clib__string_length(request->channel.description, sizeof(request->channel.description)) < 0
        || clib__string_length(request->password, sizeof(request->password)) < 0)
    {
        return 0;
    }
    command.channel_request = *request;
    desktop_mutex__lock(session->mutex);
    for (index = 0; index < session->published.channel_count; index++)
    {
        if (session->published.channels[index].id == request->channel.id
            && session->published.channels[index].revision == request->channel.revision)
        {
            break;
        }
    }
    if (session->running && !session->stop && session->published.connected && !session->published.idle
        && session->command_count < 8 && index < session->published.channel_count)
    {
        session->commands[session->command_count++] = command;
        queued = 1;
    }
    desktop_mutex__unlock(session->mutex);
    mbedtls_platform_zeroize(&command, sizeof(command));
    return queued;
}
