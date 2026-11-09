#ifndef LEMON_DESKTOP_PROTOCOL_H
#define LEMON_DESKTOP_PROTOCOL_H

#include "clib.h"

#define DESKTOP_MESSAGE_LIMIT (1024U * 1024U)
#define DESKTOP_SERVER_CLOSE_KICKED 4001
#define DESKTOP_SERVER_CLOSE_BANNED 4002
struct cJSON;
struct cJSON *desktop_protocol__parse_json(const char *bytes, uint64 size, uint64 limit, uint64 maximum_depth);

boole desktop_protocol__message_type(const char *bytes, uint64 size, char *type, uint64 capacity);

#endif
