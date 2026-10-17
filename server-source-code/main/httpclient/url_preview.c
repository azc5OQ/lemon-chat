// This file creates the URL preview shown when a client posts a website link
// in chat. The feature must be enabled in the server settings. It finds the
// page's title, description, site name and picture so the client can display
// them below the message.
//
// It reads the downloaded page without running the website's JavaScript.
// Pictures are downloaded here and sent as image bytes so the receiving
// clients do not have to contact the website themselves.
// The result keeps only the pieces needed by the card. Its image_data member
// owns an allocated data URL, or is NULL when no supported picture was found.
// Call url_preview__free() when the result is no longer needed.
//
// Open Graph fields are preferred, followed by Twitter card fields and the
// ordinary HTML title and description. Pages must return HTML without
// compression. Sites that require login or JavaScript may have no preview.
// Pages are limited to 1 MiB and pictures to 256 KiB. Oversized or unsupported
// pictures are omitted while the title and description are kept.
// Each download follows at most three redirects, checking the actual network
// address before every connection. Each connection gets eight seconds after
// DNS lookup; DNS itself uses the operating system's timeout. Redirects and
// pictures start another connection, so the complete lookup can take longer.
#include "httpclient.h"
#include "url_preview.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/base64.h>

#define PREVIEW_PAGE_LIMIT (1024u * 1024u)
#define PREVIEW_IMAGE_LIMIT (256u * 1024u)
#define PREVIEW_REDIRECT_LIMIT 3

// declarations
static int _url_preview_internal__same(const char* left, const char* right);
static int _url_preview_internal__starts_with(const char* text, const char* prefix);
static const char* _url_preview_internal__find(const char* text, const char* wanted);
static void _url_preview_internal__copy(char* output, size_t capacity, const char* text, size_t length);
static void _url_preview_internal__text(const char* source, size_t length, char* output, size_t capacity);
static void _url_preview_internal__choose(char* output, size_t capacity, const char* value, int priority, int* previous);
static void _url_preview_internal__remove_directory_steps(char* url);
static int _url_preview_internal__header(const char* headers, const char* name, char* output, size_t capacity);
static int _url_preview_internal__download(char* url, unsigned int limit, httpclient_response_t* response);

/**
 * @brief Checks whether two strings contain the same text, ignoring uppercase and lowercase letters.
 *
 * @param const char* left -> the first string, ending with a zero byte
 * @param const char* right -> the second string, ending with a zero byte
 *
 * @return int -> 1 when both strings match, otherwise 0
 */
static int _url_preview_internal__same(const char* left, const char* right)
{
    while (*left && *right)
    {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++))
        {
            return 0;
        }
    }
    return *left == *right;
}

/**
 * @brief Checks whether a string starts with the requested text, ignoring uppercase and lowercase letters.
 *
 * @param const char* text -> the string to check, ending with a zero byte
 * @param const char* prefix -> the text that must appear at the start
 *
 * @return int -> 1 when the prefix matches, otherwise 0
 */
static int _url_preview_internal__starts_with(const char* text, const char* prefix)
{
    while (*prefix)
    {
        if (tolower((unsigned char)*text++) != tolower((unsigned char)*prefix++))
        {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Finds the first occurrence of some text, ignoring uppercase and lowercase letters.
 *
 * @param const char* text -> the string to search, ending with a zero byte
 * @param const char* wanted -> the text to find
 *
 * @return const char* -> a pointer into text at the match, or NULL when no match was found
 */
static const char* _url_preview_internal__find(const char* text, const char* wanted)
{
    size_t size = strlen(wanted);
    for (; *text; text++)
    {
        size_t index = 0;
        while (index < size && text[index]
            && tolower((unsigned char)text[index]) == tolower((unsigned char)wanted[index]))
        {
            index++;
        }
        if (index == size)
        {
            return text;
        }
    }
    return NULL;
}

/**
 * @brief Copies text into a fixed-size buffer and leaves room for its ending zero byte.
 *
 * @param char* output -> the buffer that receives the copied text
 * @param size_t capacity -> the size of output in bytes, which must be greater than zero
 * @param const char* text -> the source bytes to copy
 * @param size_t length -> the number of source bytes to copy, shortened when output is too small
 *
 * @return void
 */
static void _url_preview_internal__copy(char* output, size_t capacity, const char* text, size_t length)
{
    if (length >= capacity)
    {
        length = capacity - 1;
    }
    memcpy(output, text, length);
    output[length] = 0;
}

/**
 * @brief Turns text read from HTML into the readable text shown in a URL preview.
 *
 * It reads HTML character references such as &amp; and &#233;, keeps valid UTF-8
 * characters whole when shortening the text, and turns line breaks into spaces.
 *
 * @param const char* source -> the HTML text to read
 * @param size_t length -> the maximum number of source bytes to read
 * @param char* output -> the buffer that receives the cleaned text and an ending zero byte
 * @param size_t capacity -> the size of output in bytes, which must be greater than zero
 *
 * @return void
 */
static void _url_preview_internal__text(const char* source, size_t length, char* output, size_t capacity)
{
    size_t written = 0;
    for (size_t index = 0; index < length && source[index];)
    {
        unsigned int code = (unsigned char)source[index];
        size_t consumed = 1;
        if (code == '&')
        {
            size_t end = index + 1;
            while (end < length && end - index < 16 && source[end] && source[end] != ';')
            {
                end++;
            }
            if (end < length && source[end] == ';')
            {
                char entity[17];
                _url_preview_internal__copy(entity, sizeof(entity), source + index + 1, end - index - 1);
                unsigned long decoded = 0;
                if (entity[0] == '#')
                {
                    char* tail = NULL;
                    int hexadecimal = entity[1] == 'x' || entity[1] == 'X';
                    const char* digits = entity + (hexadecimal ? 2 : 1);
                    decoded = strtoul(digits, &tail, hexadecimal ? 16 : 10);
                    if (tail == digits || *tail != 0)
                    {
                        decoded = 0;
                    }
                }
                else if (!strcmp(entity, "amp"))
                {
                    decoded = '&';
                }
                else if (!strcmp(entity, "lt"))
                {
                    decoded = '<';
                }
                else if (!strcmp(entity, "gt"))
                {
                    decoded = '>';
                }
                else if (!strcmp(entity, "quot"))
                {
                    decoded = '"';
                }
                else if (!strcmp(entity, "apos") || !strcmp(entity, "#39"))
                {
                    decoded = '\'';
                }
                else if (!strcmp(entity, "nbsp"))
                {
                    decoded = ' ';
                }
                if (decoded > 0 && decoded <= 0x10ffff && !(decoded >= 0xd800 && decoded <= 0xdfff))
                {
                    code = (unsigned int)decoded;
                    consumed = end - index + 1;
                }
            }
        }
        else if (code >= 128)
        {
            size_t count = code >= 0xc2 && code <= 0xdf ? 2 : code >= 0xe0 && code <= 0xef ? 3 : code >= 0xf0 && code <= 0xf4 ? 4 : 0;
            unsigned int decoded = count == 2 ? code & 31 : count == 3 ? code & 15 : code & 7;
            int valid = count != 0 && index + count <= length;
            for (size_t part = 1; valid && part < count; part++)
            {
                unsigned char byte = (unsigned char)source[index + part];
                valid = (byte & 0xc0) == 0x80;
                decoded = (decoded << 6) | (byte & 63);
            }
            valid = valid && decoded >= (count == 2 ? 0x80u : count == 3 ? 0x800u : 0x10000u)
                && decoded <= 0x10ffff && !(decoded >= 0xd800 && decoded <= 0xdfff);
            code = valid ? decoded : 0xfffd;
            consumed = valid ? count : 1;
        }
        index += consumed;
        if (code <= 32 || code == 127 || code == 160)
        {
            if (written == 0 || output[written - 1] == ' ')
            {
                continue;
            }
            code = ' ';
        }
        unsigned char bytes[4];
        size_t count = 1;
        if (code < 128)
        {
            bytes[0] = (unsigned char)code;
        }
        else if (code < 2048)
        {
            count = 2;
            bytes[0] = (unsigned char)(0xc0 | (code >> 6));
            bytes[1] = (unsigned char)(0x80 | (code & 63));
        }
        else if (code < 65536)
        {
            count = 3;
            bytes[0] = (unsigned char)(0xe0 | (code >> 12));
            bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 63));
            bytes[2] = (unsigned char)(0x80 | (code & 63));
        }
        else
        {
            count = 4;
            bytes[0] = (unsigned char)(0xf0 | (code >> 18));
            bytes[1] = (unsigned char)(0x80 | ((code >> 12) & 63));
            bytes[2] = (unsigned char)(0x80 | ((code >> 6) & 63));
            bytes[3] = (unsigned char)(0x80 | (code & 63));
        }
        if (written + count >= capacity)
        {
            break;
        }
        memcpy(output + written, bytes, count);
        written += count;
    }
    if (written && output[written - 1] == ' ')
    {
        written--;
    }
    output[written] = 0;
}

/**
 * @brief Replaces a preview field when a new nonempty value has a higher priority than the current one.
 *
 * @param char* output -> the preview field to update with cleaned HTML text
 * @param size_t capacity -> the size of output in bytes, which must be greater than zero
 * @param const char* value -> the new value found in the page
 * @param int priority -> the priority of the new value, with larger numbers preferred
 * @param int* previous -> the current priority, updated when a nonempty value is accepted
 *
 * @return void
 */
static void _url_preview_internal__choose(char* output, size_t capacity, const char* value, int priority, int* previous)
{
    if (value[0] && priority > *previous)
    {
        _url_preview_internal__text(value, strlen(value), output, capacity);
        if (output[0])
        {
            *previous = priority;
        }
    }
}

/**
 * @brief Reads a downloaded page for the title, description, site name and picture used in a URL preview.
 *
 * Scripts are skipped. Open Graph values take priority over Twitter and ordinary tags.
 *
 * @param const char* html -> the HTML text, ending with a zero byte
 * @param url_preview_t* preview -> the initialized preview whose text and image URL fields are filled
 *
 * @return void
 */
void url_preview__read_html(const char* html, url_preview_t* preview)
{
    int title_priority = 0;
    int description_priority = 0;
    int image_priority = 0;
    const char* cursor = html;
    while ((cursor = strchr(cursor, '<')) != NULL)
    {
        if (!strncmp(cursor, "<!--", 4))
        {
            cursor = strstr(cursor + 4, "-->");
            if (cursor == NULL)
            {
                break;
            }
            cursor += 3;
            continue;
        }
        cursor++;
        if (*cursor == '/')
        {
            if (_url_preview_internal__starts_with(cursor, "/head"))
            {
                break;
            }
            continue;
        }
        const char* name_start = cursor;
        while (isalnum((unsigned char)*cursor))
        {
            cursor++;
        }
        char tag[32];
        _url_preview_internal__copy(tag, sizeof(tag), name_start, (size_t)(cursor - name_start));
        char name[128] = {0};
        char property[128] = {0};
        char content[4097] = {0};
        // Attribute values can contain '>', and attributes may arrive in any
        // order. Read each quoted value before looking for the next attribute.
        while (*cursor && *cursor != '>')
        {
            while (isspace((unsigned char)*cursor) || *cursor == '/')
            {
                cursor++;
            }
            if (!*cursor || *cursor == '>')
            {
                break;
            }
            const char* attribute_start = cursor;
            while (*cursor && !isspace((unsigned char)*cursor) && *cursor != '=' && *cursor != '>')
            {
                cursor++;
            }
            char attribute[64];
            _url_preview_internal__copy(attribute, sizeof(attribute), attribute_start, (size_t)(cursor - attribute_start));
            while (isspace((unsigned char)*cursor))
            {
                cursor++;
            }
            if (*cursor != '=')
            {
                continue;
            }
            cursor++;
            while (isspace((unsigned char)*cursor))
            {
                cursor++;
            }
            char quote = *cursor == '"' || *cursor == '\'' ? *cursor++ : 0;
            const char* value = cursor;
            while (*cursor && (quote ? *cursor != quote : !isspace((unsigned char)*cursor) && *cursor != '>'))
            {
                cursor++;
            }
            size_t length = (size_t)(cursor - value);
            if (_url_preview_internal__same(attribute, "name"))
            {
                _url_preview_internal__copy(name, sizeof(name), value, length);
            }
            else if (_url_preview_internal__same(attribute, "property"))
            {
                _url_preview_internal__copy(property, sizeof(property), value, length);
            }
            else if (_url_preview_internal__same(attribute, "content"))
            {
                _url_preview_internal__copy(content, sizeof(content), value, length);
            }
            if (quote && *cursor == quote)
            {
                cursor++;
            }
        }
        if (!*cursor)
        {
            break;
        }
        cursor++;
        if (_url_preview_internal__same(tag, "script") || _url_preview_internal__same(tag, "style"))
        {
            cursor = _url_preview_internal__find(cursor, _url_preview_internal__same(tag, "script") ? "</script" : "</style");
            if (cursor == NULL)
            {
                break;
            }
            continue;
        }
        if (_url_preview_internal__same(tag, "title"))
        {
            const char* end = _url_preview_internal__find(cursor, "</title");
            if (end != NULL)
            {
                if (title_priority == 0)
                {
                    _url_preview_internal__text(cursor, (size_t)(end - cursor), preview->title, sizeof(preview->title));
                    title_priority = preview->title[0] ? 1 : 0;
                }
                cursor = end;
            }
        }
        else if (_url_preview_internal__same(tag, "meta"))
        {
            const char* key = property[0] ? property : name;
            if (_url_preview_internal__same(key, "og:title") || _url_preview_internal__same(key, "twitter:title"))
            {
                _url_preview_internal__choose(preview->title, sizeof(preview->title), content, _url_preview_internal__same(key, "og:title") ? 3 : 2, &title_priority);
            }
            else if (_url_preview_internal__same(key, "og:description") || _url_preview_internal__same(key, "twitter:description") || _url_preview_internal__same(key, "description"))
            {
                int priority = _url_preview_internal__same(key, "og:description") ? 3 : _url_preview_internal__same(key, "twitter:description") ? 2 : 1;
                _url_preview_internal__choose(preview->description, sizeof(preview->description), content, priority, &description_priority);
            }
            else if (_url_preview_internal__same(key, "og:image") || _url_preview_internal__same(key, "og:image:secure_url") || _url_preview_internal__same(key, "twitter:image"))
            {
                int priority = _url_preview_internal__same(key, "og:image:secure_url") ? 4 : _url_preview_internal__same(key, "og:image") ? 3 : 2;
                _url_preview_internal__choose(preview->image_url, sizeof(preview->image_url), content, priority, &image_priority);
            }
            else if (_url_preview_internal__same(key, "og:site_name"))
            {
                _url_preview_internal__text(content, strlen(content), preview->site_name, sizeof(preview->site_name));
            }
        }
    }
}

/**
 * @brief Removes current-directory and parent-directory steps from the path in an absolute URL.
 *
 * A picture such as ../images/cover.png names a file one directory above the page.
 * These steps are removed before requesting it, while everything after '?' stays
 * unchanged because it belongs to the website's query.
 *
 * @param char* url -> a writable absolute URL containing ://, updated in place
 *
 * @return void
 */
static void _url_preview_internal__remove_directory_steps(char* url)
{
    char* authority = strstr(url, "://") + 3;
    char* path = strchr(authority, '/');
    char* query = strchr(authority, '?');
    if (path == NULL || (query != NULL && query < path))
    {
        return;
    }
    char* end = path + strcspn(path, "?");
    char* read = path;
    char* write = path;
    while (read < end)
    {
        size_t remaining = (size_t)(end - read);
        if ((remaining >= 3 && !strncmp(read, "/./", 3)) || (remaining == 2 && !strncmp(read, "/.", 2)))
        {
            read += 2;
            if (read == end)
            {
                *write++ = '/';
            }
        }
        else if ((remaining >= 4 && !strncmp(read, "/../", 4)) || (remaining == 3 && !strncmp(read, "/..", 3)))
        {
            read += 3;
            while (write > path && write[-1] != '/')
            {
                write--;
            }
            if (write > path)
            {
                write--;
            }
            if (read == end)
            {
                *write++ = '/';
            }
        }
        else
        {
            *write++ = *read++;
            while (read < end && *read != '/')
            {
                *write++ = *read++;
            }
        }
    }
    memmove(write, end, strlen(end) + 1);
}

/**
 * @brief Turns a page-relative picture or redirect link into a complete HTTP or HTTPS URL.
 *
 * Fragments and directory steps are removed from the completed URL.
 *
 * @param const char* base -> the absolute URL of the page containing the link
 * @param const char* reference -> the absolute or relative link to resolve
 * @param char* output -> receives the resolved URL and an ending zero byte
 * @param unsigned int capacity -> the size of output in bytes
 *
 * @return int -> 1 when the complete URL fits and passes validation, otherwise 0
 */
int url_preview__absolute_url(const char* base, const char* reference, char* output, unsigned int capacity)
{
    httpclient_url_t parsed;
    if (httpclient__parse_url(base, &parsed) != HTTPCLIENT_OK || reference == NULL || capacity == 0)
    {
        return 0;
    }
    char clean[2049];
    size_t length = strcspn(reference, "#");
    if (length >= sizeof(clean))
    {
        return 0;
    }
    _url_preview_internal__copy(clean, sizeof(clean), reference, length);
    int written;
    if (_url_preview_internal__starts_with(clean, "http://") || _url_preview_internal__starts_with(clean, "https://"))
    {
        written = snprintf(output, capacity, "%s", clean);
    }
    else if (clean[0] == '/' && clean[1] == '/')
    {
        written = snprintf(output, capacity, "%s:%s", parsed.is_https ? "https" : "http", clean);
    }
    else
    {
        // A colon before the first slash means a different URL scheme, not a
        // relative filename. Never turn file:, data: or javascript: into a fetch.
        if (strcspn(clean, ":") < strcspn(clean, "/?"))
        {
            return 0;
        }
        char* query = strchr(parsed.target, '?');
        if (query != NULL && clean[0] != 0)
        {
            *query = 0;
        }
        if (clean[0] != '/' && clean[0] != '?' && clean[0] != 0)
        {
            char* slash = strrchr(parsed.target, '/');
            slash[1] = 0;
        }
        written = snprintf(output, capacity, "%s://%s%s%s:%s%s%s", parsed.is_https ? "https" : "http",
            parsed.is_ipv6 ? "[" : "", parsed.host, parsed.is_ipv6 ? "]" : "", parsed.port,
            clean[0] == '/' ? "" : parsed.target, clean);
    }
    if (written <= 0 || (unsigned int)written >= capacity || httpclient__parse_url(output, &parsed) != HTTPCLIENT_OK)
    {
        return 0;
    }
    _url_preview_internal__remove_directory_steps(output);
    return 1;
}

/**
 * @brief Copies the value of a named HTTP response header without surrounding spaces.
 *
 * @param const char* headers -> the response status line and headers, separated by CRLF line endings
 * @param const char* name -> the header name to find, ignoring uppercase and lowercase letters
 * @param char* output -> the buffer that receives the value, or an empty string when no value fits
 * @param size_t capacity -> the size of output in bytes, which must be greater than zero
 *
 * @return int -> 1 when the header was found and its complete value fits, otherwise 0
 */
static int _url_preview_internal__header(const char* headers, const char* name, char* output, size_t capacity)
{
    output[0] = 0;
    const char* line = strstr(headers, "\r\n");
    while (line != NULL && line[2])
    {
        line += 2;
        const char* end = strstr(line, "\r\n");
        const char* colon = strchr(line, ':');
        if (end == NULL)
        {
            break;
        }
        if (colon != NULL && colon < end)
        {
            char field[128];
            _url_preview_internal__copy(field, sizeof(field), line, (size_t)(colon - line));
            if (_url_preview_internal__same(field, name))
            {
                const char* value = colon + 1;
                while (value < end && isspace((unsigned char)*value))
                {
                    value++;
                }
                while (end > value && isspace((unsigned char)end[-1]))
                {
                    end--;
                }
                if ((size_t)(end - value) >= capacity)
                {
                    return 0;
                }
                _url_preview_internal__copy(output, capacity, value, (size_t)(end - value));
                return 1;
            }
        }
        line = end;
    }
    return 0;
}

/**
 * @brief Downloads a preview page or picture, following a limited number of redirects.
 *
 * Each request checks the destination address and limits the downloaded bytes.
 * A successful result must have status 200 and must not use HTTP compression.
 *
 * @param char* url -> the URL to request, updated after redirects; its buffer must hold 2049 bytes
 * @param unsigned int limit -> the maximum response size accepted by the HTTP client
 * @param httpclient_response_t* response -> receives the response; the caller must free it even on failure
 *
 * @return int -> 1 when a usable response was received, otherwise 0
 */
static int _url_preview_internal__download(char* url, unsigned int limit, httpclient_response_t* response)
{
    const httpclient_header_t headers[] =
    {
        {"User-Agent", "LemonChat-LinkPreview/1.0"},
        {"Accept", "text/html,application/xhtml+xml,image/png,image/jpeg,image/gif,image/webp;q=0.8,*/*;q=0.1"}
    };
    for (int redirect = 0; redirect <= PREVIEW_REDIRECT_LIMIT; redirect++)
    {
        httpclient_request_t request = {0};
        request.url = url;
        request.timeout_ms = 8000;
        request.max_response_bytes = limit;
        request.public_addresses_only = 1;
        request.headers = headers;
        request.header_count = sizeof(headers) / sizeof(headers[0]);
        if (httpclient__request(&request, response) != HTTPCLIENT_OK)
        {
            return 0;
        }
        if (response->status_code == 200)
        {
            char encoding[64];
            return !_url_preview_internal__header(response->headers, "Content-Encoding", encoding, sizeof(encoding))
                || _url_preview_internal__same(encoding, "identity");
        }
        char location[2049];
        char next[2049];
        int follows = (response->status_code == 301 || response->status_code == 302 || response->status_code == 303
            || response->status_code == 307 || response->status_code == 308)
            && _url_preview_internal__header(response->headers, "Location", location, sizeof(location))
            && url_preview__absolute_url(url, location, next, sizeof(next));
        httpclient__response_free(response);
        if (!follows)
        {
            return 0;
        }
        strcpy(url, next);
    }
    return 0;
}

/**
 * @brief Downloads a website page and an optional picture to build the preview shown below a chat link.
 *
 * Downloads use public addresses and limited redirects. Supported pictures become base64 data URLs.
 *
 * @param const char* url -> the absolute website URL, limited to 2048 characters
 * @param url_preview_t* preview -> an empty preview that receives text and optional allocated image data; release it with url_preview__free()
 *
 * @return int -> 1 when a title or description was found, even if no picture was available, otherwise 0
 */
int url_preview__resolve(const char* url, url_preview_t* preview)
{
    memset(preview, 0, sizeof(*preview));
    if (url == NULL || strlen(url) > 2048)
    {
        return 0;
    }
    strcpy(preview->url, url);
    httpclient_response_t response = {0};
    int success = _url_preview_internal__download(preview->url, PREVIEW_PAGE_LIMIT, &response);
    char content_type[128];
    if (success && _url_preview_internal__header(response.headers, "Content-Type", content_type, sizeof(content_type)))
    {
        char* separator = strchr(content_type, ';');
        if (separator != NULL)
        {
            *separator = 0;
        }
        success = _url_preview_internal__same(content_type, "text/html") || _url_preview_internal__same(content_type, "application/xhtml+xml");
    }
    else
    {
        success = 0;
    }
    if (success)
    {
        url_preview__read_html((const char*)response.body, preview);
        success = preview->title[0] || preview->description[0];
    }
    httpclient__response_free(&response);
    if (!success)
    {
        return 0;
    }
    char image_url[2049];
    if (!preview->image_url[0] || !url_preview__absolute_url(preview->url, preview->image_url, image_url, sizeof(image_url)))
    {
        return 1;
    }
    if (_url_preview_internal__download(image_url, PREVIEW_IMAGE_LIMIT, &response))
    {
        const char* mime = NULL;
        const unsigned char* bytes = response.body;
        if (response.body_size >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8))
        {
            mime = "image/png";
        }
        else if (response.body_size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
        {
            mime = "image/jpeg";
        }
        else if (response.body_size >= 6 && (!memcmp(bytes, "GIF87a", 6) || !memcmp(bytes, "GIF89a", 6)))
        {
            mime = "image/gif";
        }
        else if (response.body_size >= 12 && !memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WEBP", 4))
        {
            mime = "image/webp";
        }
        if (mime != NULL)
        {
            size_t capacity = ((size_t)response.body_size + 2) / 3 * 4 + 64;
            preview->image_data = malloc(capacity);
            if (preview->image_data != NULL)
            {
                int prefix = snprintf(preview->image_data, capacity, "data:%s;base64,", mime);
                size_t written = 0;
                if (mbedtls_base64_encode((unsigned char*)preview->image_data + prefix, capacity - (size_t)prefix,
                    &written, response.body, response.body_size) != 0)
                {
                    free(preview->image_data);
                    preview->image_data = NULL;
                }
            }
        }
    }
    httpclient__response_free(&response);
    return 1;
}

/**
 * @brief Frees a URL preview picture and clears the preview for reuse.
 *
 * @param url_preview_t* preview -> an initialized preview to clear; this pointer must not be NULL
 *
 * @return void
 */
void url_preview__free(url_preview_t* preview)
{
    free(preview->image_data);
    memset(preview, 0, sizeof(*preview));
}
