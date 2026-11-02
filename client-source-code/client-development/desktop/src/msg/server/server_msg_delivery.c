#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief reads an optional JSON string without returning a null pointer.
 * @param const cJSON* object -> borrowed JSON object; may be NULL
 * @param const char* key -> case-sensitive JSON member name
 * @return const char* -> borrowed JSON string, or an empty literal
 */
const char* server_msg__read_string(const cJSON* object, const char* key)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

/**
 * @brief reads a nonnegative integer representable exactly by the existing JSON protocol.
 * @param const cJSON* object -> borrowed JSON object; may be NULL
 * @param const char* key -> case-sensitive JSON member name
 * @return int64 -> exact nonnegative integer, or -1 for missing/invalid data
 */
int64 server_msg__read_number(const cJSON* object, const char* key)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !(value->valuedouble >= 0 && value->valuedouble <= 9007199254740991.0)
        || value->valuedouble != (double)(int64)value->valuedouble)
    {
        return -1;
    }
    return (int64)value->valuedouble;
}

