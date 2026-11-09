#ifndef LEMON_DESKTOP_CHAT_FORMAT_H
#define LEMON_DESKTOP_CHAT_FORMAT_H

#include "clib.h"

struct cJSON;

typedef struct desktop_chat_format_t
{
    char font[224];
    char color[8];
    int64 size;
} desktop_chat_format_t;

void desktop_chat_format__default(desktop_chat_format_t *format);
boole desktop_chat_format__valid(const desktop_chat_format_t *format);
void desktop_chat_format__read(desktop_chat_format_t *format, const struct cJSON *object);
boole desktop_chat_format__write(const desktop_chat_format_t *format, struct cJSON *object);

#endif
