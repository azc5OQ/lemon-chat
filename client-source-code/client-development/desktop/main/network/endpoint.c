// Bounded parsing of TCP WebSocket addresses used by the native client.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include "network/endpoint.h"
#include "clib.h"

/**
 * @brief validates separate hostname and port fields and builds the initial secure WebSocket URL.
 * @param const char* host -> domain name, IPv4 address or bare IPv6 address; never a URL
 * @param const char* port -> decimal port from 1 through 65535
 * @param const char* path -> WebSocket request path starting with slash
 * @param char* output -> caller-owned URL buffer, empty on failure
 * @param uint64 capacity -> output capacity including NUL
 * @return boole -> TRUE for a valid domain or IP address and explicit port
 */
boole desktop_endpoint__compose(const char *host, const char *port, const char *path, char *output, uint64 capacity)
{
    desktop_endpoint_t endpoint = { 0 };
    ubyte address[16] = { 0 };
    const char *cursor = NULL_POINTER;
    int64 length = 0;
    int64 label = 0;
    int64 number = 0;
    int64 written = 0;
    boole ipv6 = FALSE;
    boole numeric = TRUE;
    boole valid = FALSE;

    // check the output
    if (!output || !capacity)
    {
        return FALSE;
    }
    clib__null_memory(output, capacity);

    // check host, port, path
    if (!host || !port || !path || !host[0] || !port[0] || path[0] != '/')
    {
        return FALSE;
    }

    // check the host length
    length = clib__strlen(host);
    if (length > 253)
    {
        return FALSE;
    }

    // check the host format
    ipv6 = clib__strchr(host, ':') != NULL;
    if (ipv6)
    {
        if (inet_pton(AF_INET6, host, address) != 1)
        {
            return FALSE;
        }
    }
    else
    {
        // check every host character
        for (cursor = host; *cursor; cursor++)
        {
            // check the label end
            if (*cursor == '.')
            {
                if (!label || cursor[-1] == '-')
                {
                    return FALSE;
                }
                label = 0;
                continue;
            }

            // allow letters, digits, hyphens
            if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= '0' && *cursor <= '9') || (*cursor == '-' && label)))
            {
                return FALSE;
            }

            // update the numeric flag
            numeric &= *cursor >= '0' && *cursor <= '9';

            // limit the label length
            if (++label > 63)
            {
                return FALSE;
            }
        }

        // check end and IPv4
        if (host[length - 1] == '-' || (numeric && inet_pton(AF_INET, host, address) != 1))
        {
            return FALSE;
        }
    }

    // read the port number
    for (cursor = port; *cursor; cursor++)
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return FALSE;
        }
        number = number * 10 + *cursor - '0';
        if (number > 65535)
        {
            return FALSE;
        }
    }

    // refuse port zero
    if (!number)
    {
        return FALSE;
    }

    // build the URL
    written = clib__string_format(output, capacity, ipv6 ? "wss://[%s]:%lld%s" : "wss://%s:%lld%s", host, number, path);
    valid = written > 0 && (uint64)written < capacity && desktop_endpoint__parse(output, &endpoint);
    if (!valid)
    {
        clib__null_memory(output, capacity);
    }

    // clear the buffers
    clib__null_memory(address, sizeof(address));
    clib__null_memory(&endpoint, sizeof(endpoint));
    return valid;
}

/**
 * @brief validates a TCP WebSocket address and separates its host, port and path.
 *
 * @param const char* url -> ws:// or wss:// endpoint
 * @param desktop_endpoint_t* endpoint -> destination for the validated endpoint
 *
 * @return boole -> 1 on success; 0 for a malformed address or a field that exceeds its buffer
 */
/* ws/wss only. No embedded credentials, fragments or whitespace. */
boole desktop_endpoint__parse(const char *url, desktop_endpoint_t *endpoint)
{
    const char *cursor = NULL_POINTER;
    const char *host = NULL_POINTER;
    const char *end = NULL_POINTER;
    int64 port = 0;

    port = 0;

    // check arguments
    if (!url || !endpoint)
    {
        return 0;
    }

    // read the scheme
    clib__null_memory(endpoint, sizeof(*endpoint));
    endpoint->secure = clib__strncmp(url, "wss://", 6) == 0;
    if (!endpoint->secure && clib__strncmp(url, "ws://", 5) != 0)
    {
        return 0;
    }

    // refuse forbidden characters
    for (cursor = url; *cursor; cursor++)
    {
        if ((ubyte)*cursor <= 32 || *cursor == '#' || *cursor == '\\' || (ubyte)*cursor == 127)
        {
            return 0;
        }
    }

    // find the host text
    cursor = url + (endpoint->secure ? 6 : 5);
    host = cursor;
    if (*cursor == '[')
    {
        // find the closing bracket
        host = ++cursor;
        end = clib__strchr(cursor, ']');
        if (!end || end == host || !clib__strchr(host, ':'))
        {
            return 0;
        }
        cursor = end + 1;
    }
    else
    {
        // find the host end
        while (*cursor && *cursor != ':' && *cursor != '/' && *cursor != '?')
        {
            cursor++;
        }
        end = cursor;
    }

    // copy the host
    if (end == host || !clib__string_copy_n(endpoint->host, sizeof(endpoint->host), host, end - host))
    {
        return 0;
    }

    // refuse forbidden host characters
    if (clib__strchr(endpoint->host, '@') || clib__strchr(endpoint->host, '[') || clib__strchr(endpoint->host, ']'))
    {
        return 0;
    }

    // read the port
    endpoint->port = endpoint->secure ? 443 : 80;
    if (*cursor == ':')
    {
        // check the first digit
        cursor++;
        if (!clib__isdigit(*cursor))
        {
            return 0;
        }

        // read the port digits
        while (clib__isdigit(*cursor))
        {
            port = port * 10 + (int64)(*cursor++ - '0');
            if (port > 65535)
            {
                return 0;
            }
        }

        // refuse port zero
        if (!port)
        {
            return 0;
        }

        // store the port
        endpoint->port = (unsigned short)port;
    }

    // check the path start
    if (*cursor != 0 && *cursor != '/' && *cursor != '?')
    {
        return 0;
    }

    // add a missing slash
    if (*cursor != '/')
    {
        clib__string_copy(endpoint->path, sizeof(endpoint->path), "/");
        return clib__string_append(endpoint->path, sizeof(endpoint->path), cursor);
    }

    // copy the path
    return clib__string_copy(endpoint->path, sizeof(endpoint->path), cursor);
}
