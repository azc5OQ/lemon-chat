// This file remembers clients' identities, aliases, avatars and tags, and holds messages for offline clients.
#include "identity_store.h"
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
static boole _identity_store_internal__are_strings_equal_ignoring_ascii_case(char* string1, char* string2);

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
 * @brief restores a re-authenticated client's tags from the persisted identity store. the store is keyed by
 *        base64(SHA256(public_key)) so the settings file never holds the raw public key. the admin tag
 *        (id 0) is restored like any other tag and re-grants admin. only meaningful when identities are
 *        enabled; the caller checks that.
 *
 * @param client_t* client -> the freshly authenticated client whose tags should be restored
 *
 * @note the caller must hold the clients write lock (client->tag_ids and is_admin are written) and the tags
 *       read lock (g_tags_array is read). the store itself is guarded by g_client_stored_data_mutex.
 *
 * @return void
 */
void identity_store__restore_identity_tags(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 t = 0;
    uint64 tag_id = 0;
    boole match_found = FALSE;
    uint64 restored_count = 0;

    if (client == NULL_POINTER)
    {
        DBG_IDENTITIES log_info("%s", "restore_identity_tags: client is NULL, nothing to restore \n");
        return;
    }

    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    DBG_IDENTITIES log_info("%s %llu %s %s %s", "restore_identity_tags: client_id", client->client_id, "connecting with identity hash [", identity_hash, "] \n");

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0)
        {
            continue;
        }

        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == FALSE)
        {
            DBG_IDENTITIES log_info("%s %llu %s %s %s", "restore_identity_tags: store slot", i, "holds a different identity [", g_client_stored_data[i].public_key, "] \n");
            continue;
        }

        match_found = TRUE;
        DBG_IDENTITIES log_info("%s %llu %s %llu %s", "restore_identity_tags: MATCH in store slot", i, "with", (uint64)g_client_stored_data[i].tag_id_count, "stored tag ids \n");

        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            tag_id = g_client_stored_data[i].tag_ids[t];

            // keep the admin tag (id 0) always; for every other tag, skip it if it no longer exists
            if (tag_id != ADMIN_TAG_ID && (tag_id >= MAX_TAGS || g_tags_array[tag_id].is_existing == FALSE))
            {
                DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_tags: skipping stored tag id", tag_id, "- it no longer exists on the server \n");
                continue;
            }

            cvector_push_back(client->tag_ids, (int)tag_id);
            restored_count++;
            DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_tags: restored tag id", tag_id, "\n");

            if (tag_id == ADMIN_TAG_ID)
            {
                client->is_admin = TRUE;
                DBG_IDENTITIES log_info("%s", "restore_identity_tags: admin tag restored -> client re-granted admin \n");
            }
        }

        break;
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    if (match_found == FALSE)
    {
        DBG_IDENTITIES log_info("%s %s %s", "restore_identity_tags: NO stored identity matched hash [", identity_hash, "] - client starts with no restored tags (wrong passphrase, or the identity was never saved) \n");
    }
    else
    {
        DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_tags: done, restored", restored_count, "tag(s) \n");
    }
}

/**
 * @brief restores a reconnecting identity's persisted avatar (matched by public-key hash) into the live
 *        client, so it can be served to others immediately without waiting for a re-upload. kept separate
 *        from tag restore so it can run when avatars are allowed even if tag-identities are disabled.
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
 * @brief restores an identity's admin-registered alias (by public-key hash) into the live client.
 *        fixed-size copy, no allocation. does nothing when no alias is stored.
 *
 * @param client_t* client -> the just-authenticated client whose alias is being restored
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return void
 */
void identity_store__restore_identity_alias(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;

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
            if (g_client_stored_data[i].alias[0] != 0)
            {
                clib__null_memory(&client->alias[0], USERNAME_MAX_LENGTH);
                clib__copy_memory(&g_client_stored_data[i].alias[0], &client->alias[0], clib__utf8_string_length(&g_client_stored_data[i].alias[0]), USERNAME_MAX_LENGTH - 1);
                DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_alias: restored alias for client_id", client->client_id, "\n");
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief snapshots every connected, authenticated client that currently owns at least one tag into the
 *        in-memory identity store, keyed by base64(SHA256(public_key)).
 *
 *        an existing entry for that hash is overwritten; otherwise the first free slot is used. called at
 *        "save server settings" time so the store mirrors the tags people currently wear, without
 *        persisting on every tag change. does nothing when identities are disabled. while offline
 *        messages are enabled it also captures the raw public key of every connected registered client,
 *        before the tagless skip, so keys are not missed for users who wear no tags.
 *
 * @note the caller must hold the clients read lock (g_clients_array and each client->tag_ids are read).
 *       the store itself is guarded by g_client_stored_data_mutex.
 *
 * @return void
 */
void identity_store__snapshot_connected_clients_into_identity_store(void)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    client_t* client = NULL_POINTER;
    uint64 client_index = 0;
    uint64 tag_count = 0;
    uint64 t = 0;
    uint64 j = 0;
    int64 slot = 0;

    if (g_server_settings.is_identity_storage_enabled == FALSE)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (client_index = 0; client_index < g_server_settings.max_client_count; client_index++)
    {
        client = &g_clients_array[client_index];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
        {
            continue;
        }

        // an admin save is when the file is brought up to date, so collect the raw public keys of
        // everybody connected right now - BEFORE the tagless skip below. capturing only at
        // authentication missed every identity that was already connected when the feature was
        // switched on, and a registered user with no tags (the normal case) was skipped entirely,
        // so the key never reached the file at all
        if (g_server_settings.is_offline_messaging_allowed == TRUE && client->is_registered == TRUE && client->public_key[0] != 0)
        {
            char offline_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
            util__hash_password_to_base64(client->public_key, offline_identity_hash, sizeof(offline_identity_hash));

            // the store mutex is already held here; the by-hash helper takes it too, so write
            // through the slot directly instead of calling it (that would self-deadlock)
            for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
            {
                if (clib__is_string_equal(g_client_stored_data[j].public_key, offline_identity_hash) == TRUE)
                {
                    clib__null_memory(&g_client_stored_data[j].raw_public_key[0], MAX_PUBLIC_KEY_LENGTH);
                    clib__copy_memory(client->public_key, &g_client_stored_data[j].raw_public_key[0], clib__utf8_string_length(client->public_key), MAX_PUBLIC_KEY_LENGTH - 1);
                    break;
                }
            }
        }

        tag_count = cvector_size(client->tag_ids);
        if (tag_count == 0)
        {
            DBG_IDENTITIES log_info("%s %llu %s", "snapshot_identities: client_id", client->client_id, "has no tags -> not stored \n");
            continue;
        }

        util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

        // find an existing entry for this identity, else the first free slot
        slot = -1;
        for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
        {
            if (clib__is_string_equal(g_client_stored_data[j].public_key, identity_hash) == TRUE)
            {
                slot = (int64)j;
                break;
            }
        }
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
            DBG_IDENTITIES log_info("%s %llu %s", "snapshot_identities: identity store FULL, cannot store client_id", client->client_id, "\n");
            continue; // store is full
        }

        // rewrite the identity fields but PRESERVE base64_avatar and alias - nulling the whole slot
        // would delete them (sync_client_identity_in_store already preserves them the same way)
        clib__null_memory(&g_client_stored_data[slot].public_key[0], MAX_PUBLIC_KEY_LENGTH);
        clib__null_memory(&g_client_stored_data[slot].username[0], USERNAME_MAX_LENGTH);
        clib__null_memory(&g_client_stored_data[slot].tag_ids[0], sizeof(g_client_stored_data[slot].tag_ids));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
        clib__copy_memory(client->username, &g_client_stored_data[slot].username[0], clib__utf8_string_length(client->username), USERNAME_MAX_LENGTH - 1); // remember the last-seen username for the admin ui

        g_client_stored_data[slot].tag_id_count = 0;
        for (t = 0; t < tag_count && t < MAX_TAGS_FOR_SINGLE_CLIENT; t++)
        {
            g_client_stored_data[slot].tag_ids[g_client_stored_data[slot].tag_id_count] = (uint64)client->tag_ids[t];
            g_client_stored_data[slot].tag_id_count++;
        }

        DBG_IDENTITIES log_info("%s %llu %s %lld %s %llu %s %s %s", "snapshot_identities: stored client_id", client->client_id, "into slot", slot, "with", (uint64)g_client_stored_data[slot].tag_id_count, "tag(s), hash [", identity_hash, "] \n");
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief immediately mirrors ONE client's current tags into the in-memory identity store, so a
 *        reconnecting client gets its tags back within the same server run WITHOUT waiting for an
 *        admin "save server settings" (that save only adds disk persistence on top).
 *
 *        called right after any tag change on a client. if the client has tags they overwrite (or
 *        create) its store entry; if it now has zero tags its entry is cleared, so a fully-untagged
 *        identity is forgotten - unless the slot still holds an avatar or an alias, in which case only
 *        the tags are cleared. does nothing when identities are disabled.
 *
 * @param client_t* client -> the client whose tags just changed
 *
 * @note the caller must hold the clients lock (client->tag_ids is read). the store itself is guarded
 *       by g_client_stored_data_mutex, taken here.
 *
 * @return void
 */
void identity_store__sync_client_identity_in_store(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 tag_count = 0;
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

    tag_count = cvector_size(client->tag_ids);

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

    if (tag_count == 0)
    {
        // the identity no longer wears any tag: drop its entry so it is not restored later - UNLESS it
        // still holds an avatar or an alias, in which case keep the slot and just clear the tags
        if (slot != -1)
        {
            if (g_client_stored_data[slot].base64_avatar[0] != 0 || g_client_stored_data[slot].alias[0] != 0)
            {
                g_client_stored_data[slot].tag_id_count = 0;
                clib__null_memory(&g_client_stored_data[slot].tag_ids[0], sizeof(g_client_stored_data[slot].tag_ids));
                DBG_IDENTITIES log_info("%s %llu %s %lld %s", "sync_identity: client_id", client->client_id, "has no tags left but keeps an avatar -> cleared tags on slot", slot, "\n");
            }
            else
            {
                clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
                DBG_IDENTITIES log_info("%s %llu %s %lld %s", "sync_identity: client_id", client->client_id, "has no tags left -> cleared store slot", slot, "\n");
            }
        }
        pthread_mutex_unlock(&g_client_stored_data_mutex);
        return;
    }

    // has tags but no entry yet: take the first free slot
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
    clib__null_memory(&g_client_stored_data[slot].tag_ids[0], sizeof(g_client_stored_data[slot].tag_ids));
    clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    clib__copy_memory(client->username, &g_client_stored_data[slot].username[0], clib__utf8_string_length(client->username), USERNAME_MAX_LENGTH - 1); // remember the last-seen username for the admin ui

    g_client_stored_data[slot].tag_id_count = 0;
    for (t = 0; t < tag_count && t < MAX_TAGS_FOR_SINGLE_CLIENT; t++)
    {
        g_client_stored_data[slot].tag_ids[g_client_stored_data[slot].tag_id_count] = (uint64)client->tag_ids[t];
        g_client_stored_data[slot].tag_id_count++;
    }

    DBG_IDENTITIES log_info("%s %llu %s %lld %s %llu %s %s %s", "sync_identity: client_id", client->client_id, "mirrored into slot", slot, "with", (uint64)g_client_stored_data[slot].tag_id_count, "tag(s), hash [", identity_hash, "] \n");

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief removes one stored identity (by its base64(SHA256(public_key)) hash) from the in-memory
 *        store. the on-disk copy is untouched until the next "save server settings" re-serializes the
 *        store without it. stripping the tags off a currently-connected holder is the caller's job.
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
 *        neither tags nor an alias). the on-disk copy follows on the next settings save.
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
    else if (g_client_stored_data[slot].tag_id_count == 0 && g_client_stored_data[slot].alias[0] == 0)
    {
        // cleared the avatar and the slot holds neither tags nor an alias -> drop it entirely
        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief compares two strings ignoring ascii case. used for alias clash detection so "Ada" cannot be
 *        registered next to "ada" and impersonate it in a contact list. non-ascii bytes compare as-is.
 *
 * @param char* string1 -> the first null-terminated string
 * @param char* string2 -> the second null-terminated string
 *
 * @return boole -> TRUE when the strings match case-insensitively, FALSE otherwise or when either is NULL
 */
static boole _identity_store_internal__are_strings_equal_ignoring_ascii_case(char* string1, char* string2)
{
    uint64 i = 0;
    char character1 = 0;
    char character2 = 0;

    if (string1 == NULL_POINTER || string2 == NULL_POINTER)
    {
        return FALSE;
    }

    while (string1[i] != 0 && string2[i] != 0)
    {
        character1 = string1[i];
        character2 = string2[i];

        if (character1 >= 'A' && character1 <= 'Z')
        {
            character1 = (char)(character1 + 32);
        }
        if (character2 >= 'A' && character2 <= 'Z')
        {
            character2 = (char)(character2 + 32);
        }

        if (character1 != character2)
        {
            return FALSE;
        }

        i++;
    }

    return (boole)(string1[i] == 0 && string2[i] == 0);
}

/**
 * @brief tells whether an alias is already registered on a DIFFERENT identity. the alias is the only
 *        handle the stored-clients list exposes and the key clients pair offline entries by, so two
 *        identities must never share one. an identity re-registering its own alias is not a clash, and
 *        an empty alias (clearing) never clashes.
 *
 * @param char* alias -> the alias about to be registered
 * @param char* identity_hash -> the identity it would be registered on
 *
 * @return boole TRUE when another identity already holds this alias
 *
 * @note guarded by g_client_stored_data_mutex, taken here (a leaf lock).
 */
boole identity_store__is_alias_taken_by_another_identity(char* alias, char* identity_hash)
{
    uint64 i = 0;
    boole taken = FALSE;

    if (alias == NULL_POINTER || alias[0] == 0 || identity_hash == NULL_POINTER)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0 || g_client_stored_data[i].alias[0] == 0)
        {
            continue;
        }

        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            continue; // the identity's own entry - renaming itself is allowed
        }

        if (_identity_store_internal__are_strings_equal_ignoring_ascii_case(&g_client_stored_data[i].alias[0], alias) == TRUE)
        {
            taken = TRUE;
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return taken;
}

/**
 * @brief stamps "this identity was connected just now" onto its stored entry, as a unix-seconds
 *        timestamp.
 *
 *        only REGISTERED identities are tracked - an entry exists and carries an alias - so a random
 *        guest passing through is never timestamped. also requires identities to be enabled and the
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
            if (g_client_stored_data[i].alias[0] != 0) // registered = carries an admin-granted alias
            {
                g_client_stored_data[i].last_seen_unix_seconds = (int64)time(NULL_POINTER);
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief remembers an identity's RAW rsa public key, so peers can encrypt to it while its owner is
 *        offline. only does anything while is_offline_messaging_allowed is on - with the feature off the
 *        server keeps nothing but the hash, exactly as before. creates the slot if the identity has
 *        none yet (a first-time visitor with no tags/avatar/alias).
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
 * @brief forgets the stored raw public key of one identity. called when an admin takes an alias away:
 *        the identity stops being registered, so it can no longer be written to while offline and
 *        there is no reason to keep the key that would allow it.
 *
 * @param char* identity_hash -> hash identifying the stored identity
 *
 * @return void
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
 * @brief resolves a REGISTERED alias to the identity hash that currently owns it. aliases are unique
 *        across identities (the set-alias handler enforces that), so this is a 1:1 lookup.
 *
 * @param char* alias -> the alias to look up (compared ignoring ascii case, like the uniqueness check)
 * @param char* out_identity_hash -> receives the hash
 * @param uint64 out_buffer_size -> size of out_identity_hash
 *
 * @return boole TRUE when an identity carries this alias
 */
boole identity_store__get_identity_hash_by_alias(char* alias, char* out_identity_hash, uint64 out_buffer_size)
{
    uint64 i = 0;
    boole found = FALSE;

    if (alias == NULL_POINTER || alias[0] == 0 || out_identity_hash == NULL_POINTER || out_buffer_size == 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0 || g_client_stored_data[i].alias[0] == 0)
        {
            continue;
        }

        if (_identity_store_internal__are_strings_equal_ignoring_ascii_case(&g_client_stored_data[i].alias[0], alias) == TRUE)
        {
            clib__null_memory(out_identity_hash, out_buffer_size);
            clib__copy_memory(&g_client_stored_data[i].public_key[0], out_identity_hash, clib__utf8_string_length(&g_client_stored_data[i].public_key[0]), out_buffer_size - 1);
            found = TRUE;
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found;
}

/**
 * @brief TRUE when this identity is REGISTERED on the server, meaning an admin gave it an alias.
 *
 * @param char* identity_hash -> base64 hash of the public key
 *
 * @return boole
 */
boole identity_store__is_identity_registered_by_hash(char* identity_hash)
{
    uint64 i = 0;
    boole registered = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            registered = (boole)(g_client_stored_data[i].alias[0] != 0);
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return registered;
}

/**
 * @brief stores one text message for an identity that is not connected right now. the payload is
 *        copied onto the heap and is never inspected: it arrived encrypted with the RECIPIENT's
 *        public key, so the server cannot read it and neither can anybody else.
 *
 * @param char* recipient_identity_hash -> who it is for (the identity, not the alias)
 * @param char* sender_identity_hash -> who sent it
 * @param char* sender_alias -> the sender's registered name, for display on delivery
 * @param char* base64_encrypted_message -> opaque ciphertext
 *
 * @return boole TRUE when the message was queued
 *
 * @note takes g_offline_messages_mutex (leaf lock)
 */
boole identity_store__queue_offline_message(char* recipient_identity_hash, char* sender_identity_hash, char* sender_alias, char* base64_encrypted_message)
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

    if (sender_alias != NULL_POINTER)
    {
        clib__copy_memory(sender_alias, &g_offline_messages[free_slot].sender_alias[0], clib__utf8_string_length(sender_alias), USERNAME_MAX_LENGTH - 1);
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
 * @brief stores (or replaces) an identity's admin-registered alias in the in-memory store, keyed by
 *        its base64(SHA256(public_key)) hash. creates an alias-only slot if the identity has no entry
 *        yet. pass an empty/NULL alias to clear it (and drop the slot if it then holds neither tags
 *        nor an avatar). the on-disk copy follows on the next settings save.
 *
 * @param char* identity_hash -> base64(SHA256(public_key)) of the identity, the store's key
 * @param char* alias -> the alias to store, or NULL/empty to clear it
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return void
 */
void identity_store__set_identity_alias_by_hash(char* identity_hash, char* alias)
{
    uint64 i = 0;
    int64 slot = -1;
    boole clearing = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    clearing = (boole)(alias == NULL_POINTER || alias[0] == 0);

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
            DBG_IDENTITIES log_info("%s", "set_identity_alias: identity store FULL, cannot store alias \n");
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return;
        }

        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    }

    clib__null_memory(&g_client_stored_data[slot].alias[0], USERNAME_MAX_LENGTH);

    if (clearing == FALSE)
    {
        clib__copy_memory(alias, &g_client_stored_data[slot].alias[0], clib__utf8_string_length(alias), USERNAME_MAX_LENGTH - 1);
    }
    else if (g_client_stored_data[slot].tag_id_count == 0 && g_client_stored_data[slot].base64_avatar[0] == 0)
    {
        // cleared the alias and the slot holds neither tags nor an avatar -> drop it entirely
        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
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
 * @brief adds or removes one tag id on a stored identity (by hash), for admin tag management of
 *        identities that may be offline.
 *
 *        removing an identity's last tag clears the whole entry, matching how the store never holds
 *        tagless identities. disk persistence still waits for the next "save server settings".
 *        stripping/adding the tag on a currently-connected holder is the caller's job.
 *
 * @param char* identity_hash -> base64 hash of the identity to modify
 * @param uint64 tag_id -> the tag id to add or remove
 * @param boole add -> TRUE to add the tag, FALSE to remove it
 *
 * @return boole -> TRUE if the identity existed (whether or not the tag set actually changed)
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock).
 */
boole identity_store__modify_identity_tag_in_store(char* identity_hash, uint64 tag_id, boole add)
{
    uint64 i = 0;
    uint64 t = 0;
    boole found_identity = FALSE;
    boole has_tag = FALSE;
    uint64 tag_index = 0;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
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

        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            if (g_client_stored_data[i].tag_ids[t] == tag_id)
            {
                has_tag = TRUE;
                tag_index = t;
                break;
            }
        }

        if (add == TRUE)
        {
            if (has_tag == FALSE && g_client_stored_data[i].tag_id_count < MAX_TAGS_FOR_SINGLE_CLIENT)
            {
                g_client_stored_data[i].tag_ids[g_client_stored_data[i].tag_id_count] = tag_id;
                g_client_stored_data[i].tag_id_count++;
            }
        }
        else
        {
            if (has_tag == TRUE)
            {
                for (t = tag_index; (t + 1) < g_client_stored_data[i].tag_id_count; t++)
                {
                    g_client_stored_data[i].tag_ids[t] = g_client_stored_data[i].tag_ids[t + 1];
                }
                g_client_stored_data[i].tag_id_count--;

                // an identity with no tags left is dropped entirely, like everywhere else
                if (g_client_stored_data[i].tag_id_count == 0)
                {
                    clib__null_memory(&g_client_stored_data[i], sizeof(client_stored_data_t));
                }
            }
        }

        DBG_IDENTITIES log_info("%s %s %s %llu %s %s %s", "modify_identity_tag:", (add == TRUE) ? "added" : "removed", "tag", tag_id, (add == TRUE) ? "to" : "from", "identity [", identity_hash);

        break;
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found_identity;
}
