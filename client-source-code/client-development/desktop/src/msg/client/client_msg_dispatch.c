#include "clib.h"
#include "attachments/attachments_internal.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief consumes one GUI command after checking current worker-owned channel state.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return void
 */
void client_msg__dispatch_command(desktop_session_t* session)
{
    desktop_session_command_t command = {0};
    boole available = 0;
    int64 slot = -1;

    desktop_mutex__lock(session->mutex);
    if (session->command_count)
    {
        command = session->commands[0];
        session->command_count--;
        clib__memmove(session->commands, session->commands + 1, sizeof(command) * session->command_count);
        clib__null_memory(&session->commands[session->command_count], sizeof(command));
        available = 1;
    }
    desktop_mutex__unlock(session->mutex);
    if (!available)
    {
        return;
    }
    if (command.attach)
    {
        client_msg__attachments__send(session, &command.attachment_request);
    }
    else if (command.channel_request.action != DESKTOP_CHANNEL_NONE)
    {
        client_msg__channel__send_change(session, &command.channel_request);
    }
    else if (command.action.type != DESKTOP_ACTION_NONE)
    {
        client_msg__actions__send(session, &command.action);
    }
    else if (command.join)
    {
        client_msg__channel__send_join_request(session, command.channel, command.text);
    }
    else if (command.edit)
    {
        client_msg__chat__send_edit(session, command.chat_sequence, command.connection_serial, command.text);
    }
    else if (command.direct)
    {
        client_msg__chat__send_direct_message(session, command.peer, command.peer_serial, command.text);
    }
    else if (command.mute)
    {
        slot = desktop_session__client(session, command.peer);
        if (slot >= 0 && session->model.clients[slot].serial == command.peer_serial)
        {
            session->model.clients[slot].muted = command.muted;
            desktop_voice__channel_changed(session->voice);
        }
    }
    else
    {
        client_msg__chat__send_channel_message(session, command.channel, command.text);
    }
    mbedtls_platform_zeroize(&command, sizeof(command));
}
