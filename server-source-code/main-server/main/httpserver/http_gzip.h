#ifndef HTTP_GZIP_H
#define HTTP_GZIP_H

#include "../definitions.h"

#include <stddef.h>

typedef enum http_encoding
{
    HTTP_ENCODING_UNACCEPTABLE = -1,
    HTTP_ENCODING_IDENTITY = 0,
    HTTP_ENCODING_GZIP = 1
} http_encoding_t;

http_encoding_t http_gzip__choose_encoding(const char *request, boole *identity_allowed);

ubyte *http_gzip__compress(const void *data, uint64 size, uint64 *compressed_size);

#endif
