#ifndef LEMON_DESKTOP_PROTOCOL_H
#define LEMON_DESKTOP_PROTOCOL_H

#include "clib.h"


#define DESKTOP_MESSAGE_LIMIT (1024U * 1024U)
/* Validates a complete, unencrypted Lemon Chat JSON envelope. */
boole desktop_protocol__message_type(const char* bytes, uint64 size, char* type, uint64 capacity);

#endif
