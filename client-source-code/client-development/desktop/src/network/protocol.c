#include "platform/memory.h"
// Validates received JSON envelopes before exposing their message type to the UI.
#include "network/protocol.h"
#include "clib.h"
#include "cJSON.h"
#include <stdlib.h>

/**
 * @brief extracts a bounded message type from a complete JSON envelope.
 *
 * @param const char* bytes -> message bytes
 * @param uint64 size -> message length in bytes
 * @param char* type -> destination for the parsed message type
 * @param uint64 capacity -> destination capacity including its terminator
 *
 * @return boole -> 1 when the type was copied; 0 for invalid data or an insufficient destination
 */
boole desktop_protocol__message_type(const char* bytes, uint64 size, char* type, uint64 capacity)
{
    boole valid = 0;
    char* terminated = NULL;
    cJSON* root = NULL;
    cJSON* message = NULL;
    cJSON* field = NULL;

    valid = 0;
    if (type && capacity)
    {
        clib__null_memory(type, capacity);
    }
    if (!bytes || !size || size > DESKTOP_MESSAGE_LIMIT || !type || !capacity || clib__memchr(bytes, 0, size))
    {
        return 0;
    }
    terminated = calloc(1, size + 1);
    if (!terminated)
    {
        return 0;
    }
    clib__string_copy_n(terminated, size + 1, bytes, size);
    root = cJSON_ParseWithLengthOpts(terminated, size + 1, NULL, 1);
    if (cJSON_IsObject(root))
    {
        message = cJSON_GetObjectItemCaseSensitive(root, "message");
        field = cJSON_GetObjectItemCaseSensitive(message, "type");
        if (cJSON_IsObject(message) && cJSON_IsString(field) && field->valuestring[0])
        {
            valid = clib__string_copy(type, capacity, field->valuestring);
        }
    }
    cJSON_Delete(root);
    desktop_memory__erase(terminated, size + 1);
    free(terminated);
    return valid;
}
