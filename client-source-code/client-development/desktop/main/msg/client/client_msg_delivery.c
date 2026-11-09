#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief attempts one bounded attachment write without waiting on a full socket queue.
 * @param desktop_session_t* session -> sole protocol sender
 * @param cJSON* message -> consumed inner message on every path
 * @return boole -> TRUE when queued; FALSE leaves the transfer available for a later retry
 */
boole client_msg__delivery__try_send(desktop_session_t *session, cJSON *message)
{
    cJSON *root = cJSON_CreateObject();
    char *json = NULL;
    char *wire = NULL;
    boole sent = FALSE;
    if (root && message && cJSON_AddItemToObject(root, "message", message))
    {
        json = cJSON_PrintUnformatted(root);
        wire = json ? crypto_session__encrypt(session->crypto, json) : NULL;
        sent = wire && desktop_transport__send(session->transport, wire, clib__strlen(wire));
    }
    else
    {
        crypto_session__free_json(message);
    }
    crypto_session__free_json(root);
    crypto_session__free_text(json);
    crypto_session__free_text(wire);
    return sent;
}

/**
 * @brief creates the owned inner message object.
 * @param const char* type -> new control or protocol message type
 * @return cJSON* -> owned inner message, or NULL on allocation failure
 */
cJSON *client_msg__delivery__new_message(const char *type)
{
    cJSON *message = cJSON_CreateObject();
    if (message && !cJSON_AddStringToObject(message, "type", type))
    {
        crypto_session__free_json(message);
        message = NULL;
    }
    return message;
}

/**
 * @brief consumes a complete message, encrypts it and sends it from the sole sending worker.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param cJSON* message -> owned outgoing message; ownership is consumed even on failure
 * @param boole complete -> nonzero when all required outgoing fields were allocated
 * @return boole -> 1 when accepted by transport; consumes message on every path
 */
boole client_msg__delivery__send(desktop_session_t *session, cJSON *message, boole complete)
{
    cJSON *root = cJSON_CreateObject();
    char *json = NULL;
    char *wire = NULL;
    uint64 deadline = desktop_system__milliseconds() + 2000;
    boole sent = 0;

    if (!complete || !root || !message || !cJSON_AddItemToObject(root, "message", message))
    {
        crypto_session__free_json(message);
    }
    else
    {
        json = cJSON_PrintUnformatted(root);
        wire = json ? crypto_session__encrypt(session->crypto, json) : NULL;
    }
    while (wire && !desktop_session__stopping(session) && desktop_system__milliseconds() < deadline)
    {
        sent = desktop_transport__send(session->transport, wire, clib__strlen(wire));
        if (sent)
        {
            break;
        }
        desktop_system__sleep(5);
    }
    crypto_session__free_text(wire);
    crypto_session__free_text(json);
    crypto_session__free_json(root);
    if (!sent && !desktop_session__stopping(session))
    {
        desktop_session__detail(session, "Could not send a complete protocol message. Disconnected.", 1);
    }
    return sent;
}
