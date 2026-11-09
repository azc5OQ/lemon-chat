#include "clib.h"
#include "session/session_internal.h"
#include "client_msg.h"

/**
 * @brief sends the existing public identity and ephemeral mix before authentication.
 * @param desktop_session_t* session -> worker-owned connection
 * @param const char* public_mix -> decimal ephemeral public mix
 * @return boole -> 1 when the complete request was accepted by transport
 */
boole client_msg__connection__send_public_key(desktop_session_t *session, const char *public_mix)
{
    cJSON *message = client_msg__delivery__new_message("public_key_info");
    boole complete = message && cJSON_AddStringToObject(message, "value", session->public_key) && cJSON_AddStringToObject(message, "verification_string", "welcome") && cJSON_AddStringToObject(message, "dh_public_mix", public_mix) && cJSON_AddNumberToObject(message, "crypto_version", 2);
    return client_msg__delivery__send(session, message, complete);
}

/**
 * @brief sends the decrypted identity challenge using the newly established metadata keys.
 * @param desktop_session_t* session -> worker-owned connection
 * @param const char* challenge -> verified RSA challenge plaintext
 * @return boole -> 1 when accepted by transport
 */
boole client_msg__connection__send_challenge_response(desktop_session_t *session, const char *challenge)
{
    cJSON *message = client_msg__delivery__new_message("public_key_challenge_response");
    boole complete = message && cJSON_AddStringToObject(message, "value", challenge) && cJSON_AddBoolToObject(message, "fast_reconnect", 0) && cJSON_AddStringToObject(message, "chosen_username", session->connection.username);
    return client_msg__delivery__send(session, message, complete);
}

/**
 * @brief maintains the authenticated TCP session without running networking on the GUI thread.
 * @param desktop_session_t* session -> worker-owned connection
 * @return boole -> 1 when accepted by transport
 */
boole client_msg__connection__send_keepalive(desktop_session_t *session)
{
    return client_msg__delivery__send(session, client_msg__delivery__new_message("client_connection_check"), 1);
}
