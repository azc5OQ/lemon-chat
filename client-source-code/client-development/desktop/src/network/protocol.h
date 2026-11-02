#ifndef LEMON_DESKTOP_PROTOCOL_H
#define LEMON_DESKTOP_PROTOCOL_H

#include "clib.h"


#define DESKTOP_MESSAGE_LIMIT (1024U * 1024U)
struct cJSON;
struct cJSON* desktop_protocol__parse_json(const char* bytes, uint64 size, uint64 limit, uint64 maximum_depth);

/* Validates a complete, unencrypted Lemon Chat JSON envelope. */
boole desktop_protocol__message_type(const char* bytes, uint64 size, char* type, uint64 capacity);

#endif
