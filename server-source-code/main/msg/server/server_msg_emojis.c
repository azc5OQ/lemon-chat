// This file sends the server emoji names and requested images to connected clients.
#include "server_msg.h"
#include "../../core/emojis.h"

/**
 * @brief Sends emoji names without loading their images into the client's chat.
 * @param receiver Connected client receiving the list with the clients lock held.
 */
void server_msg__emojis__send_list(client_t* receiver)
{
    cJSON* message = NULL_POINTER;
    cJSON* root = NULL_POINTER;
    cJSON* emojis = emojis__catalogue();

    if (emojis == NULL_POINTER)
    {
        server_msg__emojis__send_error(receiver, "load_server_emojis", "There is not enough memory to send the emoji list.", NULL_POINTER);
        return;
    }
    root = server_msg__delivery__new_message("server_emoji_list", &message);
    if (root == NULL_POINTER || !cJSON_AddItemToObject(message, "emojis", emojis))
    {
        cJSON_Delete(emojis);
        cJSON_Delete(root);
        return;
    }
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Sends one requested image or reports that it no longer exists.
 * @param receiver Connected client receiving the image with the clients lock held.
 * @param id Stable emoji identifier.
 */
void server_msg__emojis__send_image(client_t* receiver, cstring id)
{
    cJSON* image = emojis__image(id);
    cJSON* root = NULL_POINTER;

    if (image == NULL_POINTER)
    {
        server_msg__emojis__send_error(receiver, "load_server_emoji", "This emoji is no longer available.", id);
        return;
    }
    root = cJSON_CreateObject();
    if (root == NULL_POINTER
        || cJSON_AddStringToObject(image, "type", "server_emoji_image") == NULL_POINTER
        || !cJSON_AddItemToObject(root, "message", image))
    {
        cJSON_Delete(image);
        cJSON_Delete(root);
        return;
    }
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Announces an added image by name so clients can request it when needed.
 * @param id New stable identifier.
 * @param name Emoji name with the clients lock held.
 */
void server_msg__emojis__send_added(cstring id, cstring name)
{
    cJSON* message = NULL_POINTER;
    cJSON* root = server_msg__delivery__new_message("server_emoji_added", &message);

    cJSON_AddStringToObject(message, "id", id);
    cJSON_AddStringToObject(message, "name", name);
    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);
    cJSON_Delete(root);
}

/**
 * @brief Removes a deleted emoji from connected clients' choices.
 * @param id Deleted identifier with the clients lock held.
 */
void server_msg__emojis__send_deleted(cstring id)
{
    cJSON* message = NULL_POINTER;
    cJSON* root = server_msg__delivery__new_message("server_emoji_deleted", &message);

    cJSON_AddStringToObject(message, "emoji_id", id);
    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);
    cJSON_Delete(root);
}

/**
 * @brief Reports an emoji request failure so the client can stop waiting.
 * @param receiver Client that sent the request with the clients lock held.
 * @param operation Request message type.
 * @param reason Explanation shown by the client.
 * @param id Optional emoji identifier.
 */
void server_msg__emojis__send_error(client_t* receiver, cstring operation, cstring reason, cstring id)
{
    cJSON* message = NULL_POINTER;
    cJSON* root = server_msg__delivery__new_message("server_emoji_error", &message);

    cJSON_AddStringToObject(message, "operation", operation);
    cJSON_AddStringToObject(message, "reason", reason);
    if (emojis__is_id_valid(id))
    {
        cJSON_AddStringToObject(message, "emoji_id", id);
    }
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}
