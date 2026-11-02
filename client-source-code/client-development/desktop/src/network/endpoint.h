#ifndef LEMON_DESKTOP_ENDPOINT_H
#define LEMON_DESKTOP_ENDPOINT_H

#include "clib.h"

typedef struct desktop_endpoint_t
{
    char host[256];
    char path[2048];
    ushort port;
    boole secure;
} desktop_endpoint_t;

/* ws/wss only. No embedded credentials, fragments or whitespace. */
boole desktop_endpoint__parse(const char* url, desktop_endpoint_t* endpoint);
boole desktop_endpoint__compose(const char* host, const char* port, const char* path, char* output, uint64 capacity);

#endif
