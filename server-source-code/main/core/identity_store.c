// This file remembers clients' identities, avatars and groups, and holds messages for offline clients.
#include "identity_store.h"
#include "permissions.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"

#include "../memory_manager.h"
#include "../clib/clib_memory.h"
#include "../clib/clib_string.h"
#include "../util.h"
#include "../../third-party/zhicheng/base64.h"
#include "../../third-party/ITH-sha/sha256.h"

uint64 g_offline_message_sequence_counter = 0;
offline_chat_message_t* g_offline_messages = NULL_POINTER;
pthread_mutex_t g_offline_messages_mutex = PTHREAD_MUTEX_INITIALIZER;
client_stored_data_t* g_client_stored_data;
// Take the identity or offline-message mutex after the other server locks.
// Do not take another lock while holding either of these mutexes.
pthread_mutex_t g_client_stored_data_mutex = PTHREAD_MUTEX_INITIALIZER;

// declarations

/**
 * @brief returns TRUE if the given public key is present in the stored client data
 *
 * @param char* public_key -> the public key to look for
 *
 * @return boole
 */
boole identity_store__is_public_key_present_in_client_stored_data(char* public_key)
{
    boole result = FALSE;
    boole status = FALSE;
    uint64 i = 0;

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        status = clib__is_string_equal(g_client_stored_data[i].public_key, public_key);
        if (status == TRUE)
        {
            DBG_IDENTITIES log_info("%s %s %s %s", "public key found in client stored data ", g_client_stored_data[i].public_key, public_key, "\n");
            result = TRUE;
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return result;
}

/**
 * @brief restores a re-authenticated client's groups from the persisted identity store. the store is keyed by
 *        base64(SHA256(public_key)) so the settings file never holds the raw public key. the admin group
 *        (id 0) is restored like any other group and re-grants admin. only meaningful when identities are
 *        enabled; the caller checks that.
 *
 * @param client_t* client -> the freshly authenticated client whose groups should be restored
 *
 * @note the caller must hold the clients write lock (client->group_ids and is_admin are written) and the groups
 *       read lock (g_groups_array is read). the store itself is guarded by g_client_stored_data_mutex.
 *
 * @return void
 */
void identity_store__restore_identity_groups(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 t = 0;
    uint64 group_id = 0;
    boole match_found = FALSE;
    uint64 restored_count = 0;

    if (client == NULL_POINTER)
    {
        DBG_IDENTITIES log_info("%s", "restore_identity_groups: client is NULL, nothing to restore \n");
        return;
    }

    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    DBG_IDENTITIES log_info("%s %llu %s %s %s", "restore_identity_groups: client_id", client->client_id, "connecting with identity hash [", identity_hash, "] \n");

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0)
        {
            continue;
        }

        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == FALSE)
        {
            DBG_IDENTITIES log_info("%s %llu %s %s %s", "restore_identity_groups: store slot", i, "holds a different identity [", g_client_stored_data[i].public_key, "] \n");
            continue;
        }

        match_found = TRUE;
        DBG_IDENTITIES log_info("%s %llu %s %llu %s", "restore_identity_groups: MATCH in store slot", i, "with", (uint64)g_client_stored_data[i].group_id_count, "stored group ids \n");

        for (t = 0; t < g_client_stored_data[i].group_id_count; t++)
        {
            group_id = g_client_stored_data[i].group_ids[t];

            // Guest is assigned from the current memberships after restoration.
            if (group_id == g_guest_group_id || group_id >= MAX_GROUPS || g_groups_array[group_id].is_existing == FALSE)
            {
                DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_groups: skipping stored group id", group_id, "- it no longer exists on the server \n");
                continue;
            }

            cvector_push_back(client->group_ids, (int)group_id);
            restored_count++;
            DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_groups: restored group id", group_id, "\n");

            if (group_id == ADMIN_GROUP_ID && g_server_settings.is_permission_system_enabled == TRUE)
            {
                client->is_admin = TRUE;
                DBG_IDENTITIES log_info("%s", "restore_identity_groups: admin group restored -> client re-granted admin \n");
            }
        }

        break;
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    if (match_found == FALSE)
    {
        DBG_IDENTITIES log_info("%s %s %s", "restore_identity_groups: NO stored identity matched hash [", identity_hash, "] - client starts with no restored groups (wrong passphrase, or the identity was never saved) \n");
    }
    else
    {
        DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_groups: done, restored", restored_count, "group(s) \n");
    }
}

/**
 * @brief restores a reconnecting identity's persisted avatar (matched by public-key hash) into the live
 *        client, so it can be served to others immediately without waiting for a re-upload. kept separate
 *        from group restore so it can run when avatars are allowed even if group-identities are disabled.
 *
 * @param client_t* client -> the just-authenticated client
 *
 * @note takes g_client_stored_data_mutex (a leaf lock). call with the clients lock held (it writes
 *       client->base64_avatar, a heap block freed on disconnect).
 *
 * @return void
 */
void identity_store__restore_identity_avatar(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 avatar_len = 0;

    if (client == NULL_POINTER || client->public_key[0] == 0)
    {
        return;
    }

    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            if (g_client_stored_data[i].base64_avatar[0] != 0)
            {
                avatar_len = clib__utf8_string_length(&g_client_stored_data[i].base64_avatar[0]);

                if (client->base64_avatar != NULL_POINTER)
                {
                    memorymanager__free((nuint)client->base64_avatar);
                    client->base64_avatar = NULL_POINTER;
                }

                client->base64_avatar = (char*)memorymanager__allocate(avatar_len + 1, MEMALLOC_AVATAR);
                if (client->base64_avatar != NULL_POINTER)
                {
                    clib__copy_memory(&g_client_stored_data[i].base64_avatar[0], client->base64_avatar, avatar_len, avatar_len);
                    DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_avatar: restored avatar for client_id", client->client_id, "\n");
                }
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}


/**
 * @brief Saves connected users' current groups and offline messaging keys in memory.
 * The caller holds the clients and groups locks.
 */
void identity_store__snapshot_connected_clients_into_identity_store(void)
{
    uint64 i = 0;

    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        identity_store__sync_client_identity_in_store(&g_clients_array[i]);
        identity_store__update_offline_public_key(&g_clients_array[i]);
    }
}

/**
 * @brief immediately mirrors ONE client's current groups into the in-memory identity store, so a
 *        reconnecting client gets its groups back within the same server run WITHOUT waiting for an
 *        admin "save server settings" (that save only adds disk persistence on top).
 *
 *        called right after any group change on a client. if the client has groups they overwrite (or
 *        create) its store entry; if it now has zero groups its entry is cleared, so a fully-untagged
 *        identity is forgotten - unless the slot still holds an avatar or an offline messaging key, in which case only
 *        the groups are cleared. does nothing when identities are disabled.
 *
 * @param client_t* client -> the client whose groups just changed
 *
 * @note the caller must hold the clients lock (client->group_ids is read). the store itself is guarded
 *       by g_client_stored_data_mutex, taken here.
 *
 * @return void
 */
void identity_store__sync_client_identity_in_store(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 group_count = 0;
    uint64 t = 0;
    uint64 j = 0;
    int64 slot = 0;

    if (g_server_settings.is_identity_storage_enabled == FALSE)
    {
        return;
    }

    if (client == NULL_POINTER || client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
    {
        return;
    }

    if (client->public_key[0] == 0)
    {
        return; // no key yet -> nothing to key the identity on
    }

    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    for (t = 0; t < cvector_size(client->group_ids); t++)
    {
        if ((uint64)client->group_ids[t] != g_guest_group_id)
        {
            group_count++;
        }
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    // find this identity's existing slot (if any)
    slot = -1;
    for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
    {
        if (clib__is_string_equal(g_client_stored_data[j].public_key, identity_hash) == TRUE)
        {
            slot = (int64)j;
            break;
        }
    }

    if (group_count == 0 && (slot == -1 || (g_client_stored_data[slot].base64_avatar[0] == 0
        && g_client_stored_data[slot].raw_public_key[0] == 0)))
    {
        if (slot != -1)
        {
            clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        }
        pthread_mutex_unlock(&g_client_stored_data_mutex);
        return;
    }

    // has groups but no entry yet: take the first free slot
    if (slot == -1)
    {
        for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
        {
            if (g_client_stored_data[j].public_key[0] == 0)
            {
                slot = (int64)j;
                break;
            }
        }
    }

    if (slot == -1)
    {
        DBG_IDENTITIES log_info("%s %llu %s", "sync_identity: identity store FULL, cannot store client_id", client->client_id, "\n");
        pthread_mutex_unlock(&g_client_stored_data_mutex);
        return;
    }

    // rewrite the identity fields but PRESERVE base64_avatar (nulling the whole slot would delete a
    // stored avatar). a first-free slot is already fully zeroed, so this is correct for new entries too
    clib__null_memory(&g_client_stored_data[slot].public_key[0], MAX_PUBLIC_KEY_LENGTH);
    clib__null_memory(&g_client_stored_data[slot].username[0], USERNAME_MAX_LENGTH);
    clib__null_memory(&g_client_stored_data[slot].group_ids[0], sizeof(g_client_stored_data[slot].group_ids));
    clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    clib__copy_memory(client->username, &g_client_stored_data[slot].username[0], clib__utf8_string_length(client->username), USERNAME_MAX_LENGTH - 1); // remember the last-seen username for the admin ui

    g_client_stored_data[slot].group_id_count = 0;
    for (t = 0; t < cvector_size(client->group_ids)
        && g_client_stored_data[slot].group_id_count < MAX_GROUPS_FOR_SINGLE_CLIENT; t++)
    {
        if ((uint64)client->group_ids[t] == g_guest_group_id)
        {
            continue;
        }
        g_client_stored_data[slot].group_ids[g_client_stored_data[slot].group_id_count] = (uint64)client->group_ids[t];
        g_client_stored_data[slot].group_id_count++;
    }

    DBG_IDENTITIES log_info("%s %llu %s %lld %s %llu %s %s %s", "sync_identity: client_id", client->client_id, "mirrored into slot", slot, "with", (uint64)g_client_stored_data[slot].group_id_count, "group(s), hash [", identity_hash, "] \n");

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief removes one stored identity (by its base64(SHA256(public_key)) hash) from the in-memory
 *        store. the on-disk copy is untouched until the next "save server settings" re-serializes the
 *        store without it. stripping the groups off a currently-connected holder is the caller's job.
 *
 * @param char* identity_hash -> the base64 hash identifying the identity to drop
 *
 * @return boole TRUE if a matching entry was found and cleared
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock).
 */
boole identity_store__delete_identity_from_store_by_hash(char* identity_hash)
{
    uint64 i = 0;
    boole found = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            clib__null_memory(&g_client_stored_data[i], sizeof(client_stored_data_t));
            found = TRUE;
            DBG_IDENTITIES log_info("%s %llu %s %s %s", "delete_identity: cleared store slot", i, "for hash [", identity_hash, "] \n");
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found;
}

/**
 * @brief stores (or replaces) an identity's avatar in the in-memory store, keyed by its
 *        base64(SHA256(public_key)) hash. creates an avatar-only slot if the identity has no entry
 *        yet. pass an empty/NULL base64_avatar to clear it (and drop the slot if it then holds
 *        neither groups nor an offline messaging key). the on-disk copy follows on the next settings save.
 *
 * @param char* identity_hash -> base64(SHA256(public_key)) of the identity, the store's key
 * @param char* base64_avatar -> the base64 avatar to store, or NULL/empty to clear it
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return void
 */
void identity_store__set_identity_avatar_by_hash(char* identity_hash, char* base64_avatar)
{
    uint64 i = 0;
    int64 slot = -1;
    boole clearing = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    clearing = (boole)(base64_avatar == NULL_POINTER || base64_avatar[0] == 0);

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            slot = (int64)i;
            break;
        }
    }

    if (slot == -1)
    {
        if (clearing == TRUE)
        {
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return; // nothing stored to clear
        }

        for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
        {
            if (g_client_stored_data[i].public_key[0] == 0)
            {
                slot = (int64)i;
                break;
            }
        }

        if (slot == -1)
        {
            DBG_IDENTITIES log_info("%s", "set_identity_avatar: identity store FULL, cannot store avatar \n");
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return;
        }

        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    }

    clib__null_memory(&g_client_stored_data[slot].base64_avatar[0], MAX_CLIENT_AVATAR_LENGTH);

    if (clearing == FALSE)
    {
        clib__copy_memory(base64_avatar, &g_client_stored_data[slot].base64_avatar[0], clib__utf8_string_length(base64_avatar), MAX_CLIENT_AVATAR_LENGTH - 1);
    }
    else if (g_client_stored_data[slot].group_id_count == 0 && g_client_stored_data[slot].raw_public_key[0] == 0)
    {
        // cleared the avatar and the slot holds neither groups nor an offline messaging key -> drop it entirely
        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}


/**
 * @brief stamps "this identity was connected just now" onto its stored entry, as a unix-seconds
 *        timestamp.
 *
 *        admin to have switched is_last_seen_enabled on; with either off no timestamp is ever recorded or
 *        served. like every stored-data write, the value reaches disk on the next settings save.
 *
 * @param char* identity_hash -> base64 hash of the client's public key
 *
 * @return void
 */
void identity_store__touch_identity_last_seen_by_hash(char* identity_hash)
{
    uint64 i = 0;

    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_last_seen_enabled == FALSE)
    {
        return;
    }

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            g_client_stored_data[i].last_seen_unix_seconds = (int64)time(NULL_POINTER);
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief remembers an identity's RAW rsa public key, so peers can encrypt to it while its owner is
 *        offline. only does anything while is_offline_messaging_allowed is on - with the feature off the
 *        server keeps nothing but the hash, exactly as before. creates the slot if the identity has
 *        none yet (a first-time visitor with no groups or an avatar).
 *
 * @param char* identity_hash -> base64 hash of the public key (the store's key)
 * @param char* raw_public_key -> the public key itself
 *
 * @return void
 */
void identity_store__store_identity_raw_public_key(char* identity_hash, char* raw_public_key)
{
    uint64 i = 0;
    int64 slot = -1;

    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_offline_messaging_allowed == FALSE)
    {
        return;
    }

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0 || raw_public_key == NULL_POINTER || raw_public_key[0] == 0)
    {
        return;
    }

    if (clib__utf8_string_length(raw_public_key) >= MAX_PUBLIC_KEY_LENGTH)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            slot = (int64)i;
            break;
        }
    }

    if (slot == -1)
    {
        for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
        {
            if (g_client_stored_data[i].public_key[0] == 0)
            {
                slot = (int64)i;
                break;
            }
        }

        if (slot == -1)
        {
            DBG_IDENTITIES log_info("%s", "store_identity_raw_public_key: identity store FULL \n");
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return;
        }

        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    }

    clib__null_memory(&g_client_stored_data[slot].raw_public_key[0], MAX_PUBLIC_KEY_LENGTH);
    clib__copy_memory(raw_public_key, &g_client_stored_data[slot].raw_public_key[0], clib__utf8_string_length(raw_public_key), MAX_PUBLIC_KEY_LENGTH - 1);

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief Removes the public key used to send offline messages to this identity.
 * @param identity_hash Identity whose saved public key is removed.
 */
void identity_store__clear_identity_raw_public_key(char* identity_hash)
{
    uint64 i = 0;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            clib__null_memory(&g_client_stored_data[i].raw_public_key[0], MAX_PUBLIC_KEY_LENGTH);
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}


/**
 * @brief stores one text message for an identity that is not connected right now. the payload is
 *        copied onto the heap and is never inspected: it arrived encrypted with the RECIPIENT's
 *        public key, so the server cannot read it and neither can anybody else.
 *
 * @param char* recipient_identity_hash -> recipient identity
 * @param char* sender_identity_hash -> who sent it
 * @param char* sender_name -> the sender's name, for display on delivery
 * @param char* base64_encrypted_message -> opaque ciphertext
 *
 * @return boole TRUE when the message was queued
 *
 * @note takes g_offline_messages_mutex (leaf lock)
 */
boole identity_store__queue_offline_message(char* recipient_identity_hash, char* sender_identity_hash, char* sender_name, char* base64_encrypted_message)
{
    uint64 i = 0;
    uint64 message_length = 0;
    uint64 messages_for_recipient = 0;
    int64 free_slot = -1;
    int64 oldest_slot = -1;
    uint64 oldest_sequence_number = 0;
    char* message_copy = NULL_POINTER;

    if (g_offline_messages == NULL_POINTER || recipient_identity_hash == NULL_POINTER || base64_encrypted_message == NULL_POINTER)
    {
        return FALSE;
    }

    message_length = clib__utf8_string_length(base64_encrypted_message);
    if (message_length == 0 || message_length >= MAX_OFFLINE_MESSAGE_LENGTH)
    {
        DBG_IDENTITIES log_info("%s", "queue_offline_message: empty or oversize message dropped \n");
        return FALSE;
    }

    pthread_mutex_lock(&g_offline_messages_mutex);

    // one pass: count what this recipient already has waiting, find a free slot, and remember the
    // oldest entry of this recipient in case the per-identity limit is reached
    for (i = 0; i < MAX_OFFLINE_MESSAGES; i++)
    {
        if (g_offline_messages[i].is_used == FALSE)
        {
            if (free_slot == -1)
            {
                free_slot = (int64)i;
            }
            continue;
        }

        if (clib__is_string_equal(&g_offline_messages[i].recipient_identity_hash[0], recipient_identity_hash) == TRUE)
        {
            messages_for_recipient++;

            if (oldest_slot == -1 || g_offline_messages[i].sequence_number < oldest_sequence_number)
            {
                oldest_slot = (int64)i;
                oldest_sequence_number = g_offline_messages[i].sequence_number;
            }
        }
    }

    // recipient is at their personal limit: drop their oldest to make room, so a busy conversation
    // keeps the most recent messages instead of refusing new ones forever
    if (messages_for_recipient >= MAX_OFFLINE_MESSAGES_PER_IDENTITY && oldest_slot != -1)
    {
        if (g_offline_messages[oldest_slot].base64_encrypted_message != NULL_POINTER)
        {
            memorymanager__free((nuint)g_offline_messages[oldest_slot].base64_encrypted_message);
        }
        clib__null_memory(&g_offline_messages[oldest_slot], sizeof(offline_chat_message_t));
        free_slot = oldest_slot;
    }

    if (free_slot == -1)
    {
        DBG_IDENTITIES log_info("%s", "queue_offline_message: queue FULL, message refused \n");
        pthread_mutex_unlock(&g_offline_messages_mutex);
        return FALSE;
    }

    message_copy = (char*)memorymanager__allocate(message_length + 1, MEMALLOC_OFFLINE_MESSAGE);
    if (message_copy == NULL_POINTER)
    {
        pthread_mutex_unlock(&g_offline_messages_mutex);
        return FALSE;
    }

    clib__null_memory(&g_offline_messages[free_slot], sizeof(offline_chat_message_t));
    clib__copy_memory(base64_encrypted_message, message_copy, message_length, message_length);

    g_offline_messages[free_slot].is_used = TRUE;
    g_offline_messages[free_slot].base64_encrypted_message = message_copy;
    g_offline_messages[free_slot].message_length = message_length;
    g_offline_messages[free_slot].queued_unix_seconds = (int64)time(NULL_POINTER);
    g_offline_message_sequence_counter++;
    g_offline_messages[free_slot].sequence_number = g_offline_message_sequence_counter;

    clib__copy_memory(recipient_identity_hash, &g_offline_messages[free_slot].recipient_identity_hash[0], clib__utf8_string_length(recipient_identity_hash), IDENTITY_HASH_MAX_LENGTH - 1);

    if (sender_identity_hash != NULL_POINTER)
    {
        clib__copy_memory(sender_identity_hash, &g_offline_messages[free_slot].sender_identity_hash[0], clib__utf8_string_length(sender_identity_hash), IDENTITY_HASH_MAX_LENGTH - 1);
    }

    if (sender_name != NULL_POINTER)
    {
        clib__copy_memory(sender_name, &g_offline_messages[free_slot].sender_name[0], clib__utf8_string_length(sender_name), USERNAME_MAX_LENGTH - 1);
    }

    DBG_IDENTITIES log_info("%s %llu %s", "queue_offline_message: queued as sequence", g_offline_messages[free_slot].sequence_number, "\n");

    pthread_mutex_unlock(&g_offline_messages_mutex);

    return TRUE;
}

/**
 * @brief drops every queued message for one identity, freeing their payloads. used after delivery
 *        and when an identity is deleted from the store.
 *
 * @param char* identity_hash -> whose messages to drop
 *
 * @return void
 *
 * @note takes g_offline_messages_mutex (leaf lock)
 */
void identity_store__free_offline_messages_for_identity(char* identity_hash)
{
    uint64 i = 0;

    if (g_offline_messages == NULL_POINTER || identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    pthread_mutex_lock(&g_offline_messages_mutex);

    for (i = 0; i < MAX_OFFLINE_MESSAGES; i++)
    {
        if (g_offline_messages[i].is_used == TRUE && clib__is_string_equal(&g_offline_messages[i].recipient_identity_hash[0], identity_hash) == TRUE)
        {
            if (g_offline_messages[i].base64_encrypted_message != NULL_POINTER)
            {
                memorymanager__free((nuint)g_offline_messages[i].base64_encrypted_message);
            }
            clib__null_memory(&g_offline_messages[i], sizeof(offline_chat_message_t));
        }
    }

    pthread_mutex_unlock(&g_offline_messages_mutex);
}


/**
 * @brief copies an identity's stored avatar (by hash) into out_buffer. out_buffer is always
 *        null-terminated, and the copy is truncated to out_buffer_size - 1 bytes.
 *
 * @param char* identity_hash -> base64(SHA256(public_key)) of the identity to look up
 * @param char* out_buffer -> receives the stored base64 avatar, emptied first
 * @param uint64 out_buffer_size -> size of out_buffer in bytes, including the null terminator
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return boole -> TRUE only if the identity was found and held a non-empty avatar
 */
boole identity_store__get_identity_avatar_by_hash(char* identity_hash, char* out_buffer, uint64 out_buffer_size)
{
    uint64 i = 0;
    boole found = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0 || out_buffer == NULL_POINTER || out_buffer_size == 0)
    {
        return FALSE;
    }

    out_buffer[0] = 0;

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            if (g_client_stored_data[i].base64_avatar[0] != 0)
            {
                clib__copy_memory(&g_client_stored_data[i].base64_avatar[0], out_buffer, clib__utf8_string_length(&g_client_stored_data[i].base64_avatar[0]), out_buffer_size - 1);
                found = TRUE;
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found;
}

/**
 * @brief adds or removes one group id on a stored identity (by hash), for admin group management of
 *        identities that may be offline.
 *
 *        removing an identity's last group clears the whole entry, matching how the store never holds
 *        tagless identities. disk persistence still waits for the next "save server settings".
 *        stripping/adding the group on a currently-connected holder is the caller's job.
 *
 * @param char* identity_hash -> base64 hash of the identity to modify
 * @param uint64 group_id -> the group id to add or remove
 * @param boole add -> TRUE to add the group, FALSE to remove it
 *
 * @return boole -> TRUE if the identity existed (whether or not the group set actually changed)
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock).
 */
boole identity_store__modify_identity_group_in_store(char* identity_hash, uint64 group_id, boole add)
{
    uint64 i = 0;
    uint64 t = 0;
    boole found_identity = FALSE;
    boole has_group = FALSE;
    uint64 group_index = 0;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0 || group_id == g_guest_group_id)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == FALSE)
        {
            continue;
        }

        found_identity = TRUE;

        for (t = 0; t < g_client_stored_data[i].group_id_count; t++)
        {
            if (g_client_stored_data[i].group_ids[t] == group_id)
            {
                has_group = TRUE;
                group_index = t;
                break;
            }
        }

        if (add == TRUE)
        {
            if (has_group == FALSE && g_client_stored_data[i].group_id_count < MAX_GROUPS_FOR_SINGLE_CLIENT)
            {
                g_client_stored_data[i].group_ids[g_client_stored_data[i].group_id_count] = group_id;
                g_client_stored_data[i].group_id_count++;
            }
        }
        else
        {
            if (has_group == TRUE)
            {
                for (t = group_index; (t + 1) < g_client_stored_data[i].group_id_count; t++)
                {
                    g_client_stored_data[i].group_ids[t] = g_client_stored_data[i].group_ids[t + 1];
                }
                g_client_stored_data[i].group_id_count--;

                // an identity with no groups left is dropped entirely, like everywhere else
                if (g_client_stored_data[i].group_id_count == 0 && g_client_stored_data[i].raw_public_key[0] == 0
                    && g_client_stored_data[i].base64_avatar[0] == 0)
                {
                    clib__null_memory(&g_client_stored_data[i], sizeof(client_stored_data_t));
                }
            }
        }

        DBG_IDENTITIES log_info("%s %s %s %llu %s %s %s", "modify_identity_group:", (add == TRUE) ? "added" : "removed", "group", group_id, (add == TRUE) ? "to" : "from", "identity [", identity_hash);

        break;
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found_identity;
}

/**
 * @brief Removes a deleted group from saved memberships before its id can be reused.
 * @param group_id -> group being deleted
 */
void identity_store__remove_group_from_all_identities(uint64 group_id)
{
    uint64 i = 0;
    uint64 source = 0;
    uint64 destination = 0;
    client_stored_data_t* identity = NULL_POINTER;

    pthread_mutex_lock(&g_client_stored_data_mutex);
    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        identity = &g_client_stored_data[i];
        destination = 0;
        for (source = 0; source < identity->group_id_count; source++)
        {
            if (identity->group_ids[source] != group_id)
            {
                identity->group_ids[destination++] = identity->group_ids[source];
            }
        }
        identity->group_id_count = destination;
    }
    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief Saves an offline messaging key only when the user's groups allow receiving offline messages.
 * The caller holds the clients and groups locks.
 * @param client Connected user whose saved key is updated.
 */
void identity_store__update_offline_public_key(client_t* client)
{
    char identity_hash[IDENTITY_HASH_MAX_LENGTH];

    if (client == NULL_POINTER || client->is_existing == FALSE || client->is_authenticated == FALSE
        || client->is_music_bot == TRUE || client->public_key[0] == 0)
    {
        return;
    }
    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));
    if (g_server_settings.is_offline_messaging_allowed == TRUE
        && permissions__is_allowed_in_groups(client, PERMISSION_RECEIVE_OFFLINE_MESSAGES) == TRUE)
    {
        identity_store__store_identity_raw_public_key(identity_hash, client->public_key);
        identity_store__sync_client_identity_in_store(client);
    }
    else
    {
        identity_store__clear_identity_raw_public_key(identity_hash);
    }
}
