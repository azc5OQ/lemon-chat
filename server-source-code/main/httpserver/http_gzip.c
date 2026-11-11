#include "http_gzip.h"
#include "../../third-party/miniz-3.1.2/miniz.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static boole _http_gzip_internal__equal(const char *begin, const char *end, const char *expected)
{
    ubyte value = 0;

    // compare lowercase letters
    while (begin < end && *expected != 0)
    {
        value = (ubyte)*begin++;
        if (value >= 'A' && value <= 'Z')
        {
            value += 'a' - 'A';
        }
        if (value != (ubyte)*expected++)
        {
            return FALSE;
        }
    }
    return begin == end && *expected == 0;
}

// HTTP quality values are 0..1 with at most three decimal places.
// Invalid values disable that coding rather than accidentally enabling it.
static int64 _http_gzip_internal__quality(const char *begin, const char *end)
{
    int64 quality = 0;
    int64 place = 100;

    // check first digit
    if (begin == end || (*begin != '0' && *begin != '1'))
    {
        return 0;
    }

    // read the first digit
    quality = (*begin++ - '0') * 1000;
    if (begin == end)
    {
        return quality;
    }

    // check decimal point
    if (*begin++ != '.')
    {
        return 0;
    }

    // read decimal digits
    while (begin < end)
    {
        if (*begin < '0' || *begin > '9' || place == 0 || (quality >= 1000 && *begin != '0'))
        {
            return 0;
        }
        quality += (*begin++ - '0') * place;
        place /= 10;
    }
    return quality;
}

// Parse complete request headers. Explicit q=0 exclusions override wildcards.
http_encoding_t http_gzip__choose_encoding(const char *request, boole *identity_allowed)
{
    const char *end = NULL_POINTER;
    const char *colon = NULL_POINTER;
    const char *item = NULL_POINTER;
    const char *next = NULL_POINTER;
    const char *item_end = NULL_POINTER;
    const char *parameter = NULL_POINTER;
    const char *name_end = NULL_POINTER;
    int64 quality = 0;
    int64 *selected = NULL_POINTER;
    int64 gzip = -1;
    int64 identity = -1;
    int64 wildcard = -1;
    const char *line = strstr(request, "\r\n");

    // read every header line
    while (line != NULL && line[2] != '\r' && line[2] != 0)
    {
        // find line end
        line += 2;
        end = strstr(line, "\r\n");
        if (end == NULL)
        {
            break;
        }

        // find Accept-Encoding header
        colon = memchr(line, ':', (uint64)(end - line));
        if (colon != NULL && _http_gzip_internal__equal(line, colon, "accept-encoding"))
        {
            // read each list item
            item = colon + 1;
            while (item < end)
            {
                // find item end
                next = memchr(item, ',', (uint64)(end - item));
                item_end = next == NULL ? end : next;
                quality = 1000;
                selected = NULL;

                // remove surrounding spaces
                while (item < item_end && (*item == ' ' || *item == '\t'))
                {
                    item++;
                }
                while (item_end > item && (item_end[-1] == ' ' || item_end[-1] == '\t'))
                {
                    item_end--;
                }

                // split name and parameter
                parameter = memchr(item, ';', (uint64)(item_end - item));
                name_end = parameter == NULL ? item_end : parameter;
                while (name_end > item && (name_end[-1] == ' ' || name_end[-1] == '\t'))
                {
                    name_end--;
                }

                // read quality value
                if (parameter != NULL)
                {
                    parameter++;
                    while (parameter < item_end && (*parameter == ' ' || *parameter == '\t'))
                    {
                        parameter++;
                    }
                    if (item_end - parameter < 2 || (parameter[0] != 'q' && parameter[0] != 'Q') || parameter[1] != '=')
                    {
                        quality = 0;
                    }
                    else
                    {
                        quality = _http_gzip_internal__quality(parameter + 2, item_end);
                    }
                }

                // match encoding name
                if (_http_gzip_internal__equal(item, name_end, "gzip") || _http_gzip_internal__equal(item, name_end, "x-gzip"))
                {
                    selected = &gzip;
                }
                else if (_http_gzip_internal__equal(item, name_end, "identity"))
                {
                    selected = &identity;
                }
                else if (_http_gzip_internal__equal(item, name_end, "*"))
                {
                    selected = &wildcard;
                }

                // for contradictory duplicates, keep the stricter preference
                if (selected != NULL && (*selected < 0 || quality < *selected))
                {
                    *selected = quality;
                }

                // go to next item
                item = next == NULL ? end : next + 1;
            }
        }

        // go to next line
        line = end;
    }

    // allow or refuse identity
    *identity_allowed = identity > 0 || (identity < 0 && wildcard != 0);

    // use wildcard for gzip
    if (gzip < 0)
    {
        gzip = wildcard;
    }

    // choose the encoding
    if (gzip > 0 && (identity <= gzip || !*identity_allowed))
    {
        return HTTP_ENCODING_GZIP;
    }
    return *identity_allowed ? HTTP_ENCODING_IDENTITY : HTTP_ENCODING_UNACCEPTABLE;
}

// Compress one complete response body. Caller owns the returned malloc buffer.
ubyte *http_gzip__compress(const void *data, uint64 size, uint64 *compressed_size)
{
    uint64 i = 0;
    static const ubyte header[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255 };
    size_t deflated_size = 0; // miniz writes this through a size_t pointer
    ubyte *deflated = NULL_POINTER;
    ubyte *result = NULL_POINTER;
    uint64 crc = 0;

    // clear the result size
    *compressed_size = 0;

    // check input size
    if (size > SIZE_MAX)
    {
        return NULL;
    }

    // compress the data
    deflated = tdefl_compress_mem_to_heap(data, size, &deflated_size, TDEFL_DEFAULT_MAX_PROBES);
    if (deflated == NULL)
    {
        return NULL;
    }

    // check result size
    if (deflated_size > SIZE_MAX - 18)
    {
        free(deflated);
        return NULL;
    }

    // enlarge the buffer
    result = realloc(deflated, deflated_size + 18);
    if (result == NULL)
    {
        free(deflated);
        return NULL;
    }

    // write gzip header
    memmove(result + 10, result, deflated_size);
    memcpy(result, header, sizeof(header));

    // calculate the checksum
    crc = (uint64)mz_crc32(MZ_CRC32_INIT, data, size);

    // write low 32 bits
    for (i = 0; i < 4; i++)
    {
        result[10 + deflated_size + i] = (ubyte)(crc >> (i * 8));
        result[14 + deflated_size + i] = (ubyte)(size >> (i * 8));
    }

    // store result size
    *compressed_size = deflated_size + 18;
    return result;
}
