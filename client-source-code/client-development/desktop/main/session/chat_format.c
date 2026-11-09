#include "session/chat_format.h"
#include "cJSON.h"

/** @brief Selects readable desktop text within the browser's supported size range. */
void desktop_chat_format__default(desktop_chat_format_t *format)
{
    clib__null_memory(format, sizeof(*format));
    clib__string_copy(format->font, sizeof(format->font), "custom-font-usage-default");
    // Empty color inherits the recipient's theme, including a light theme.
    format->size = 14;
}

/** @brief Accepts a bounded font token, opaque RGB color and the browser's 12-30px size range. */
boole desktop_chat_format__valid(const desktop_chat_format_t *format)
{
    int64 length = 0;
    int64 index = 0;
    ubyte value = 0;
    if (!format || format->size < 12 || format->size > 30 || (format->color[0] && (clib__string_length(format->color, sizeof(format->color)) != 7 || format->color[0] != '#')))
    {
        return FALSE;
    }
    length = clib__string_length(format->font, sizeof(format->font));
    if (length <= 17 || clib__strncmp(format->font, "custom-font-usage-", 17) != 0)
    {
        return FALSE;
    }
    for (index = 0; index < length; index++)
    {
        value = (ubyte)format->font[index];
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '-' || value == '_'))
        {
            return FALSE;
        }
    }
    for (index = 1; format->color[0] && index < 7; index++)
    {
        value = (ubyte)format->color[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F')))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/** @brief Reads validated formatting without treating remote font names or colors as markup. */
void desktop_chat_format__read(desktop_chat_format_t *format, const cJSON *object)
{
    desktop_chat_format_t candidate;
    const cJSON *field = NULL;
    desktop_chat_format__default(format);
    candidate = *format;
    field = cJSON_GetObjectItemCaseSensitive(object, "font");
    if (cJSON_IsString(field) && clib__string_copy(candidate.font, sizeof(candidate.font), field->valuestring) && desktop_chat_format__valid(&candidate))
    {
        *format = candidate;
    }
    candidate = *format;
    field = cJSON_GetObjectItemCaseSensitive(object, "font_color");
    if (cJSON_IsString(field) && clib__string_copy(candidate.color, sizeof(candidate.color), field->valuestring) && desktop_chat_format__valid(&candidate))
    {
        *format = candidate;
    }
    field = cJSON_GetObjectItemCaseSensitive(object, "font_size");
    if (cJSON_IsNumber(field) && field->valuedouble >= 12 && field->valuedouble <= 30 && field->valuedouble == (double)field->valueint)
    {
        format->size = field->valueint;
    }
}

/** @brief Adds the same formatting fields used by the browser to encrypted message content. */
boole desktop_chat_format__write(const desktop_chat_format_t *format, cJSON *object)
{
    desktop_chat_format_t fallback;
    if (!desktop_chat_format__valid(format))
    {
        desktop_chat_format__default(&fallback);
        format = &fallback;
    }
    return object && cJSON_AddStringToObject(object, "font", format->font) && cJSON_AddStringToObject(object, "font_color", format->color) && cJSON_AddNumberToObject(object, "font_size", (double)format->size);
}
