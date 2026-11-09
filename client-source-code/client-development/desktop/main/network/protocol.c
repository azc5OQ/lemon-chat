// Validates received JSON envelopes before exposing their message type to the UI.
#include "network/protocol.h"
#include "clib.h"
#include "cJSON.h"

/**
 * @brief parses one bounded JSON value without accepting hidden string suffixes or trailing data.
 * @param const char* bytes -> exactly size readable bytes; no terminator is required
 * @param uint64 size -> JSON byte count, excluding any terminator
 * @param uint64 limit -> caller's existing message-size limit
 * @param uint64 maximum_depth -> caller's existing nesting limit
 * @return struct cJSON* -> owned tree, or NULL for invalid or excessive input
 */
struct cJSON *desktop_protocol__parse_json(const char *bytes, uint64 size, uint64 limit, uint64 maximum_depth)
{
    cJSON *root = NULL_POINTER;
    const char *end = NULL_POINTER;
    uint64 index = 0;
    uint64 depth = 0;
    boole quoted = FALSE;
    boole escaped = FALSE;

    if (!bytes || !size || size > limit || limit > 40ULL * 1024 * 1024 || !maximum_depth || maximum_depth > CJSON_NESTING_LIMIT)
    {
        return NULL;
    }
    for (index = 0; index < size; index++)
    {
        if (!bytes[index] || ((ubyte)bytes[index] < 32 && (quoted || (bytes[index] != ' ' && bytes[index] != '\t' && bytes[index] != '\r' && bytes[index] != '\n'))))
        {
            return NULL;
        }
        if (quoted)
        {
            if (escaped)
            {
                if (size - index >= 5 && clib__strncmp(bytes + index, "u0000", 5) == 0)
                {
                    return NULL;
                }
                escaped = FALSE;
            }
            else if (bytes[index] == '\\')
            {
                escaped = TRUE;
            }
            else if (bytes[index] == '"')
            {
                quoted = FALSE;
            }
        }
        else if (bytes[index] == '"')
        {
            quoted = TRUE;
        }
        else if (bytes[index] == '{' || bytes[index] == '[')
        {
            if (++depth > maximum_depth)
            {
                return NULL;
            }
        }
        else if (bytes[index] == '}' || bytes[index] == ']')
        {
            if (!depth)
            {
                return NULL;
            }
            depth--;
        }
    }
    if (quoted || depth)
    {
        return NULL;
    }
    root = cJSON_ParseWithLengthOpts(bytes, size, &end, FALSE);
    if (!root)
    {
        return NULL;
    }
    while (end < bytes + size && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
    {
        end++;
    }
    if (end != bytes + size)
    {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

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
/* Validates a complete, unencrypted Lemon Chat JSON envelope. */
boole desktop_protocol__message_type(const char *bytes, uint64 size, char *type, uint64 capacity)
{
    boole valid = 0;
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    cJSON *field = NULL_POINTER;

    valid = 0;
    if (type && capacity)
    {
        clib__null_memory(type, capacity);
    }
    if (!bytes || !size || size > DESKTOP_MESSAGE_LIMIT || !type || !capacity || clib__memchr(bytes, 0, size))
    {
        return 0;
    }
    root = desktop_protocol__parse_json(bytes, size, DESKTOP_MESSAGE_LIMIT, CJSON_NESTING_LIMIT);
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
    return valid;
}
