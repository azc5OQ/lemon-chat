// This file manages each client's record on the server: its slot, username,
// duplicate logins and cleanup after disconnection. During a reconnect it can
// attach a new WebSocket connection to the existing session. The socket callbacks
// and incoming messages are handled in chat_websocket.c.
#include "../../third-party/zhicheng/base64.h"
#include "channel.h"
#include "client_session.h"
#include "identity_store.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"
#include "../msg/server/server_msg.h"
#include "../memory_manager.h"
#include "../clib/clib_memory.h"
#include "../clib/clib_string.h"
#include "../util.h"
#include "audio_channel.h"
#include "../audio_websocket.h"
#include "../video_websocket.h"
#include "video_channel.h"
#include "server_logs.h"

/**
 * @brief closes connection in thread safe way
 *
 * @param uint64 client_index -> id of the client whose connection to close
 * @param boole use_readlock -> TRUE to acquire the read lock inside this function
 *
 * @return void
 *
 * @attention closing connection takes more lines of code, and this function was created to eliminate that.
 */
void client_session__close_websocket_connection(uint64 client_index, boole use_readlock)
{
    if (use_readlock == TRUE)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
    }
    if (g_clients_array[client_index].is_existing == TRUE)
    {
        if (g_clients_array[client_index].is_authenticated == TRUE)
        {
            DBG_CLOSE_CONNECTION log_info("%s %llu %s", "client_session__close_websocket_connection closing with authenticated client", client_index, "\n");
            ws_close_client(g_clients_array[client_index].p_ws_connection);
        }
        else
        {
            DBG_CLOSE_CONNECTION log_info("%s %llu %s", "client_session__close_websocket_connection client with not authenticated client", client_index, "\n");
            ws_close_client(g_clients_array[client_index].p_ws_connection);
        }
    }
    else
    {
        DBG_CLOSE_CONNECTION log_info("%s %llu %s", "client_session__close_websocket_connection client doesnt exist", client_index, "\n");
    }

    if (use_readlock == TRUE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief assigns username to client
 *
 * @param uint64 client_index -> index of client that needs new username
 * @param cstring default_name -> default part of name string
 *
 * @return boole returns true if username has been assigned, false if not. username is assigned within this function, to avoid need allocating on heap (stack returning meh)
 *
 * @attention bad code
 */
boole client_session__assign_username_for_newly_joined_client(uint64 client_index, cstring default_name)
{
    boole status = FALSE;
    char* public_key = 0;
    char candidate_username[USERNAME_MAX_LENGTH];
    char index_suffix_buffer[16];
    uint64 username_suffix_number = 0;
    uint64 i = 0;
    boole result = FALSE;

    public_key = &g_clients_array[client_index].public_key[0];
    DBG_AUTHENTICATION log_info("%s %s %s", "newly joined client public key is -> ", public_key, "\n");

    status = identity_store__is_public_key_present_in_client_stored_data(public_key);

    if (status == TRUE)
    {
        DBG_AUTHENTICATION log_info("%s", "public key present \n");
    }
    else
    {
        DBG_AUTHENTICATION log_info("%s", "public key not present \n");
        // two assumptions for safe operation
        // default name won't be too long
        // max number of clients will fit into 16 digits (easy), edge cases will have 3 digits, extremely edge cases 4

        // try "<default_name><n>" for n = 0, 1, 2, ... until a username no client is using is found
        for (username_suffix_number = 0; username_suffix_number < g_server_settings.max_client_count; username_suffix_number++)
        {
            uint64 default_name_length = 0;

            clib__null_memory(candidate_username, USERNAME_MAX_LENGTH);
            clib__copy_memory(g_server_settings.default_client_name, candidate_username, clib__utf8_string_length(g_server_settings.default_client_name), USERNAME_MAX_LENGTH);

            // as long as I know what I'm doing, shouldn't be dangerous
            clib__null_memory(index_suffix_buffer, 16);

            // itoa(username_suffix_number, index_suffix_buffer, 10); gcc on linux does not support itoa so I'm using sprintf
            sprintf(index_suffix_buffer, "%llu", username_suffix_number);

            default_name_length = clib__utf8_string_length(candidate_username);
            clib__copy_memory(&index_suffix_buffer[0], &candidate_username[default_name_length], clib__utf8_string_length(index_suffix_buffer), clib__utf8_string_length(g_server_settings.default_client_name));

            DBG_AUTHENTICATION log_info("%s %s %s", "index_suffix_buffer -> ", index_suffix_buffer, "\n");
            DBG_AUTHENTICATION log_info("%s %s %s", "candidate_username -> ", candidate_username, "\n");

            for (i = 0; i < g_server_settings.max_client_count; i++)
            {
                // is_existing needs to be guarded with mutex, while opening client, rwlock is not usable

                // log_info("%s %s %d %s", "trying username -> ", candidate_username , i, "\n");

                if (g_clients_array[i].is_existing == FALSE)
                { // client not is_existing, skip, this needs global lock
                    goto final_check;
                }

                if (g_clients_array[i].is_authenticated == FALSE)
                {
                    goto final_check;
                }

                if (i == client_index)
                { // skip current client
                    goto final_check;
                }

                status = clib__is_string_equal(g_clients_array[i].username, candidate_username);

                if (status == TRUE)
                { // username used by some of the clients, start another loop, with incremented numeric part of client's username
                    DBG_AUTHENTICATION log_info("%s %s %s", "username ", candidate_username, " it is not available \n");
                    break;
                }

final_check:

                if ((i + 1) == g_server_settings.max_client_count)
                { // if loop reached its end and username currently used in this loop was found to not be used by any of the clients
                    // go to end_loop where this newly found username will be assigned to client
                    DBG_AUTHENTICATION log_info("%s", "(i + 1) == g_server_settings.max_client_count goto end_loop \n");
                    result = TRUE;
                    goto end_loop;
                }

                // still some clients that need to be checked, not needed , included just for
            }
        }
    }

    return result; // either code jumps to end_loop or it returns false here

end_loop:
    clib__copy_memory(&candidate_username[0], &g_clients_array[client_index].username[0], clib__utf8_string_length(candidate_username), USERNAME_MAX_LENGTH);
    DBG_AUTHENTICATION log_info("%s %llu %s %s", "client: ", client_index, g_clients_array[client_index].username, "\n");

    return result;
}

/**
 * @brief finds the index of the first free client slot, or -1 if the server is full
 *
 * @return int64 free client index, or -1 if none available
 */
int64 client_session__get_new_index_for_client(void)
{
    int64 new_index = -1;
    uint64 i = 0;

    /* Reserved slots include bots and bots still winding down, while the
     * socket counter does not. The actual slot table is the source of truth. */
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].timestamp_connected == 0)
        {
            new_index = i;
            break;
        }
    }

    return new_index;
}

/**
 * @brief drops every OTHER connected client holding this public key, so a returning identity
 *        replaces its own stale session instead of being locked out by it
 *
 * @param uint64 client_index_to_keep -> the client that just proved it owns the key
 * @param cstring public_key -> the public key both sessions carry
 *
 * @return uint64 -> how many stale sessions were closed
 *
 * @attention call ONLY after the challenge response verified ownership: the public key is public,
 *            so acting on it any earlier would let anyone kick anyone off the server.
 *            the caller must hold the clients write lock
 */
uint64 client_session__disconnect_other_clients_with_same_public_key(uint64 client_index_to_keep, cstring public_key)
{
    uint64 disconnected_count = 0;
    uint64 i = 0;

    if (public_key == NULL_POINTER || public_key[0] == 0)
    {
        return 0;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_index_to_keep)
        {
            continue;
        }

        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == FALSE)
        {
            continue;
        }

        // the socket may have been closed long ago (a drop the server never saw); closing it makes that
        // client's own reader thread run the normal disconnect teardown
        log_info("%s %llu %s %llu %s", "identity takeover: client", i, "is replaced by the returning client", client_index_to_keep, "\n");
        server_logs__client_disconnect_reason(&g_clients_array[i], "identity takeover");
        client_session__close_websocket_connection(i, FALSE);

        // torn down right here, not when the socket closes: the returning client's list and the
        // disconnect broadcast must both precede its join, so nobody ever sees both sessions
        clib__write_lock(&g_channels_global_rwlock_guard);
        client_session__process_client_disconnect(i);
        clib__unlock(&g_channels_global_rwlock_guard);
        disconnected_count++;
    }

    return disconnected_count;
}

/**
 * @brief fast reconnect: hands the new socket to this identity's still-open session, so the session keeps
 *        its id, channel, name, tags and roles. the old socket is closed without any broadcast
 *
 * @param uint64 new_client_index -> the entry the new socket got at _chat_websocket_internal__onopen; emptied on success
 * @param cstring public_key -> the key the new socket just proved it owns
 *
 * @return int64 -> index of the adopted session, -1 when this key has no open session
 *
 * @attention the caller must hold the clients write lock. same rule as the takeover: only after the
 *            challenge response verified ownership, the public key alone proves nothing
 */
int64 client_session__adopt_socket_into_existing_session(uint64 new_client_index, cstring public_key)
{
    uint64 i = 0;
    uint64 timestamp_now = 0;
    client_t* old_session = NULL_POINTER;
    client_t* new_entry = NULL_POINTER;
    ws_cli_conn_t* old_ws_connection = NULL_POINTER;

    if (public_key == NULL_POINTER || public_key[0] == 0)
    {
        return -1;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == new_client_index || g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == TRUE)
        {
            old_session = &g_clients_array[i];
            break;
        }
    }

    if (old_session == NULL_POINTER)
    {
        return -1;
    }

    new_entry = &g_clients_array[new_client_index];
    audio_websocket__disconnect(old_session);
    video_websocket__disconnect(old_session);
    old_ws_connection = old_session->p_ws_connection;
    timestamp_now = util__get_timestamp_ms();

    // the per-socket half moves over; the session half (id, channel, name, tags, roles, avatar) stays
    old_session->p_ws_connection = new_entry->p_ws_connection;
    old_session->is_dh_shared_secret_agreed_upon = new_entry->is_dh_shared_secret_agreed_upon;
    old_session->is_public_key_challenge_sent = new_entry->is_public_key_challenge_sent;
    clib__copy_memory(new_entry->dh_shared_secret, old_session->dh_shared_secret, SHARED_SECRET_LENGTH, SHARED_SECRET_LENGTH);
    old_session->session_keys = new_entry->session_keys;
    atomic_store_explicit(&old_session->media_send_counter,
        atomic_load_explicit(&new_entry->media_send_counter, memory_order_relaxed), memory_order_relaxed);
    clib__copy_memory(new_entry->challenge_string, old_session->challenge_string, CHALLENGE_STRING_LENGTH, CHALLENGE_STRING_LENGTH);
    clib__null_memory(old_session->ip_address, sizeof(old_session->ip_address));
    clib__copy_memory(new_entry->ip_address, old_session->ip_address, clib__utf8_string_length(new_entry->ip_address), sizeof(old_session->ip_address));
    clib__null_memory(old_session->country_iso_code, sizeof(old_session->country_iso_code));
    clib__copy_memory(new_entry->country_iso_code, old_session->country_iso_code, clib__utf8_string_length(new_entry->country_iso_code), sizeof(old_session->country_iso_code));
    old_session->timestamp_last_maintain_connection_message_received = timestamp_now;
    old_session->timestamp_last_action = timestamp_now;
    // Replacing a socket must not move an idle client into root without a channel-join message.
    old_session->has_pending_maintainer_reset_vote = FALSE;

    // an upload that was running on the old socket ended with it
    if (old_session->file_upload_extension.file_upload_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)old_session->file_upload_extension.file_upload_buffer);
    }
    clib__null_memory(&old_session->file_upload_extension, sizeof(old_session->file_upload_extension));

    // the throwaway entry owns nothing on the heap this early in the handshake; mirrored from the
    // disconnect path anyway, so a later change there cannot turn this into a leak
    if (new_entry->tag_ids != NULL_POINTER)
    {
        cvector_free(new_entry->tag_ids);
    }
    if (new_entry->base64_avatar != NULL_POINTER)
    {
        memorymanager__free((nuint)new_entry->base64_avatar);
    }
    if (new_entry->file_upload_extension.file_upload_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)new_entry->file_upload_extension.file_upload_buffer);
    }
    clib__null_memory(new_entry, sizeof(client_t));

    log_info("%s %llu %s %llu %s", "fast reconnect: session", i, "adopted the socket of entry", new_client_index, "\n");

    // closed like any other socket; its late _chat_websocket_internal__onclose finds no entry holding this pointer
    ws_close_client(old_ws_connection);

    return (int64)i;
}

/**
 * @brief tells whether some OTHER authenticated client holds this public key (identity takeover off:
 *        such a newcomer is refused instead of replacing that session)
 *
 * @param uint64 client_index -> the newcomer, skipped
 * @param cstring public_key -> the key it just proved
 *
 * @return boole -> TRUE when another authenticated session carries the same key
 */
boole client_session__is_there_another_authenticated_client_with_same_public_key(uint64 client_index, cstring public_key)
{
    uint64 i = 0;

    if (public_key == NULL_POINTER || public_key[0] == 0)
    {
        return FALSE;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_index || g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief Checks whether an existing authenticated client has the given public key.
 * @param cstring public_key -> public key to look for
 * @return boole -> TRUE when a matching client exists; FALSE otherwise
 * @attention The caller holds the clients lock.
 */
boole client_session__is_there_a_client_with_same_public_key(cstring public_key)
{
    boole result = FALSE;
    uint64 i = 0;
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE)
        {
            continue;
        }

        if (g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key))
        {
            result = TRUE;
            break;
        }
    }
    return result;
}

/**
 * @brief the socket-open half of the same-ip rule: only one handshake at a time per ip, so a flood
 *        from one address costs one slot and not the whole table, while an established session never counts
 *
 * @param cstring ip_address -> ip address of the socket that just opened
 *
 * @return boole
 *
 * @attention the caller must hold the clients write lock
 */
boole client_session__is_there_an_unfinished_handshake_from_same_ip_address(cstring ip_address)
{
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == TRUE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].ip_address, ip_address) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief the login half of the same-ip rule, checked only once the challenge proved the identity, so
 *        a returning client is refused because of a second person on the same ip and never because of its own dropped session
 *
 * @param uint64 client_index -> the client being checked, never counted
 * @param cstring ip_address -> the client's ip address
 * @param cstring public_key -> the client's identity; older sessions holding it are earlier sessions of the same identity that are being disconnected, and they are skipped
 *
 * @return boole
 *
 * @attention the caller must hold the clients write lock. only authenticated sessions count: an unfinished
 *            handshake from the same ip is checked when it gets here itself, which lets exactly one of them in
 */
boole client_session__is_there_another_authenticated_client_with_same_ip_address(uint64 client_index, cstring ip_address, cstring public_key)
{
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_index || g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == TRUE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].ip_address, ip_address) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief this function takes care of events that must happen when client disconnects
 *
 * @param uint64 client_index -> id of the disconnecting client
 *
 * @attention caller of this function must place this function in write lock
 *
 * @return void
 */
void client_session__process_client_disconnect(uint64 client_index)
{
    uint64 channel_id = 0;
    boole is_client_also_channel_maintainer = FALSE;
    boole owns_temp_channel = FALSE;
    uint64 owned_temp_channel_id = 0;
    client_t* client = NULL_POINTER;

    DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "client_session__process_client_disconnect ", client_index, "\n");

    client = &g_clients_array[client_index];

    if (client->is_existing == TRUE)
    {
        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "client_session__process_client_disconnect is_existing TRUE ", client_index, "\n");

        // only ever-authenticated clients are recorded; the wrapper checks that itself
        server_logs__client_disconnected(client);

        // remember when this identity was last here, while the client struct still holds its key
        if (g_server_settings.is_last_seen_enabled == TRUE && client->public_key[0] != 0 && client->is_music_bot == FALSE)
        {
            char last_seen_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
            util__hash_password_to_base64(client->public_key, last_seen_identity_hash, sizeof(last_seen_identity_hash));
            identity_store__touch_identity_last_seen_by_hash(last_seen_identity_hash);
        }

        audio_channel__process_client_disconnect(client);
        video_websocket__disconnect(client);

        // a streamer leaving ends its stream for the channel, a viewer leaving drops off the streamer's count
        video_channel__process_client_leaving_channel(client, client->channel_id);

        channel_id = g_clients_array[client_index].channel_id;

        /* Idle clients use a marker value outside the valid channel range instead of a real channel. */
        is_client_also_channel_maintainer = (boole)(channel_id < g_server_settings.max_channel_count && g_channel_array[channel_id].is_existing == TRUE && g_channel_array[channel_id].is_channel_maintainer_present == TRUE && g_channel_array[channel_id].maintainer_id == client_index);

        owns_temp_channel = client->is_temp_admin_channel;
        owned_temp_channel_id = client->temp_channel_id;

        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "client_session__process_client_disconnect setting client struct to null ", client_index, "\n");

        if (client->tag_ids != NULL_POINTER)
        {
            cvector_free(client->tag_ids);
        }

        // tag_ids are the only thing stored in vector
        // if vector is NULL, that's okay

        // clear out file upload buffer
        if (client->file_upload_extension.file_upload_buffer != NULL_POINTER)
        {
            memorymanager__free((nuint)client->file_upload_extension.file_upload_buffer);
        }

        // free the heap-allocated live avatar before the struct is zeroed, or the pointer would leak
        if (client->base64_avatar != NULL_POINTER)
        {
            memorymanager__free((nuint)client->base64_avatar);
            client->base64_avatar = NULL_POINTER;
        }

        clib__null_memory(client, sizeof(client_t));

        server_msg__connection__send_client_disconnect_message_to_all_clients(client_index);

        if (owns_temp_channel == TRUE)
        {
            // Remove any temporary channel this client still owns.
            channel__destroy_temp_channel(owned_temp_channel_id);
        }
        // After returning from idle, the client may maintain a different channel.
        if (is_client_also_channel_maintainer == TRUE && g_channel_array[channel_id].is_existing == TRUE)
        {
            DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "client_session__process_client_disconnect client was maintainer of channel ", client_index, "\n");

            if (channel__replace_departed_maintainer(&g_channel_array[channel_id], client_index) == TRUE)
            {
                server_msg__channel__send_maintainer_id_to_clients_in_same_channel(&g_channel_array[channel_id], g_channel_array[channel_id].maintainer_id);
            }
        }
    }
}

/**
 * @brief tells whether a username is currently worn by some OTHER connected client. usernames are
 *        the one name a person is shown under, so every path that writes one has to ask this first.
 *
 * @param char* username -> the name to look for (exact compare, like the rename path)
 * @param uint64 client_id_to_skip -> the client the name is meant for, skipped in the scan
 *
 * @attention the caller must already hold a lock on clients_global_rwlock_guard
 *
 * @return boole TRUE when another connected client already uses this username
 */
boole client_session__is_username_taken_by_another_client(char* username, uint64 client_id_to_skip)
{
    uint64 i = 0;

    if (username == NULL_POINTER || username[0] == 0)
    {
        return FALSE;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_id_to_skip)
        {
            continue;
        }

        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].username, username) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}
