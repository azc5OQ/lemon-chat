// This file checks login messages, establishes the encrypted connection and answers keepalive requests.
#include "client_msg.h"
#include <string.h>
#include "../../core/channel.h"
#include "../../core/client_session.h"
#include "../../core/client_login.h"
#include "../../core/identity_store.h"
#include "../../core/administration.h"
#include "../../core/login_admission.h"
#include "../../metadata_crypto.h"
#include "../server/server_msg.h"
#include "../../core/server_logs.h"
#include "../../core/video_channel.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/zhicheng/base64.h"
#include "../../dh_primes.h"
#include "../../../third-party/libtom/libtommath/tommath.h"
#include "../../ip_tools.h"

typedef struct pending_login_t
{
    uint64 client_id;
    ws_cli_conn_t* socket;
} pending_login_t;

// declarations
static void _client_msg_connection_internal__get_public_key_and_verification_string_and_dh_public_mix(cJSON* json_root, char** out_public_key, char** out_verification_string, char** out_dh_mix);
static boole _client_msg_connection_internal__is_public_key_info_message_valid(cJSON* json_root, uint64 client_id);
static int _client_msg_connection_internal__login_connection_is_valid(void* argument);
static boole _client_msg_connection_internal__is_public_key_challenge_response_valid(cJSON* json_root, uint64 client_id);
static char* _client_msg_connection_internal__get_challenge_string(cJSON* json_root);
static boole _client_msg_connection_internal__is_username_free_at_login(cstring candidate_username, uint64 client_index);
static boole _client_msg_connection_internal__try_apply_chosen_username(cJSON* json_root, uint64 client_index);

/**
 * @brief This function processes connection check message from client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @attention this function uses write lock on clients_global_rwlock_guard
 *
 * @return void
 */
void client_msg__connection__process_connection_check(cJSON* json_root, uint64 sender_client_id)
{
    clib__write_lock(&g_clients_global_rwlock_guard);

    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        g_clients_array[sender_client_id].timestamp_last_maintain_connection_message_received = util__get_timestamp_ms();
        server_msg__connection__send_connection_check_response_to_single_client(&g_clients_array[sender_client_id]);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief extracts the client's public key, verification string and dh public mix from the json into the provided output pointers
 *
 * @param cJSON* json_root -> the parsed client request
 * @param char** out_public_key -> receives a pointer to the client's public key string
 * @param char** out_verification_string -> receives a pointer to the verification string
 * @param char** out_dh_mix -> receives a pointer to the client's dh public mix string
 *
 * @return void
 *
 * @attention this function assumes json_root is already verified , has correct data, if not this function will cause crash of entire server
 */
static void _client_msg_connection_internal__get_public_key_and_verification_string_and_dh_public_mix(cJSON* json_root, char** out_public_key, char** out_verification_string, char** out_dh_mix)
{
    cJSON* public_key_json = 0;
    cJSON* verification_string_json = 0;
    cJSON* json_message_object = 0;
    cJSON* dh_public_mix_json = 0;

    // "message" object is fetched from json_root, again
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    public_key_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    verification_string_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
    dh_public_mix_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "dh_public_mix");

    *out_verification_string = verification_string_json->valuestring;
    *out_public_key = public_key_json->valuestring;
    *out_dh_mix = dh_public_mix_json->valuestring;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention this function is static, only used within msg/client/client_msg_connection.c
 *
 * @return boole
 */
static boole _client_msg_connection_internal__is_public_key_info_message_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_message_type = 0;
    cJSON* json_message_value = 0;
    cJSON* verification_string = 0;
    cJSON* json_message_object = 0;
    size_t length;
    size_t maximum_length;
    size_t digit;

    boole status = FALSE;

    // "message" object is fetched from json_root, again
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        // no object is not present under key "message", not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    if (cJSON_IsObject(json_message_object) == FALSE)
    {
        // message key exists but it does not store object in it, not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_message_object) == false\n");
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
    if (cJSON_IsString(json_message_type) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_message_type) \n");
        return FALSE;
    }

    if (json_message_type->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_message_type->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_message_type->valuestring) \n");
        return FALSE;
    }

    status = clib__is_string_equal(json_message_type->valuestring, "public_key_info");

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, "clib__is_string_equal(json_message_type->valuestring, \"public_key_info\") \n");
        return FALSE;
    }

    // type is verified, continue validating received json
    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    if (cJSON_IsString(json_message_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type != string \n");
        return FALSE;
    }

    if (json_message_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_value->valuestring == NULL \n");
        return FALSE;
    }

    // the public key is base64 of the modulus: 344 chars at 2048 bits, up to 1368 at 8192.
    // this used to demand exactly 344, which made every size except 2048 unusable - including
    // the larger sizes minimum_rsa_key_bits exists to let an admin require. only lengths no rsa
    // key could have are rejected here; the exact bit length is measured further down
    {
        uint64 public_key_string_length = clib__utf8_string_length(json_message_value->valuestring);

        if (public_key_string_length < PUBLIC_KEY_STRING_MIN_LENGTH || public_key_string_length > PUBLIC_KEY_STRING_MAX_LENGTH || (public_key_string_length % 4) != 0)
        {
            DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " public key string length is not a valid base64 modulus \n");
            return FALSE;
        }
    }

    // it's verified that json contains client's public key, continue json validation
    verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
    if (cJSON_IsString(verification_string) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type != string \n");
        return FALSE;
    }

    if (verification_string->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " verification_string->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(verification_string->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(verification_string->valuestring) \n");
        return FALSE;
    }

    // it's verified that json contains verification_string, continue json validation
    verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "dh_public_mix");
    if (cJSON_IsString(verification_string) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " dh_public_mix != string \n");
        return FALSE;
    }

    if (verification_string->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " dh_public_mix->valuestring == NULL \n");
        return FALSE;
    }

    // A DH public value is a decimal number smaller than the known prime.
    // Check its size and digits before parsing it as a large integer.
    {
        length = strlen(verification_string->valuestring);
        maximum_length = sizeof(DH_MODULUS_STR) - 1;

        if (length == 0 || length > maximum_length)
        {
            return FALSE;
        }
        for (digit = 0; digit < length; digit++)
        {
            if (verification_string->valuestring[digit] < '0' || verification_string->valuestring[digit] > '9')
            {
                return FALSE;
            }
        }
    }

    // it's verified that json contains dh_public_mix, json appears to be valid
    return TRUE;
}

/**
 * @brief Checks that the original connection still occupies its client slot and
 * is still waiting to begin the key exchange.
 *
 * @param void* argument -> pending_login_t containing the client id and original socket
 *
 * @attention This function takes the clients read lock. The login waiting code
 * must release its own mutex before calling it.
 *
 * @return int -> 1 when this connection may begin its login; 0 otherwise
 */
static int _client_msg_connection_internal__login_connection_is_valid(void* argument)
{
    pending_login_t* pending = argument;
    int valid;

    clib__read_lock(&g_clients_global_rwlock_guard);
    valid = pending->client_id < (uint64)g_server_settings.max_client_count && g_clients_array[pending->client_id].is_existing == TRUE && g_clients_array[pending->client_id].p_ws_connection == pending->socket && g_clients_array[pending->client_id].is_authenticated == FALSE && g_clients_array[pending->client_id].is_public_key_challenge_sent == FALSE && ws_get_state(pending->socket) == WS_STATE_OPEN && !base__is_server_ram_budget_reached();
    clib__unlock(&g_clients_global_rwlock_guard);
    return valid;
}

/**
 * @brief Handles the client's first login message. After checking its format,
 * waits for a free login slot, checks the RSA key, calculates the shared secret
 * and encrypts a random challenge for the client to answer.
 *
 * @param cJSON* json_root -> parsed login message; the caller keeps it alive until this function returns
 * @param uint64 sender_client_id -> the client slot assigned to this connection
 * @param ws_cli_conn_t* socket -> the original socket that received this login message
 *
 * @attention The expensive key calculations run without holding the clients lock.
 * Before storing their results or closing a failed login, this function checks
 * that the original socket still belongs to the same client slot. The later
 * challenge response proves that the client owns its private key, including when
 * it resumes an earlier session.
 *
 * @return void
 */
void client_msg__connection__process_public_key_info(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket)
{
    uint64 started_ms = util__get_timestamp_ms();
    uint64 crypto_started_ms = 0;
    uint64 crypto_finished_ms = 0;
    uint64 client_key_bits;
    uint64 media_send_counter = 0;
    boole is_too_weak;
    boole is_unusable;
    boole status = FALSE;
    mp_err mp_status = 0;
    size_t mp_written = 0;
    boole is_bignums_initialized = FALSE;
    boole is_failed = TRUE; // only a run that reaches the end turns this off; a failure closes the connection
    boole is_slot_changed = FALSE; // the slot belongs to another connection now: nothing to close, nothing to store
    boole has_login_slot = FALSE;
    boole is_minimum_rsa_key_bits_announced = FALSE;
    uint64 minimum_rsa_key_bits = 0;
    pending_login_t pending = { sender_client_id, socket };

    // data extracted from JSON
    char* public_key = 0;
    char* verification_string = 0;
    char* dh_received_public_mix_from_client = 0;

    // DH bignums
    mp_int bignum_modulus;
    mp_int bignum_server_exponent;
    mp_int bignum_client_public_mix;
    mp_int bignum_shared_secret;
    mp_int bignum_generator;
    mp_int bignum_server_public_mix;
    mp_int bignum_modulus_minus_one;

    // the results of the arithmetic, computed without a lock and stored into the slot under the second lock
    char* shared_secret_string = 0;
    char* dh_public_mix_from_server_string_for_client = 0;
    char* challenge_string = 0;
    char* challenge_value_for_client = 0;

    char exponent_bits[DH_EXPONENT_BITS + 1];
    unsigned char exponent_random_bytes[DH_EXPONENT_BITS / 8];
    uint64 i = 0;

    // ──────────────────────────────────────────────────────────────────
    // STEP 1: Basic validation (no lock)
    // ──────────────────────────────────────────────────────────────────
    status = _client_msg_connection_internal__is_public_key_info_message_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: invalid message structure", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    _client_msg_connection_internal__get_public_key_and_verification_string_and_dh_public_mix(json_root, &public_key, &verification_string, &dh_received_public_mix_from_client);
    if (clib__is_string_equal(verification_string, "welcome") == FALSE)
    {
        goto _label_client_msg__process_public_key_info_end;
    }

    // Remember that this connection is waiting before releasing the client lock.
    // Its queue wait has a separate deadline from the time allowed for login.
    clib__write_lock(&g_clients_global_rwlock_guard);

    if (sender_client_id >= (uint64)g_server_settings.max_client_count || g_clients_array[sender_client_id].is_existing == FALSE || g_clients_array[sender_client_id].p_ws_connection != socket || g_clients_array[sender_client_id].is_authenticated == TRUE || g_clients_array[sender_client_id].is_public_key_challenge_sent == TRUE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    if (started_ms < g_clients_array[sender_client_id].timestamp_last_action + TIMESTAMP_LAST_ACTION_COOLDOWN_MS)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    g_clients_array[sender_client_id].timestamp_last_action = started_ms;
    g_clients_array[sender_client_id].is_waiting_for_login_slot = TRUE;
    clib__unlock(&g_clients_global_rwlock_guard);

    // Waiting readers sleep without holding the client lock. Their connection
    // is checked again while waiting, after admission, and before storing keys.
    has_login_slot = login_admission__acquire(_client_msg_connection_internal__login_connection_is_valid, &pending, 60000);
    if (has_login_slot == FALSE)
    {
        goto _label_client_msg__process_public_key_info_end;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_clients_array[sender_client_id].is_existing == FALSE || g_clients_array[sender_client_id].p_ws_connection != socket || g_clients_array[sender_client_id].is_authenticated == TRUE || g_clients_array[sender_client_id].is_public_key_challenge_sent == TRUE || ws_get_state(socket) != WS_STATE_OPEN)
    {
        is_slot_changed = TRUE;
        clib__unlock(&g_clients_global_rwlock_guard);
        goto _label_client_msg__process_public_key_info_end;
    }
    g_clients_array[sender_client_id].is_waiting_for_login_slot = FALSE;
    g_clients_array[sender_client_id].timestamp_connected = util__get_timestamp_ms();
    minimum_rsa_key_bits = (uint64)g_server_settings.minimum_rsa_key_bits;
    is_minimum_rsa_key_bits_announced = g_server_settings.is_minimum_rsa_key_bits_announced;
    clib__unlock(&g_clients_global_rwlock_guard);

    crypto_started_ms = util__get_timestamp_ms();

    // Parsing and checking the RSA key can be expensive too. Do that after
    // admission, without stopping other clients from using the client lock.
    // A modulus one bit below the requested size is still accepted.
    {
        client_key_bits = client_login__get_public_key_bit_length(public_key);
        is_too_weak = (client_key_bits < minimum_rsa_key_bits - 1) ? TRUE : FALSE;
        is_unusable = (client_key_bits == 0 || client_key_bits > 8192) ? TRUE : FALSE;

        if (is_too_weak == TRUE || is_unusable == TRUE)
        {
            DBG_AUTHENTICATION log_info("[auth] client %llu: public key is %llu bits, minimum is %llu, dropping", sender_client_id, client_key_bits, minimum_rsa_key_bits);

            // only a genuinely too-weak key earns the notice, and only if the admin opted in.
            // a malformed or oversized key is never answered: nothing legitimate produces one,
            // so there is nobody to help, only a prober to inform
            if (is_too_weak == TRUE && is_unusable == FALSE && is_minimum_rsa_key_bits_announced == TRUE)
            {
                clib__read_lock(&g_clients_global_rwlock_guard);
                if (g_clients_array[sender_client_id].is_existing == TRUE && g_clients_array[sender_client_id].p_ws_connection == socket)
                {
                    server_msg__connection__send_rsa_key_too_weak_to_single_client(&g_clients_array[sender_client_id]);
                }
                clib__unlock(&g_clients_global_rwlock_guard);
            }

            goto _label_client_msg__process_public_key_info_end;
        }
    }

    // ──────────────────────────────────────────────────────────────────
    // STEP 3: Diffie-Hellman key exchange, without a lock
    //
    // Both client and server know:
    // p = dh_known_modulus_str  (shared prime, defined at top of file)
    // g = 2                    (generator)
    //
    // Client sent:   A = g^a mod p   (dh_received_public_mix_from_client)
    // Server picks:  b              (random secret exponent)
    // Server computes:
    // shared_secret = A^b mod p   (same value as g^(a*b) mod p)
    // B = g^b mod p               (sent back to client)
    // Client will compute:
    // shared_secret = B^a mod p   (same value as g^(a*b) mod p)
    // ──────────────────────────────────────────────────────────────────
    mp_status = mp_init_multi(&bignum_modulus, &bignum_server_exponent, &bignum_client_public_mix, &bignum_shared_secret, &bignum_generator, &bignum_server_public_mix, &bignum_modulus_minus_one, (mp_int*)NULL_POINTER);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to initialize DH bignums", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    is_bignums_initialized = TRUE;

    // 3a. load the shared prime modulus
    mp_status = mp_read_radix(&bignum_modulus, DH_MODULUS_STR, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to parse DH modulus", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 3b. generate the server's secret exponent (DH_EXPONENT_BITS random bits, sized to the modulus)
    // first bit forced to 1 to guarantee the full length, the rest from a cryptographically secure
    // RNG. rand() must never be used here: it is a predictable PRNG seeded from the clock, so an
    // exponent built from it could be reconstructed by an attacker, breaking the key exchange.
    if (util__fill_secure_random_bytes(exponent_random_bytes, sizeof(exponent_random_bytes)) == FALSE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to generate DH exponent randomness", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    clib__null_memory(exponent_bits, sizeof(exponent_bits));
    exponent_bits[0] = '1';
    for (i = 1; i < DH_EXPONENT_BITS; i++)
    {
        exponent_bits[i] = (((exponent_random_bytes[i / 8] >> (7 - (i % 8))) & 1) == 0) ? '0' : '1';
    }
    exponent_bits[DH_EXPONENT_BITS] = '\0';
    mp_status = mp_read_radix(&bignum_server_exponent, exponent_bits, 2);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to create DH exponent", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    DBG_AUTHENTICATION log_info("[auth] client %llu: generated DH exponent", sender_client_id);

    // 3c. parse client's public mix: A = g^a mod p (received as decimal string)
    mp_status = mp_read_radix(&bignum_client_public_mix, (char*)dh_received_public_mix_from_client, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to parse client's DH public mix", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 3c-validate: reject a degenerate client public mix before exponentiating. A in {0, 1, p-1} (or outside
    // [2, p-2]) forces a known / tiny shared secret; for a safe prime the only dangerous small-order elements
    // are 1 and p-1, so requiring 2 <= A <= p-2 is sufficient.
    mp_status = mp_sub_d(&bignum_modulus, 1uL, &bignum_modulus_minus_one);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to compute p-1 for DH validation", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    if (mp_cmp_d(&bignum_client_public_mix, 1uL) != MP_GT || mp_cmp(&bignum_client_public_mix, &bignum_modulus_minus_one) != MP_LT)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: rejected degenerate DH public mix (not in [2, p-2])", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 3d. compute shared_secret = A^b mod p, as a decimal string
    mp_status = mp_exptmod(&bignum_client_public_mix, &bignum_server_exponent, &bignum_modulus, &bignum_shared_secret);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: mp_exptmod failed for shared secret", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    shared_secret_string = (char*)memorymanager__allocate(SHARED_SECRET_LENGTH, MEMALLOC_DHPROCESS);
    if (shared_secret_string == NULL_POINTER)
    {
        goto _label_client_msg__process_public_key_info_end;
    }
    clib__null_memory(shared_secret_string, SHARED_SECRET_LENGTH);
    mp_status = mp_to_radix(&bignum_shared_secret, shared_secret_string, SHARED_SECRET_LENGTH, &mp_written, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to convert shared secret to string", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 3e. compute server's public mix: B = g^b mod p, as a decimal string
    mp_set_i64(&bignum_generator, 2);
    mp_status = mp_exptmod(&bignum_generator, &bignum_server_exponent, &bignum_modulus, &bignum_server_public_mix);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: mp_exptmod failed for server public mix", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    dh_public_mix_from_server_string_for_client = (char*)memorymanager__allocate(SHARED_SECRET_LENGTH, MEMALLOC_DHPROCESS);
    if (dh_public_mix_from_server_string_for_client == NULL_POINTER)
    {
        goto _label_client_msg__process_public_key_info_end;
    }
    mp_status = mp_to_radix(&bignum_server_public_mix, dh_public_mix_from_server_string_for_client, SHARED_SECRET_LENGTH, &mp_written, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to convert server public mix to string", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    DBG_AUTHENTICATION log_info("[auth] client %llu: server public mix computed", sender_client_id);

    // ──────────────────────────────────────────────────────────────────
    // STEP 4: the RSA challenge, without a lock: a random string, encrypted with the client's public
    // key. only the real owner of the private key can decrypt it
    // ──────────────────────────────────────────────────────────────────
    challenge_string = (char*)memorymanager__allocate(128, MEMALLOC_TYPE_CHALLENGE);
    if (challenge_string == NULL_POINTER || util__fill_block_of_data_with_ascii_characters(challenge_string, CHALLENGE_STRING_SIZE) == FALSE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to generate RSA challenge randomness", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }
    DBG_AUTHENTICATION log_info("[auth] client %llu: generated %d-byte challenge", sender_client_id, CHALLENGE_STRING_SIZE);
    challenge_value_for_client = client_login__encrypt_string_with_public_key(public_key, (unsigned char*)challenge_string, (uint64)clib__utf8_string_length(challenge_string));
    if (challenge_value_for_client == NULL_POINTER || challenge_value_for_client[0] == 0)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: the challenge could not be encrypted with the client's key", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    // ──────────────────────────────────────────────────────────────────
    // STEP 5: the second, short write lock: the slot must still belong to the same connection, then
    // the shared secret and the challenge are stored and the challenge is sent
    // ──────────────────────────────────────────────────────────────────
    crypto_finished_ms = util__get_timestamp_ms();
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_clients_array[sender_client_id].is_existing == FALSE || g_clients_array[sender_client_id].p_ws_connection != socket || g_clients_array[sender_client_id].is_authenticated == TRUE || g_clients_array[sender_client_id].is_public_key_challenge_sent == TRUE || ws_get_state(socket) != WS_STATE_OPEN)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: the slot changed hands during the key exchange, nothing stored", sender_client_id);
        is_slot_changed = TRUE;
        clib__unlock(&g_clients_global_rwlock_guard);
        goto _label_client_msg__process_public_key_info_end;
    }
    clib__copy_memory(public_key, g_clients_array[sender_client_id].public_key, clib__utf8_string_length(public_key), MAX_PUBLIC_KEY_LENGTH - 1);
    clib__null_memory(g_clients_array[sender_client_id].dh_shared_secret, SHARED_SECRET_LENGTH);
    clib__copy_memory(shared_secret_string, g_clients_array[sender_client_id].dh_shared_secret, clib__utf8_string_length(shared_secret_string), SHARED_SECRET_LENGTH - 1);
    g_clients_array[sender_client_id].is_dh_shared_secret_agreed_upon = TRUE;
    // Make the keys that this connection will use for metadata, audio and video.
    // Also choose a random starting packet number for media sent to this client.
    // If this connection later takes over the client's earlier session, those
    // keys and the packet number must move into that session together.
    if (!metadata_crypto__derive_session_keys(shared_secret_string, &g_clients_array[sender_client_id].session_keys) || !util__fill_secure_random_bytes((unsigned char*)&media_send_counter, sizeof(media_send_counter)))
    {
        metadata_crypto__clear_session_keys(&g_clients_array[sender_client_id].session_keys);
        clib__unlock(&g_clients_global_rwlock_guard);
        goto _label_client_msg__process_public_key_info_end;
    }
    atomic_init(&g_clients_array[sender_client_id].media_send_counter, media_send_counter);
    DBG_AUTHENTICATION log_info("[auth] client %llu: shared secret computed and stored", sender_client_id);

    // send server's DH public mix + encrypted challenge to client, keep the plaintext challenge for the answer
    server_msg__connection__send_public_key_challenge_to_single_client(&g_clients_array[sender_client_id], challenge_value_for_client, dh_public_mix_from_server_string_for_client);
    clib__copy_memory(challenge_string, &g_clients_array[sender_client_id].challenge_string[0], CHALLENGE_STRING_SIZE, 128);
    g_clients_array[sender_client_id].is_public_key_challenge_sent = TRUE;
    is_failed = FALSE;
    clib__unlock(&g_clients_global_rwlock_guard);
    DBG_AUTHENTICATION log_info("[auth] client %llu: challenge stored, waiting for response", sender_client_id);

_label_client_msg__process_public_key_info_end:
    if (crypto_finished_ms == 0)
    {
        crypto_finished_ms = util__get_timestamp_ms();
    }
    DBG_CONNECTION_PROGRESS log_info("[connection] key exchange client=%llu ready=%d total_ms=%llu crypto_ms=%llu", (unsigned long long)sender_client_id, (int)!is_failed, (unsigned long long)(util__get_timestamp_ms() - started_ms), (unsigned long long)(crypto_started_ms == 0 ? 0 : crypto_finished_ms - crypto_started_ms));
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (sender_client_id < (uint64)g_server_settings.max_client_count && g_clients_array[sender_client_id].is_existing == TRUE && g_clients_array[sender_client_id].p_ws_connection == socket)
    {
        g_clients_array[sender_client_id].is_waiting_for_login_slot = FALSE;
        if (is_failed == TRUE && is_slot_changed == FALSE)
        {
            client_session__close_websocket_connection(sender_client_id, FALSE);
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);
    if (shared_secret_string != NULL_POINTER)
    {
        clib__null_memory(shared_secret_string, SHARED_SECRET_LENGTH);
        memorymanager__free((nuint)shared_secret_string);
    }
    if (dh_public_mix_from_server_string_for_client != NULL_POINTER)
    {
        memorymanager__free((nuint)dh_public_mix_from_server_string_for_client);
    }
    if (challenge_string != NULL_POINTER)
    {
        memorymanager__free((nuint)challenge_string);
    }
    if (challenge_value_for_client != NULL_POINTER)
    {
        memorymanager__free((nuint)challenge_value_for_client);
    }
    if (is_bignums_initialized == TRUE)
    {
        mp_clear_multi(&bignum_modulus, &bignum_server_exponent, &bignum_client_public_mix, &bignum_shared_secret, &bignum_generator, &bignum_server_public_mix, &bignum_modulus_minus_one, (mp_int*)NULL_POINTER);
    }
    if (has_login_slot == TRUE)
    {
        login_admission__release();
    }
}

/**
 * @brief validates a public key challenge response: the message object and its value string are present
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention this function is static, only used within msg/client/client_msg_connection.c
 *
 * @return boole
 */
static boole _client_msg_connection_internal__is_public_key_challenge_response_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_message_value = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        // no object is not present under key "message", not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    if (cJSON_IsString(json_message_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type != string \n");
        return FALSE;
    }

    if (json_message_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_value->valuestring == NULL \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Helper function. gets challenge string from json.
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return char* returns  the challenge string
 *
 * @attention this function assumes json_root is already verified , has correct data, if not this function will cause crash of entire server
 */
static char* _client_msg_connection_internal__get_challenge_string(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* challenge_string = 0;
    char* result = 0;

    // "message" object is fetched from json_root, again
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    challenge_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    result = challenge_string->valuestring;

    return result;
}

/**
 * @brief tells whether a username is free for this client to take while it is logging in
 *
 * @param cstring candidate_username -> the name being tried
 * @param uint64 client_index -> the client that wants it
 *
 * @return boole -> TRUE when nobody else connected uses it and no other identity has it registered
 *
 * @note sessions carrying the SAME public key are skipped: those are this client's own earlier sessions,
 *       already closed by the identity takeover, so the name they still hold counts as free
 */
static boole _client_msg_connection_internal__is_username_free_at_login(cstring candidate_username, uint64 client_index)
{
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (i == client_index)
        {
            continue;
        }

        if (g_clients_array[client_index].public_key[0] != 0 && clib__is_string_equal(g_clients_array[i].public_key, g_clients_array[client_index].public_key) == TRUE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].username, candidate_username) == TRUE)
        {
            return FALSE;
        }
    }

    // registered names are reserved for their identity, same rule the rename path enforces
    if (g_clients_array[client_index].public_key[0] != 0)
    {
        char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
        util__hash_password_to_base64(g_clients_array[client_index].public_key, identity_hash, sizeof(identity_hash));

        if (identity_store__is_alias_taken_by_another_identity((char*)candidate_username, identity_hash) == TRUE)
        {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief Tries the requested login name and numbered variants without taking another client's or identity's name.
 * @param cJSON* json_root -> challenge response, optionally containing message.chosen_username
 * @param uint64 client_index -> client being authenticated
 * @return boole -> TRUE when a name was applied; FALSE to fall back to the normal default_nameN assignment
 * @note Names must fit within 50 bytes; an unavailable name does not reject the login.
 * @attention The caller holds the clients write lock.
 */
static boole _client_msg_connection_internal__try_apply_chosen_username(cJSON* json_root, uint64 client_index)
{
    cJSON* json_message_object = 0;
    cJSON* json_chosen_username = 0;
    char candidate_username[USERNAME_MAX_LENGTH];
    char suffix_buffer[16];
    int64 chosen_length = 0;
    uint64 i = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        return FALSE;
    }

    json_chosen_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "chosen_username");
    if (cJSON_IsString(json_chosen_username) == FALSE || json_chosen_username->valuestring == NULL_POINTER)
    {
        return FALSE;
    }

    chosen_length = clib__utf8_string_length_check_max_length(json_chosen_username->valuestring, 50);
    if (chosen_length <= 0)
    {
        return FALSE;
    }

    // the wanted name first, then the same name with 2, 3, ... appended, so somebody else holding
    // it costs a digit instead of the whole wish
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        clib__null_memory(candidate_username, USERNAME_MAX_LENGTH);
        clib__null_memory(suffix_buffer, sizeof(suffix_buffer));
        clib__copy_memory(json_chosen_username->valuestring, candidate_username, chosen_length, USERNAME_MAX_LENGTH);

        if (i > 0)
        {
            sprintf(suffix_buffer, "%llu", (unsigned long long)(i + 1));

            // a name that long has no room left for a number, so stop trying
            if ((chosen_length + (int64)clib__utf8_string_length(suffix_buffer)) > 50)
            {
                break;
            }

            clib__copy_memory(&suffix_buffer[0], &candidate_username[chosen_length], clib__utf8_string_length(suffix_buffer), USERNAME_MAX_LENGTH - chosen_length);
        }

        if (_client_msg_connection_internal__is_username_free_at_login(candidate_username, client_index) == TRUE)
        {
            clib__null_memory(g_clients_array[client_index].username, USERNAME_MAX_LENGTH);
            clib__copy_memory(candidate_username, g_clients_array[client_index].username, clib__utf8_string_length(candidate_username), USERNAME_MAX_LENGTH);
            DBG_AUTHENTICATION log_info("%s %llu %s %s %s", "client ", client_index, " connected with chosen username ", g_clients_array[client_index].username, "\n");
            return TRUE;
        }
    }

    DBG_AUTHENTICATION log_info("%s %s %s", "chosen username ", json_chosen_username->valuestring, " and its numbered variants are all taken - falling back to an assigned name \n");
    return FALSE;
}

/**
 * @brief processes public key challenge response from client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__connection__process_public_key_challenge_response(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    char* received_challenge_string = 0;
    channel_t* resumed_channel = NULL_POINTER;
    client_t* current_client = NULL_POINTER;
    cJSON* json_message_object = 0;
    cJSON* json_fast_reconnect = 0;
    boole is_fast_reconnect = FALSE; // the client asks to resume its still-open session
    boole is_resumed_session = FALSE; // granted: current_client is the adopted session, not a fresh join
    int64 adopted_index = -1;

    // status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    // if (status == FALSE)
    // {
    // DBG_CLIENT_MESSAGE log_info("%s", "client_msg__connection__process_public_key_challenge_response base__is_request_allowed_based_on_spam_protection == FALSE \n");
    // return;
    // }
    status = _client_msg_connection_internal__is_public_key_challenge_response_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        client_session__close_websocket_connection(sender_client_id, TRUE);
        DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__connection__process_public_key_challenge_response deleting client because challenge response not valid: client index ", sender_client_id, " \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    // client sends public key to server at the time of authentication
    // server generates random string, encrypts that string with the client's public key
    // server then verifies if the client really is the owner of public key by sending client a little challenge
    // "if the public key is really yours, client, please, decrypt and then send back this randomly generated string that I will send you. you will have no problem telling me what I sent you, if it's really your key"
    // something like that
    current_client = &g_clients_array[sender_client_id];
    if (current_client->is_public_key_challenge_sent == FALSE)
    {
        client_session__close_websocket_connection(sender_client_id, FALSE);
        DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__connection__process_public_key_challenge_response deleting client because !current_client->is_public_key_challenge_sent : client index ", sender_client_id, " \n");
        goto _label_client_msg__process_public_key_challenge_response_end;
    }

    // compare the key
    received_challenge_string = _client_msg_connection_internal__get_challenge_string(json_root);

    status = clib__is_string_equal(g_clients_array[sender_client_id].challenge_string, received_challenge_string);

    if (status == TRUE)
    {
        DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__connection__process_public_key_challenge_response challenge response string match : client index ", sender_client_id, " \n");

        current_client->channel_id = 0;
        current_client->has_pending_maintainer_reset_vote = FALSE; // fresh session - no vote can be pending
        current_client->is_admin = FALSE;
        current_client->is_authenticated = TRUE;
        current_client->timestamp_last_maintain_connection_message_received = util__get_timestamp_ms();

        // identity ban: the ip check at socket open cannot see the key yet, so a banned identity
        // arriving from a new ip is caught here - the challenge just proved the client really holds it
        if (server_admin__is_identity_banned(current_client->public_key) == TRUE)
        {
            log_info("%s %s %s", "join refused: banned identity (ip", current_client->ip_address, ") \n");
            server_logs__join_refused("banned identity", current_client->ip_address);
            client_session__close_websocket_connection(sender_client_id, FALSE);
            goto _label_client_msg__process_public_key_challenge_response_end;
        }

        if (g_server_settings.is_display_country_flags_active == TRUE)
        {
            ip_tools_load_iso_country_code(current_client->ip_address, current_client->country_iso_code);
        }

        // country blocking: the join is refused here, before the client gets a username or any
        // lists. evaluated per connection, so the same person from an allowed country gets in.
        // resolves the country itself when the flags feature (above) has not already done it
        if (g_server_settings.is_country_blocking_active == TRUE)
        {
            if (current_client->country_iso_code[0] == 0)
            {
                ip_tools_load_iso_country_code(current_client->ip_address, current_client->country_iso_code);
            }

            if (server_admin__is_country_blocked(current_client->country_iso_code) == TRUE)
            {
                // an abrupt close with no explanation, by design - exactly what a wrong key gets
                log_info("%s %s %s %s %s", "join refused: country", current_client->country_iso_code, "is blocked (ip", current_client->ip_address, ") \n");
                server_logs__join_refused_country(current_client->country_iso_code, current_client->ip_address);
                client_session__close_websocket_connection(sender_client_id, FALSE);
                goto _label_client_msg__process_public_key_challenge_response_end;
            }
        }

        // fast reconnect (settings tab): the challenge just proved the identity, so the client may adopt
        // its still-open session and keep id, channel, name and roles. asked for by a flag in this message
        json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
        json_fast_reconnect = cJSON_GetObjectItemCaseSensitive(json_message_object, "fast_reconnect");
        is_fast_reconnect = (boole)(cJSON_IsBool(json_fast_reconnect) && cJSON_IsTrue(json_fast_reconnect));

        if (is_fast_reconnect == TRUE && g_server_settings.is_fast_reconnect_allowed == TRUE)
        {
            adopted_index = client_session__adopt_socket_into_existing_session(sender_client_id, current_client->public_key);
        }

        if (adopted_index >= 0)
        {
            // from here on the handler works on the adopted session; the new socket's own entry is gone
            is_resumed_session = TRUE;
            sender_client_id = (uint64)adopted_index;
            current_client = &g_clients_array[sender_client_id];
        }

        // identity takeover (settings tab): any other session still holding the same key is torn down now
        // and this login goes on as an ordinary fresh one, which is what keeps the channel key handover
        // honest - a client only gets channel keys by joining for real. with takeover off, an identity
        // that is already online is refused instead, until that session ends or times out
        if (g_server_settings.is_identity_takeover_allowed == TRUE)
        {
            client_session__disconnect_other_clients_with_same_public_key(sender_client_id, current_client->public_key);
        }
        else if (is_resumed_session == FALSE && client_session__is_there_another_authenticated_client_with_same_public_key(sender_client_id, current_client->public_key) == TRUE)
        {
            log_info("%s %s %s", "join refused: this identity is already connected (ip", current_client->ip_address, ") \n");
            server_logs__join_refused("identity already connected", current_client->ip_address);
            client_session__close_websocket_connection(sender_client_id, FALSE);
            goto _label_client_msg__process_public_key_challenge_response_end;
        }

        // a resumed session was admitted and named when it first joined; both are checked for new joins only
        if (is_resumed_session == FALSE)
        {
            // the same-ip rule is checked here and not at socket open, because only now is it known whether
            // the session already on this ip was this very identity (its earlier session, dropped just above)
            if (g_server_settings.is_same_ip_address_allowed == FALSE)
            {
                if (client_session__is_there_another_authenticated_client_with_same_ip_address(sender_client_id, current_client->ip_address, current_client->public_key) == TRUE)
                {
                    log_info("%s %s %s", "join refused: another identity is already connected from ip", current_client->ip_address, "\n");
                    server_logs__join_refused("same ip already connected", current_client->ip_address);
                    client_session__close_websocket_connection(sender_client_id, FALSE);
                    goto _label_client_msg__process_public_key_challenge_response_end;
                }
            }

            // a connect-time chosen username is used instead of the assigned default_nameN when the
            // client sent one and it is free; a registered identity's admin-set name still wins,
            // because it takes precedence over the session name right below
            if (_client_msg_connection_internal__try_apply_chosen_username(json_root, sender_client_id) == FALSE)
            {
                status = client_session__assign_username_for_newly_joined_client(sender_client_id, g_server_settings.default_client_name);

                if (status == FALSE)
                {
                    client_session__close_websocket_connection(sender_client_id, FALSE);
                    DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__connection__process_public_key_challenge_response deleting client client_session__assign_username_for_newly_joined_client returned false : client index ", sender_client_id, " \n");
                    goto _label_client_msg__process_public_key_challenge_response_end;
                }
            }
        }

        // it's better when readlock is placed here instead of it being placed directly in server_msg__channel__send_list_to_single_client function
        clib__write_lock(&g_channels_global_rwlock_guard);
        clib__read_lock(&g_tags_global_rwlock_guard);
        clib__read_lock(&g_icons_global_rwlock_guard);

        status = base__is_client_valid(current_client->client_id);

        if (status == TRUE)
        {
            // a resumed session already carries its tags, avatar, alias and stored key: nothing to restore
            if (is_resumed_session == FALSE)
            {
                // restore this client's saved tags from the identity store (matched by public-key hash); the
                // admin tag re-grants admin. runs before the connect broadcast so every client sees the tags.
                // the tags read lock (held above) and clients write lock (held for this handler) cover it
                if (g_server_settings.is_identity_storage_enabled == TRUE)
                {
                    DBG_IDENTITIES log_info("%s %llu %s", "identities: enabled -> restoring tags for authenticated client_id", current_client->client_id, "\n");
                    identity_store__restore_identity_tags(current_client);
                }
                else
                {
                    DBG_IDENTITIES log_info("%s", "identities: DISABLED on this server (is_identity_storage_enabled=false) -> no tags will be restored on connect \n");
                }

                // restore this identity's persisted avatar into the live client so others can load it. gated on
                // is_avatar_upload_allowed only (avatars may be enabled without tag-identities)
                if (g_server_settings.is_avatar_upload_allowed == TRUE)
                {
                    identity_store__restore_identity_avatar(current_client);
                }

                // restore the admin-registered alias (display name) for this identity
                if (g_server_settings.is_identity_storage_enabled == TRUE && g_server_settings.is_alias_registration_allowed == TRUE)
                {
                    identity_store__restore_identity_alias(current_client);
                }

                // an identity the admin gave a registered name to is a REGISTERED user of this server. only
                // those may list the stored clients - otherwise any guest could join and harvest everyone's
                // name and avatar. avatars are self-service so they prove nothing; a registered name is
                // admin-granted
                current_client->is_registered = (boole)(current_client->alias[0] != 0);

                // the registered name IS the username: apply it over whatever this session connected with,
                // so a registered person always appears under their one admin-set name (and cannot keep an
                // old self-chosen username after being registered)
                // registered names are reserved (the rename path refuses them and registration refuses a
                // name in use), so this is normally free - but never overwrite into a duplicate if some
                // older state left the name occupied: the client keeps the deduplicated name assigned on join instead
                if (current_client->is_registered == TRUE && client_session__is_username_taken_by_another_client(current_client->alias, current_client->client_id) == FALSE)
                {
                    clib__null_memory(&current_client->username[0], USERNAME_MAX_LENGTH);
                    clib__copy_memory(current_client->alias, &current_client->username[0], clib__utf8_string_length(current_client->alias), USERNAME_MAX_LENGTH - 1);
                }

                // offline messages need peers to be able to encrypt to this identity while it is away,
                // which means the server must keep its RAW public key (only ever while the feature is on,
                // and only for REGISTERED identities - unregistered ones cannot be messaged offline)
                if (g_server_settings.is_offline_messaging_allowed == TRUE && current_client->is_registered == TRUE && current_client->public_key[0] != 0 && current_client->is_music_bot == FALSE)
                {
                    char offline_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
                    util__hash_password_to_base64(current_client->public_key, offline_identity_hash, sizeof(offline_identity_hash));
                    identity_store__store_identity_raw_public_key(offline_identity_hash, current_client->public_key);
                }
            }

            if (is_resumed_session == TRUE)
            {
                // announced before the lists, so the client applies them as a refresh, not a first build
                server_logs__fast_reconnect(current_client);
                server_msg__connection__send_fast_reconnect_ok_to_single_client(current_client);
            }
            else
            {
                // the join is complete and the username is final (deduplicated / replaced by the alias) here
                server_logs__client_joined(current_client);
            }

            server_msg__connection__send_authentication_status_to_single_client(current_client);
            server_msg__channel__send_list_to_single_client(current_client);
            server_msg__connection__send_client_list_to_single_client(current_client);
            server_msg__administration__send_icon_list_to_single_client(current_client);
            server_msg__administration__send_tag_list_to_single_client(current_client);
            server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(current_client, current_client->channel_id);
            // a resumed session never left, so the others get no connect broadcast
            if (is_resumed_session == FALSE)
            {
                server_msg__connection__send_client_connect_message_to_all_clients(current_client);

                // a new login starts in the root channel: a stream running there asks its streamer
                // whether the newcomer may watch. a resumed session never left, its consent survived
                video_channel__process_client_joined_channel(current_client);
            }

            // hand over anything that was said to this identity while it was away, then forget it.
            // after the client list, so the receiving client already knows who everybody is
            if (g_server_settings.is_offline_messaging_allowed == TRUE && current_client->is_registered == TRUE && current_client->public_key[0] != 0)
            {
                server_msg__chat__send_queued_offline_messages_to_single_client(current_client);
            }

            // A new login starts in root; the first person there becomes its maintainer.
            if (is_resumed_session == FALSE)
            {
                channel__welcome_client(current_client);
            }
            else if (current_client->is_idle == FALSE)
            {
                // The refreshed lists do not contain the maintainer id. Announce it again,
                // and ask the maintainer for fresh keys in case this client missed a set.
                resumed_channel = &g_channel_array[current_client->channel_id];
                server_msg__channel__send_maintainer_id_to_clients_in_same_channel(resumed_channel, resumed_channel->maintainer_id);
            }

            clib__unlock(&g_icons_global_rwlock_guard);
            clib__unlock(&g_tags_global_rwlock_guard);
            clib__unlock(&g_channels_global_rwlock_guard);
        }
    }
    else
    {
        DBG_AUTHENTICATION log_info("%s", " challenge string not equal \n");
        server_logs__join_refused("challenge response mismatch", current_client->ip_address);
        client_session__close_websocket_connection(sender_client_id, FALSE);
    }

_label_client_msg__process_public_key_challenge_response_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}
