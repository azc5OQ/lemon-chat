// This file helps provide URL previews when a client posts a website link in
// chat. The feature must be enabled in the server settings. The server uses
// this code to request the page and read the website's response, so the preview
// can show its title, description and picture below the chat message.
//
// This part prepares HTTP requests and reads the returned headers and body.
// It is based on the supplied winhttpc implementation. Windows connections
// use httpclient_windows.c; Linux and macOS use httpclient_posix.c. Downloaded
// pages are only read for their preview information; their scripts are not run.
//
// Callers fill an httpclient_request_t and pass an empty response object to
// httpclient__request(). The URL must use http:// or https://, with an ASCII
// hostname or IP address. Spaces and non-ASCII path characters must already
// be written as percent-encoded bytes, such as %20 for a space. A NULL method
// selects GET; HEAD, POST, PUT, PATCH, DELETE and OPTIONS are also supported.
// All request and response sizes count bytes.
//
// A zero timeout_ms selects 15 seconds for connecting and downloading after
// the operating system finishes DNS lookup. A zero max_response_bytes selects
// a 16 MiB body limit. Set public_addresses_only when URLs come from chat users
// so they cannot make the server contact localhost or private networks. On
// Linux and macOS, ca_file can name a PEM file containing trusted certificates;
// leave it NULL to use the system's certificates.
//
// The response owns its headers and body. Both have an extra trailing zero;
// body_size excludes that zero and the body may contain other zero bytes.
// Call httpclient__response_free() before reusing the response object. A 404 or
// another HTTP error status is still a successfully received response. When
// connecting or downloading fails, native_error preserves the operating
// system or TLS library's error number. On Windows, tls_protocol contains
// Schannel's SP_PROT value for the negotiated TLS version.
//
// httpclient_internal.h declares the shared types used by the response parser
// and both connection implementations. The parser's state records whether it
// needs headers, body bytes, a chunk length, a chunk ending, trailing headers,
// or nothing more because the response is complete (state 7).
#include "httpclient_internal.h"

// declarations
static uint _httpclient_internal__length(const char* text, uint maximum);
static boole _httpclient_internal__equal(const char* text, uint length, const char* literal);
static boole _httpclient_internal__token(const char* text, uint size);
static void _httpclient_internal__consume(http_buffer_t* buffer, uint size);
static int _httpclient_internal__line(const ubyte* bytes, uint size);
static boole _httpclient_internal__number(const char* text, uint length, uint base, uint* value);
static httpclient_error_t _httpclient_internal__headers(http_parser_t* parser, httpclient_response_t* response, uint size);
static httpclient_error_t _httpclient_internal__request_head(http_buffer_t* buffer, const httpclient_request_t* request,
    const char* host, const char* port, const char* target, boole ipv6);

/**
 * @brief Checks whether a resolved address is public before requesting a URL preview.
 *
 * @param const struct sockaddr* address -> the IPv4 or IPv6 address that connect() would use
 *
 * @return int -> 1 for an allowed public address, or 0 for a private, reserved or unsupported address
 */
int httpclient_internal__address_allowed(const struct sockaddr* address)
{
    if (address->sa_family == AF_INET)
    {
        const unsigned char* ip = (const unsigned char*)&((const struct sockaddr_in*)address)->sin_addr;
        return !(ip[0] == 0 || ip[0] == 10 || ip[0] == 127 || ip[0] >= 224
            || (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127)
            || (ip[0] == 169 && ip[1] == 254)
            || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31)
            || (ip[0] == 192 && ip[1] == 168)
            || (ip[0] == 192 && ip[1] == 0)
            || (ip[0] == 198 && (ip[1] == 18 || ip[1] == 19))
            || (ip[0] == 198 && ip[1] == 51 && ip[2] == 100)
            || (ip[0] == 203 && ip[1] == 0 && ip[2] == 113));
    }
    if (address->sa_family == AF_INET6)
    {
        const unsigned char* ip = (const unsigned char*)&((const struct sockaddr_in6*)address)->sin6_addr;
        // Only ordinary global IPv6 addresses are useful for previews. This
        // also excludes mapped IPv4, local addresses and tunnel prefixes.
        return (ip[0] & 0xe0) == 0x20
            && !(ip[0] == 0x20 && ip[1] == 0x02)
            && !(ip[0] == 0x20 && ip[1] == 0x01 && ip[2] <= 1)
            && !(ip[0] == 0x20 && ip[1] == 0x01 && ip[2] == 0x0d && ip[3] == 0xb8);
    }
    return 0;
}

/**
 * @brief Counts the bytes in a string without reading beyond the supplied limit.
 *
 * @param const char* text -> the string to measure, or NULL for an empty string
 * @param uint maximum -> the maximum number of bytes to examine
 *
 * @return uint -> the number of bytes before the ending zero byte, limited to maximum
 */
static uint _httpclient_internal__length(const char* text, uint maximum)
{
    uint length = 0;

    if (text == NULL)
    {
        return 0;
    }
    while (length < maximum && text[length] != 0)
    {
        length++;
    }
    return length;
}

/**
 * @brief Compares an HTTP word without treating uppercase and lowercase letters as different.
 *
 * @param const char* text -> the bytes to compare; they do not need an ending zero byte
 * @param uint length -> the number of bytes available in text
 * @param const char* literal -> the comparison string, ending with a zero byte
 *
 * @return boole -> 1 when the lengths and letters match, otherwise 0
 */
static boole _httpclient_internal__equal(const char* text, uint length, const char* literal)
{
    uint index = 0;
    ubyte left = 0;
    ubyte right = 0;

    if (_httpclient_internal__length(literal, length + 1) != length)
    {
        return 0;
    }
    for (index = 0; index < length; index++)
    {
        left = (ubyte)text[index];
        right = (ubyte)literal[index];
        if (left >= 'A' && left <= 'Z')
        {
            left += 'a' - 'A';
        }
        if (right >= 'A' && right <= 'Z')
        {
            right += 'a' - 'A';
        }
        if (left != right)
        {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Checks that a method or header name contains only characters allowed in HTTP names.
 *
 * @param const char* text -> the bytes of the name to check
 * @param uint size -> the number of bytes in the name
 *
 * @return boole -> 1 for a nonempty valid name, otherwise 0
 */
static boole _httpclient_internal__token(const char* text, uint size)
{
    uint index = 0;
    ubyte value = 0;
    boole valid = 0;

    for (index = 0; index < size; index++)
    {
        value = (ubyte)text[index];
        valid = (boole)((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '!' || value == '#' || value == '$' || value == '%' ||
            value == '&' || value == '\'' || value == '*' || value == '+' || value == '-' || value == '.' ||
            value == '^' || value == '_' || value == '`' || value == '|' || value == '~');
        if (!valid)
        {
            return 0;
        }
    }
    return size != 0;
}

/**
 * @brief Adds bytes to a buffer, growing it only as far as its permitted size.
 *
 * @param http_buffer_t* buffer -> the buffer to grow and update
 * @param const void* data -> the bytes to append, or NULL when size is zero
 * @param uint size -> the number of bytes to append
 * @param uint limit -> the maximum number of stored bytes, excluding the ending zero byte
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK on success, HTTPCLIENT_LIMIT when too large, or HTTPCLIENT_MEMORY on allocation failure
 */
httpclient_error_t _httpclient_internal__append(http_buffer_t* buffer, const void* data, uint size, uint limit)
{
    uint needed = 0;
    uint capacity = 0;
    void* allocation = NULL;

    if (buffer->size > limit || size > limit - buffer->size)
    {
        return HTTPCLIENT_LIMIT;
    }
    needed = buffer->size + size + 1;
    if (needed > buffer->capacity)
    {
        capacity = buffer->capacity > 0 ? buffer->capacity : 1024u;
        while (capacity < needed)
        {
            capacity = capacity > (limit + 1) / 2 ? limit + 1 : capacity * 2;
        }
        allocation = buffer->data != NULL ? realloc(buffer->data, capacity) :
            malloc(capacity);
        if (allocation == NULL)
        {
            return HTTPCLIENT_MEMORY;
        }
        buffer->data = (ubyte*)allocation;
        buffer->capacity = capacity;
    }
    if (size != 0)
    {
        memcpy(buffer->data + buffer->size, data, size);
    }
    buffer->size += size;
    buffer->data[buffer->size] = 0;
    return HTTPCLIENT_OK;
}

/**
 * @brief Removes bytes already read by the parser and keeps the unfinished part for the next read.
 *
 * @param http_buffer_t* buffer -> the buffer whose remaining bytes are moved to the start
 * @param uint size -> the number of bytes to remove, which must not exceed buffer->size
 *
 * @return void
 */
static void _httpclient_internal__consume(http_buffer_t* buffer, uint size)
{
    buffer->size -= size;
    if (buffer->size != 0)
    {
        memmove(buffer->data, buffer->data + size, buffer->size);
    }
    if (buffer->data != NULL)
    {
        buffer->data[buffer->size] = 0;
    }
}

/**
 * @brief Finds the carriage return and line feed that finish a complete HTTP line.
 *
 * @param const ubyte* bytes -> the received bytes to inspect
 * @param uint size -> the number of bytes available
 *
 * @return int -> the position of the carriage return, or -1 when the line is incomplete
 */
static int _httpclient_internal__line(const ubyte* bytes, uint size)
{
    uint index = 0;

    for (index = 0; index + 1 < size; index++)
    {
        if (bytes[index] == '\r' && bytes[index + 1] == '\n')
        {
            return (int)index;
        }
    }
    return -1;
}

/**
 * @brief Reads a decimal or hexadecimal number without allowing it to overflow a uint.
 *
 * @param const char* text -> the digits to read; no ending zero byte is required
 * @param uint length -> the number of digits to read
 * @param uint base -> 10 for decimal or 16 for hexadecimal
 * @param uint* value -> receives the parsed number when the digits are valid
 *
 * @return boole -> 1 for a nonempty valid number, otherwise 0
 */
static boole _httpclient_internal__number(const char* text, uint length, uint base, uint* value)
{
    uint index = 0;
    uint digit = 0;
    uint result = 0;
    ubyte byte = 0;

    for (index = 0; index < length; index++)
    {
        byte = (ubyte)text[index];
        digit = byte >= '0' && byte <= '9' ? (uint)(byte - '0') :
            byte >= 'a' && byte <= 'f' ? (uint)(byte - 'a' + 10) :
            byte >= 'A' && byte <= 'F' ? (uint)(byte - 'A' + 10) : base;
        if (digit >= base || result > (0xFFFFFFFFu - digit) / base)
        {
            return 0;
        }
        result = result * base + digit;
    }
    *value = result;
    return length != 0;
}

/**
 * @brief Reads the HTTP status and headers and decides how the response body ends.
 *
 * @param http_parser_t* parser -> holds the complete headers in pending and receives the body-reading state
 * @param httpclient_response_t* response -> receives the final status and an allocated copy of the headers
 * @param uint size -> the complete header size in bytes, including the ending empty line
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK for accepted headers, or the parsing, size or allocation error
 */
static httpclient_error_t _httpclient_internal__headers(http_parser_t* parser, httpclient_response_t* response, uint size)
{
    const char* text = (const char*)parser->pending.data;
    uint position = 0;
    uint length = 0;
    uint colon = 0;
    uint start = 0;
    uint end = 0;
    uint index = 0;
    uint status = 0;
    uint content_length = 0;
    int line = 0;
    boole has_length = 0;
    boole chunked = 0;
    boole no_body = 0;

    if (size > HTTP_HEADER_LIMIT - parser->metadata_bytes)
    {
        return HTTPCLIENT_LIMIT;
    }
    parser->metadata_bytes += size;
    line = _httpclient_internal__line(parser->pending.data, size);
    if (line < 12 || (line > 12 && text[12] != ' ') ||
        (!_httpclient_internal__equal(text, 8, "HTTP/1.1") && !_httpclient_internal__equal(text, 8, "HTTP/1.0")) ||
        text[8] != ' ' || !_httpclient_internal__number(text + 9, 3, 10, &status) || status < 100 || status > 599)
    {
        return HTTPCLIENT_HTTP;
    }
    for (index = 12; index < (uint)line; index++)
    {
        if (((ubyte)text[index] < 32 && text[index] != '\t') || (ubyte)text[index] == 127)
        {
            return HTTPCLIENT_HTTP;
        }
    }
    position = (uint)line + 2;
    while (position + 2 < size)
    {
        line = _httpclient_internal__line(parser->pending.data + position, size - position);
        if (line <= 0)
        {
            return HTTPCLIENT_HTTP;
        }
        length = (uint)line;
        colon = 0;
        while (colon < length && text[position + colon] != ':')
        {
            colon++;
        }
        if (colon == length || !_httpclient_internal__token(text + position, colon))
        {
            return HTTPCLIENT_HTTP;
        }
        start = position + colon + 1;
        end = position + length;
        for (index = start; index < end; index++)
        {
            if (((ubyte)text[index] < 32 && text[index] != '\t') || (ubyte)text[index] == 127)
            {
                return HTTPCLIENT_HTTP;
            }
        }
        while (start < end && (text[start] == ' ' || text[start] == '\t'))
        {
            start++;
        }
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        {
            end--;
        }
        if (_httpclient_internal__equal(text + position, colon, "Content-Length"))
        {
            if (has_length || !_httpclient_internal__number(text + start, end - start, 10, &content_length))
            {
                return HTTPCLIENT_HTTP;
            }
            has_length = 1;
        }
        else if (_httpclient_internal__equal(text + position, colon, "Transfer-Encoding"))
        {
            if (chunked || !_httpclient_internal__equal(text + start, end - start, "chunked"))
            {
                return HTTPCLIENT_UNSUPPORTED;
            }
            chunked = 1;
        }
        else if (_httpclient_internal__equal(text + position, colon, "Content-Encoding"))
        {
            if (!_httpclient_internal__equal(text + start, end - start, "identity"))
            {
                return HTTPCLIENT_UNSUPPORTED;
            }
        }
        position += length + 2;
    }
    if (has_length && chunked)
    {
        return HTTPCLIENT_HTTP;
    }
    if (status == 101)
    {
        return HTTPCLIENT_UNSUPPORTED;
    }
    if (status < 200)
    {
        if (++parser->informational > 16 || has_length || chunked)
        {
            return HTTPCLIENT_HTTP;
        }
        _httpclient_internal__consume(&parser->pending, size);
        return HTTPCLIENT_OK;
    }
    no_body = (boole)(parser->head || status == 204 || status == 304);
    if (!no_body && has_length && content_length > parser->limit)
    {
        return HTTPCLIENT_LIMIT;
    }
    response->headers = (char*)malloc(size + 1);
    if (response->headers == NULL)
    {
        return HTTPCLIENT_MEMORY;
    }
    memcpy(response->headers, text, size);
    response->headers[size] = 0;
    response->status_code = status;
    parser->remaining = content_length;
    parser->state = no_body ? 7 : chunked ? 3 : has_length ? (content_length == 0 ? 7 : 1) : 2;
    _httpclient_internal__consume(&parser->pending, size);
    return HTTPCLIENT_OK;
}

/**
 * @brief Reads as much of a URL preview response as the currently received bytes allow.
 *
 * @param http_parser_t* parser -> the current reading state, unfinished bytes and accumulated response body
 * @param httpclient_response_t* response -> receives the HTTP status and headers
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when the bytes are valid, including when more bytes are needed, or a parsing error
 */
httpclient_error_t _httpclient_internal__parse(http_parser_t* parser, httpclient_response_t* response)
{
    uint index = 0;
    uint amount = 0;
    uint digits = 0;
    int line = 0;
    ubyte value = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    for (;;)
    {
        if (parser->state == 0)
        {
            amount = 0;
            for (index = 0; index + 3 < parser->pending.size; index++)
            {
                if (parser->pending.data[index] == '\r' && parser->pending.data[index + 1] == '\n' &&
                    parser->pending.data[index + 2] == '\r' && parser->pending.data[index + 3] == '\n')
                {
                    amount = index + 4;
                    break;
                }
            }
            if (amount == 0)
            {
                return parser->pending.size >= HTTP_HEADER_LIMIT - parser->metadata_bytes ? HTTPCLIENT_LIMIT : HTTPCLIENT_OK;
            }
            error = _httpclient_internal__headers(parser, response, amount);
            if (error != HTTPCLIENT_OK)
            {
                return error;
            }
        }
        else if (parser->state == 1 || parser->state == 2 || parser->state == 4)
        {
            amount = parser->pending.size;
            if (parser->state != 2 && amount > parser->remaining)
            {
                amount = parser->remaining;
            }
            if (amount == 0)
            {
                return HTTPCLIENT_OK;
            }
            error = _httpclient_internal__append(&parser->body, parser->pending.data, amount, parser->limit);
            if (error != HTTPCLIENT_OK)
            {
                return error;
            }
            _httpclient_internal__consume(&parser->pending, amount);
            if (parser->state != 2)
            {
                parser->remaining -= amount;
                if (parser->remaining == 0)
                {
                    parser->state = parser->state == 1 ? 7 : 5;
                }
            }
        }
        else if (parser->state == 3 || parser->state == 6)
        {
            line = _httpclient_internal__line(parser->pending.data, parser->pending.size);
            if (line < 0)
            {
                return parser->pending.size >= 8192 ? HTTPCLIENT_LIMIT : HTTPCLIENT_OK;
            }
            if (line > 8190 || (uint)line + 2 > HTTP_HEADER_LIMIT - parser->metadata_bytes)
            {
                return HTTPCLIENT_LIMIT;
            }
            parser->metadata_bytes += (uint)line + 2;
            for (index = 0; index < (uint)line; index++)
            {
                value = parser->pending.data[index];
                if ((value < 32 && value != '\t') || value == 127)
                {
                    return HTTPCLIENT_HTTP;
                }
            }
            if (parser->state == 3)
            {
                digits = 0;
                while (digits < (uint)line && parser->pending.data[digits] != ';')
                {
                    digits++;
                }
                if (!_httpclient_internal__number((const char*)parser->pending.data, digits, 16, &parser->remaining))
                {
                    return HTTPCLIENT_HTTP;
                }
                if (parser->remaining > parser->limit - parser->body.size)
                {
                    return HTTPCLIENT_LIMIT;
                }
                parser->state = parser->remaining == 0 ? 6 : 4;
            }
            else if (line == 0)
            {
                parser->state = 7;
            }
            else
            {
                digits = 0;
                while (digits < (uint)line && parser->pending.data[digits] != ':')
                {
                    digits++;
                }
                if (digits == (uint)line || !_httpclient_internal__token((const char*)parser->pending.data, digits) ||
                    _httpclient_internal__equal((const char*)parser->pending.data, digits, "Content-Length") ||
                    _httpclient_internal__equal((const char*)parser->pending.data, digits, "Transfer-Encoding") ||
                    _httpclient_internal__equal((const char*)parser->pending.data, digits, "Content-Encoding"))
                {
                    return HTTPCLIENT_HTTP;
                }
            }
            _httpclient_internal__consume(&parser->pending, (uint)line + 2);
        }
        else if (parser->state == 5)
        {
            if (parser->pending.size < 2)
            {
                return HTTPCLIENT_OK;
            }
            if (parser->pending.data[0] != '\r' || parser->pending.data[1] != '\n')
            {
                return HTTPCLIENT_HTTP;
            }
            _httpclient_internal__consume(&parser->pending, 2);
            parser->state = 3;
        }
        else
        {
            // There is no pipelined request. Any already-received excess plaintext is invalid.
            return parser->pending.size == 0 ? HTTPCLIENT_OK : HTTPCLIENT_HTTP;
        }
    }
}

/**
 * @brief Writes the request method, path and headers for a URL preview download.
 *
 * Extra headers cannot replace body-length or connection headers.
 *
 * @param http_buffer_t* buffer -> receives the request line and headers, excluding the body
 * @param const httpclient_request_t* request -> the method, extra headers and body size to validate and write
 * @param const char* host -> the hostname or IP address without IPv6 brackets
 * @param const char* port -> the destination port written as decimal text
 * @param const char* target -> the request path and query, beginning with a slash
 * @param boole ipv6 -> nonzero when the Host header needs brackets around host
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK when the headers are ready, or an argument, size or allocation error
 */
static httpclient_error_t _httpclient_internal__request_head(http_buffer_t* buffer, const httpclient_request_t* request,
    const char* host, const char* port, const char* target, boole ipv6)
{
    static const char* methods[] =
    {
        "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"
    };
    const char* method = NULL;
    const char* parts[15];
    const char* name = NULL;
    const char* value = NULL;
    char number[11];
    uint offset = 10;
    uint remaining = 0;
    uint index = 0;
    uint field = 0;
    uint length = 0;
    uint name_length = 0;
    uint value_length = 0;
    ubyte byte = 0;
    boole reserved = 0;
    httpclient_error_t error = HTTPCLIENT_OK;

    length = _httpclient_internal__length(request->method, 16);
    for (index = 0; index < sizeof(methods) / sizeof(methods[0]); index++)
    {
        if (request->method == NULL || _httpclient_internal__equal(request->method, length, methods[index]))
        {
            method = methods[index];
            break;
        }
    }
    if (method == NULL || request->header_count > 128 || (request->header_count != 0 && request->headers == NULL))
    {
        return HTTPCLIENT_ARGUMENT;
    }
    number[10] = 0;
    remaining = request->body_size;
    do
    {
        number[--offset] = (char)('0' + remaining % 10);
        remaining /= 10;
    }
    while (remaining != 0);
    parts[0] = method;
    parts[1] = " ";
    parts[2] = target;
    parts[3] = " HTTP/1.1\r\nHost: ";
    parts[4] = ipv6 ? "[" : "";
    parts[5] = host;
    parts[6] = ipv6 ? "]" : "";
    parts[7] = ":";
    parts[8] = port;
    parts[9] = "\r\nConnection: close\r\nAccept-Encoding: identity\r\nContent-Length: ";
    parts[10] = number + offset;
    parts[11] = "\r\n";
    for (index = 0; index < 12; index++)
    {
        error = _httpclient_internal__append(buffer, parts[index], _httpclient_internal__length(parts[index], 8193), HTTP_HEADER_LIMIT);
        if (error != HTTPCLIENT_OK)
        {
            return error;
        }
    }
    for (field = 0; field < request->header_count; field++)
    {
        name = request->headers[field].name;
        value = request->headers[field].value;
        name_length = _httpclient_internal__length(name, 1024);
        value_length = _httpclient_internal__length(value, 8192);
        reserved = (boole)(_httpclient_internal__equal(name, name_length, "Host") ||
            _httpclient_internal__equal(name, name_length, "Content-Length") ||
            _httpclient_internal__equal(name, name_length, "Transfer-Encoding") ||
            _httpclient_internal__equal(name, name_length, "Connection") ||
            _httpclient_internal__equal(name, name_length, "Accept-Encoding") ||
            _httpclient_internal__equal(name, name_length, "Expect") ||
            _httpclient_internal__equal(name, name_length, "Upgrade"));
        if (name == NULL || value == NULL || name_length == 1024 || value_length == 8192 ||
            !_httpclient_internal__token(name, name_length) || reserved)
        {
            return HTTPCLIENT_ARGUMENT;
        }
        for (index = 0; index < value_length; index++)
        {
            byte = (ubyte)value[index];
            if ((byte < 32 && byte != '\t') || byte == 127)
            {
                return HTTPCLIENT_ARGUMENT;
            }
        }
        parts[0] = name;
        parts[1] = ": ";
        parts[2] = value;
        parts[3] = "\r\n";
        for (index = 0; index < 4; index++)
        {
            error = _httpclient_internal__append(buffer, parts[index], _httpclient_internal__length(parts[index], 8192), HTTP_HEADER_LIMIT);
            if (error != HTTPCLIENT_OK)
            {
                return error;
            }
        }
    }
    return _httpclient_internal__append(buffer, "\r\n", 2, HTTP_HEADER_LIMIT);
}

/**
 * @brief Splits an HTTP or HTTPS URL into the host, port and request path used for a preview download.
 *
 * Missing ports use 80 for HTTP or 443 for HTTPS. Fragments and credentials are rejected.
 *
 * @param const char* url -> an absolute ASCII URL; spaces and non-ASCII path bytes must already be percent-encoded
 * @param httpclient_url_t* parsed -> receives the URL parts and HTTPS and IPv6 flags
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK for a supported URL, otherwise HTTPCLIENT_ARGUMENT
 */
httpclient_error_t httpclient__parse_url(const char* url, httpclient_url_t* parsed)
{
    if (parsed == NULL)
    {
        return HTTPCLIENT_ARGUMENT;
    }
    memset(parsed, 0, sizeof(*parsed));
    char* host = parsed->host;
    char* port = parsed->port;
    char* target = parsed->target;
    int* ipv6 = &parsed->is_ipv6;
    uint length = 0;
    uint index = 0;
    uint start = 8;
    uint end = 0;
    uint port_start = 0;
    uint port_end = 0;
    uint number = 0;
    ubyte value = 0;
    struct in6_addr address;

    length = _httpclient_internal__length(url, 8192);
    parsed->is_https = length >= 8 && _httpclient_internal__equal(url, 8, "https://");
    start = parsed->is_https ? 8 : 7;
    if (length <= start || length == 8192 || (!parsed->is_https && !_httpclient_internal__equal(url, 7, "http://")))
    {
        return HTTPCLIENT_ARGUMENT;
    }
    for (index = start; index < length; index++)
    {
        value = (ubyte)url[index];
        if (value <= 32 || value >= 127 || value == '#' || value == '\\')
        {
            return HTTPCLIENT_ARGUMENT;
        }
    }
    *ipv6 = url[start] == '[';
    if (*ipv6)
    {
        start++;
    }
    end = start;
    while (end < length && url[end] != '/' && url[end] != '?' && url[end] != ']' && (*ipv6 || url[end] != ':'))
    {
        end++;
    }
    if (end == start || end - start > 253 || (*ipv6 && (end == length || url[end] != ']')))
    {
        return HTTPCLIENT_ARGUMENT;
    }
    memcpy(host, url + start, end - start);
    host[end - start] = 0;
    if (*ipv6)
    {
        if (inet_pton(AF_INET6, host, &address) != 1)
        {
            return HTTPCLIENT_ARGUMENT;
        }
        end++;
    }
    else
    {
        for (index = 0; host[index] != 0; index++)
        {
            value = (ubyte)host[index];
            if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                (value >= '0' && value <= '9') || value == '-' || value == '.'))
            {
                return HTTPCLIENT_ARGUMENT;
            }
        }
    }
    memcpy(port, parsed->is_https ? "443" : "80", parsed->is_https ? 4 : 3);
    if (end < length && url[end] == ':')
    {
        port_start = ++end;
        while (end < length && url[end] != '/' && url[end] != '?')
        {
            end++;
        }
        port_end = end;
        if (port_end - port_start > 5 || !_httpclient_internal__number(url + port_start, port_end - port_start, 10, &number) ||
            number == 0 || number > 65535)
        {
            return HTTPCLIENT_ARGUMENT;
        }
        memcpy(port, url + port_start, port_end - port_start);
        port[port_end - port_start] = 0;
    }
    if (end < length && url[end] != '/' && url[end] != '?')
    {
        return HTTPCLIENT_ARGUMENT;
    }
    index = 0;
    if (end == length || url[end] == '?')
    {
        target[index++] = '/';
    }
    memcpy(target + index, url + end, length - end);
    target[index + length - end] = 0;
    return HTTPCLIENT_OK;
}

/**
 * @brief Frees the downloaded HTTP headers and body and clears the response for reuse.
 *
 * @param httpclient_response_t* response -> the response to clear, or NULL to do nothing
 *
 * @return void
 */
void httpclient__response_free(httpclient_response_t* response)
{
    if (response == NULL)
    {
        return;
    }
    if (response->headers != NULL)
    {
        free(response->headers);
    }
    if (response->body != NULL)
    {
        free(response->body);
    }
    memset(response, 0, sizeof(*response));
}

/**
 * @brief Downloads one HTTP response for a URL preview using the connection code for this operating system.
 *
 * Requests have separate buffers for simultaneous use and do not follow redirects.
 * On failure, only the native error and TLS version remain in the response.
 *
 * @param const httpclient_request_t* request -> the URL, headers, body and connection limits; the caller keeps these values alive until return
 * @param httpclient_response_t* response -> an empty response that receives owned headers and body; release them with httpclient__response_free()
 *
 * @return httpclient_error_t -> HTTPCLIENT_OK for a complete response, including HTTP error statuses, or the connection or parsing error
 */
httpclient_error_t httpclient__request(const httpclient_request_t* request, httpclient_response_t* response)
{
    httpclient_connection_t* connection = NULL;
    http_parser_t parser = {0};
    http_buffer_t header = {0};
    httpclient_url_t url;
    boole closed = 0;
    httpclient_error_t error = HTTPCLIENT_ARGUMENT;

    if (response == NULL)
    {
        return HTTPCLIENT_ARGUMENT;
    }
    memset(response, 0, sizeof(*response));
    if (request == NULL || request->url == NULL || (request->body_size != 0 && request->body == NULL)
        || request->body_size > BODY_LIMIT_MAX || request->max_response_bytes > BODY_LIMIT_MAX)
    {
        return HTTPCLIENT_ARGUMENT;
    }
    parser.limit = request->max_response_bytes == 0 ? 16u * 1024u * 1024u : request->max_response_bytes;
    parser.head = request->method != NULL && _httpclient_internal__equal(request->method,
        _httpclient_internal__length(request->method, 16), "HEAD");
    error = httpclient__parse_url(request->url, &url);
    if (error == HTTPCLIENT_OK)
    {
        error = _httpclient_internal__request_head(&header, request, url.host, url.port, url.target, (boole)url.is_ipv6);
    }
    if (error == HTTPCLIENT_OK)
    {
        error = httpclient_transport__open(request, &url, response, &connection);
    }
    if (error == HTTPCLIENT_OK)
    {
        error = httpclient_transport__write(connection, header.data, header.size);
    }
    if (error == HTTPCLIENT_OK && request->body_size != 0)
    {
        error = httpclient_transport__write(connection, request->body, request->body_size);
    }
    while (error == HTTPCLIENT_OK && parser.state != 7)
    {
        error = httpclient_transport__read(connection, &parser, &closed);
        if (error == HTTPCLIENT_OK && closed)
        {
            if (parser.state == 2)
            {
                parser.state = 7;
            }
            else if (parser.state != 7)
            {
                error = HTTPCLIENT_TRUNCATED;
            }
        }
    }
    if (error == HTTPCLIENT_OK)
    {
        error = _httpclient_internal__append(&parser.body, NULL, 0, parser.limit);
        if (error == HTTPCLIENT_OK)
        {
            response->body = parser.body.data;
            response->body_size = parser.body.size;
            parser.body.data = NULL;
        }
    }
    free(parser.pending.data);
    free(parser.body.data);
    free(header.data);
    httpclient_transport__close(connection);
    if (error != HTTPCLIENT_OK)
    {
        uint native = response->native_error;
        uint protocol = response->tls_protocol;
        httpclient__response_free(response);
        response->native_error = native;
        response->tls_protocol = protocol;
    }
    return error;
}

/**
 * @brief Returns a readable explanation of a URL preview HTTP client error.
 *
 * @param httpclient_error_t error -> the error code to describe
 *
 * @return const char* -> fixed text owned by this file; the caller must not change or free it
 */
const char* httpclient__error_text(httpclient_error_t error)
{
    switch (error)
    {
        case HTTPCLIENT_OK:
        {
            return "success";
        }
        case HTTPCLIENT_ARGUMENT:
        {
            return "invalid or unsupported request argument";
        }
        case HTTPCLIENT_MEMORY:
        {
            return "memory allocation failed";
        }
        case HTTPCLIENT_DNS:
        {
            return "name resolution failed";
        }
        case HTTPCLIENT_SOCKET:
        {
            return "socket operation failed";
        }
        case HTTPCLIENT_TIMEOUT:
        {
            return "socket transaction deadline expired";
        }
        case HTTPCLIENT_TLS:
        {
            return "TLS handshake, certificate validation or record processing failed";
        }
        case HTTPCLIENT_HTTP:
        {
            return "malformed or ambiguous HTTP response";
        }
        case HTTPCLIENT_LIMIT:
        {
            return "response or protocol metadata exceeded the configured limit";
        }
        case HTTPCLIENT_UNSUPPORTED:
        {
            return "unsupported HTTP encoding, upgrade or client-certificate requirement";
        }
        case HTTPCLIENT_TRUNCATED:
        {
            return "connection closed before a complete authenticated response";
        }
        default:
        {
            return "unknown httpclient error";
        }
    }
}
