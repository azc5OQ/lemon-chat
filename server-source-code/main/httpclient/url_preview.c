#include "../clib/clib.h"
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
static boole _url_preview_internal__same(const char *left, const char *right);
static boole _url_preview_internal__starts_with(const char *text, const char *prefix);
static const char *_url_preview_internal__find(const char *text, const char *wanted);
static void _url_preview_internal__copy(char *output, uint64 capacity, const char *text, uint64 length);
static void _url_preview_internal__text(const char *source, uint64 length, char *output, uint64 capacity);
static void _url_preview_internal__choose(char *output, uint64 capacity, const char *value, int64 priority, int64 *previous);
static void _url_preview_internal__remove_directory_steps(char *url);
static boole _url_preview_internal__header(const char *headers, const char *name, char *output, uint64 capacity);
static boole _url_preview_internal__download(char *url, uint64 url_capacity, uint64 limit, httpclient_response_t *response);

/**
 * @brief Checks whether two strings contain the same text, ignoring uppercase and lowercase
 *        letters.
 *
 * @param const char* left -> the first string, ending with a zero byte
 * @param const char* right -> the second string, ending with a zero byte
 *
 * @return boole -> TRUE when both strings match, otherwise FALSE
 */
static boole _url_preview_internal__same(const char *left, const char *right)
{
    // compare the letters
    while (*left && *right)
    {
        if (clib__tolower((ubyte)*left++) != clib__tolower((ubyte)*right++))
        {
            return 0;
        }
    }
    return *left == *right;
}

/**
 * @brief Checks whether a string starts with the requested text, ignoring uppercase and lowercase
 *        letters.
 *
 * @param const char* text -> the string to check, ending with a zero byte
 * @param const char* prefix -> the text that must appear at the start
 *
 * @return boole -> TRUE when the prefix matches, otherwise FALSE
 */
static boole _url_preview_internal__starts_with(const char *text, const char *prefix)
{
    // compare the prefix
    while (*prefix)
    {
        if (clib__tolower((ubyte)*text++) != clib__tolower((ubyte)*prefix++))
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
static const char *_url_preview_internal__find(const char *text, const char *wanted)
{
    uint64 index = 0;
    uint64 size = clib__strlen(wanted);

    // try every position
    for (; *text; text++)
    {
        index = 0;
        while (index < size && text[index] && clib__tolower((ubyte)text[index]) == clib__tolower((ubyte)wanted[index]))
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
 * @param uint64 capacity -> the size of output in bytes, which must be greater than zero
 * @param const char* text -> the source bytes to copy
 * @param uint64 length -> the number of source bytes to copy, shortened when output is too small
 *
 * @return void
 */
static void _url_preview_internal__copy(char *output, uint64 capacity, const char *text, uint64 length)
{
    // limit the length
    if (length >= capacity)
    {
        length = capacity - 1;
    }

    // copy the text
    clib__memcpy(output, text, length);
    output[length] = 0;
}

/**
 * @brief Turns text read from HTML into the readable text shown in a URL preview.
 *
 * It reads HTML character references such as &amp; and &#233;, keeps valid UTF-8
 * characters whole when shortening the text, and turns line breaks into spaces.
 *
 * @param const char* source -> the HTML text to read
 * @param uint64 length -> the maximum number of source bytes to read
 * @param char* output -> the buffer that receives the cleaned text and an ending zero byte
 * @param uint64 capacity -> the size of output in bytes, which must be greater than zero
 *
 * @return void
 */
static void _url_preview_internal__text(const char *source, uint64 length, char *output, uint64 capacity)
{
    uint64 index = 0;
    uint64 code = 0;
    uint64 consumed = 0;
    uint64 end = 0;
    char entity[17] = { 0 };
    uint64 entity_code = 0;
    char *tail = NULL_POINTER;
    boole hexadecimal = FALSE;
    const char *digits = NULL_POINTER;
    uint64 input_count = 0;
    uint64 decoded = 0;
    boole valid = FALSE;
    uint64 part = 0;
    ubyte byte = 0;
    ubyte bytes[4] = { 0 };
    uint64 count = 0;
    uint64 written = 0;

    // convert every character
    for (index = 0; index < length && source[index];)
    {
        // read one byte
        code = (ubyte)source[index];
        consumed = 1;

        // decode HTML entities
        if (code == '&')
        {
            // find the semicolon
            end = index + 1;
            while (end < length && end - index < 16 && source[end] && source[end] != ';')
            {
                end++;
            }
            if (end < length && source[end] == ';')
            {
                // copy entity name
                clib__memset(entity, 0, sizeof(entity));
                _url_preview_internal__copy(entity, sizeof(entity), source + index + 1, end - index - 1);
                entity_code = 0;

                // find character code
                if (entity[0] == '#')
                {
                    // read numeric entity
                    tail = NULL;
                    hexadecimal = entity[1] == 'x' || entity[1] == 'X';
                    digits = entity + (hexadecimal ? 2 : 1);
                    entity_code = clib__strtoul(digits, &tail, hexadecimal ? 16 : 10);
                    if (tail == digits || *tail != 0)
                    {
                        entity_code = 0;
                    }
                }
                else if (!clib__strcmp(entity, "amp"))
                {
                    entity_code = '&';
                }
                else if (!clib__strcmp(entity, "lt"))
                {
                    entity_code = '<';
                }
                else if (!clib__strcmp(entity, "gt"))
                {
                    entity_code = '>';
                }
                else if (!clib__strcmp(entity, "quot"))
                {
                    entity_code = '"';
                }
                else if (!clib__strcmp(entity, "apos") || !clib__strcmp(entity, "#39"))
                {
                    entity_code = '\'';
                }
                else if (!clib__strcmp(entity, "nbsp"))
                {
                    entity_code = ' ';
                }

                // use valid code
                if (entity_code > 0 && entity_code <= 0x10ffff && !(entity_code >= 0xd800 && entity_code <= 0xdfff))
                {
                    code = (uint64)entity_code;
                    consumed = end - index + 1;
                }
            }
        }
        else if (code >= 128)
        {
            // validate UTF-8
            input_count = code >= 0xc2 && code <= 0xdf ? 2 : code >= 0xe0 && code <= 0xef ? 3 : code >= 0xf0 && code <= 0xf4 ? 4 : 0;
            decoded = input_count == 2 ? code & 31 : input_count == 3 ? code & 15 : code & 7;
            valid = input_count != 0 && index + input_count <= length;

            // read following bytes
            for (part = 1; valid && part < input_count; part++)
            {
                byte = (ubyte)source[index + part];
                valid = (byte & 0xc0) == 0x80;
                decoded = (decoded << 6) | (byte & 63);
            }

            // check code range
            valid = valid && decoded >= (input_count == 2 ? 0x80u : input_count == 3 ? 0x800u : 0x10000u) && decoded <= 0x10ffff && !(decoded >= 0xd800 && decoded <= 0xdfff);
            code = valid ? decoded : 0xfffd;
            consumed = valid ? input_count : 1;
        }

        // go to next character
        index += consumed;

        // combine whitespace
        if (code <= 32 || code == 127 || code == 160)
        {
            if (written == 0 || output[written - 1] == ' ')
            {
                continue;
            }
            code = ' ';
        }

        // encode UTF-8
        clib__memset(bytes, 0, sizeof(bytes));
        count = 1;
        if (code < 128)
        {
            bytes[0] = (ubyte)code;
        }
        else if (code < 2048)
        {
            count = 2;
            bytes[0] = (ubyte)(0xc0 | (code >> 6));
            bytes[1] = (ubyte)(0x80 | (code & 63));
        }
        else if (code < 65536)
        {
            count = 3;
            bytes[0] = (ubyte)(0xe0 | (code >> 12));
            bytes[1] = (ubyte)(0x80 | ((code >> 6) & 63));
            bytes[2] = (ubyte)(0x80 | (code & 63));
        }
        else
        {
            count = 4;
            bytes[0] = (ubyte)(0xf0 | (code >> 18));
            bytes[1] = (ubyte)(0x80 | ((code >> 12) & 63));
            bytes[2] = (ubyte)(0x80 | ((code >> 6) & 63));
            bytes[3] = (ubyte)(0x80 | (code & 63));
        }

        // fit whole character
        if (written + count >= capacity)
        {
            break;
        }

        // write the character
        clib__memcpy(output + written, bytes, count);
        written += count;
    }

    // remove last space
    if (written && output[written - 1] == ' ')
    {
        written--;
    }

    // write ending zero
    output[written] = 0;
}

/**
 * @brief Replaces a preview field when a new nonempty value has a higher priority than the current
 *        one.
 *
 * @param char* output -> the preview field to update with cleaned HTML text
 * @param uint64 capacity -> the size of output in bytes, which must be greater than zero
 * @param const char* value -> the new value found in the page
 * @param int64 priority -> the priority of the new value, with larger numbers preferred
 * @param int64* previous -> the current priority, updated when a nonempty value is accepted
 *
 * @return void
 */
static void _url_preview_internal__choose(char *output, uint64 capacity, const char *value, int64 priority, int64 *previous)
{
    // accept higher priority
    if (value[0] && priority > *previous)
    {
        _url_preview_internal__text(value, clib__strlen(value), output, capacity);
        if (output[0])
        {
            *previous = priority;
        }
    }
}

/**
 * @brief Reads a downloaded page for the title, description, site name and picture used in a URL
 *        preview.
 *
 * Scripts are skipped. Open Graph values take priority over Twitter and ordinary tags.
 *
 * @param const char* html -> the HTML text, ending with a zero byte
 * @param url_preview_t* preview -> the initialized preview whose text and image URL fields are filled
 *
 * @return void
 */
void url_preview__read_html(const char *html, url_preview_t *preview)
{
    const char *name_start = NULL_POINTER;
    char tag[32] = { 0 };
    char name[128] = { 0 };
    char property[128] = { 0 };
    char content[4097] = { 0 };
    const char *attribute_start = NULL_POINTER;
    char attribute[64] = { 0 };
    char quote = 0;
    const char *value = NULL_POINTER;
    uint64 length = 0;
    const char *end = NULL_POINTER;
    const char *key = NULL_POINTER;
    int64 description_rank = 0;
    int64 priority = 0;
    int64 title_priority = 0;
    int64 description_priority = 0;
    int64 image_priority = 0;
    const char *cursor = html;

    // find HTML tags
    while ((cursor = clib__strchr(cursor, '<')) != NULL)
    {
        // skip HTML comments
        if (!clib__strncmp(cursor, "<!--", 4))
        {
            cursor = clib__strstr(cursor + 4, "-->");
            if (cursor == NULL)
            {
                break;
            }
            cursor += 3;
            continue;
        }

        // check closing tags
        cursor++;
        if (*cursor == '/')
        {
            if (_url_preview_internal__starts_with(cursor, "/head"))
            {
                break;
            }
            continue;
        }

        // read tag name
        name_start = cursor;
        while (clib__isalnum((ubyte)*cursor))
        {
            cursor++;
        }
        clib__memset(tag, 0, sizeof(tag));
        _url_preview_internal__copy(tag, sizeof(tag), name_start, (uint64)(cursor - name_start));

        // clear attribute values
        clib__memset(name, 0, sizeof(name));
        clib__memset(property, 0, sizeof(property));
        clib__memset(content, 0, sizeof(content));

        // attribute values can contain '>', and attributes may arrive in any
        // order. read each quoted value before looking for the next attribute
        while (*cursor && *cursor != '>')
        {
            // skip the spaces
            while (clib__isspace((ubyte)*cursor) || *cursor == '/')
            {
                cursor++;
            }
            if (!*cursor || *cursor == '>')
            {
                break;
            }

            // read attribute name
            attribute_start = cursor;
            while (*cursor && !clib__isspace((ubyte)*cursor) && *cursor != '=' && *cursor != '>')
            {
                cursor++;
            }
            clib__memset(attribute, 0, sizeof(attribute));
            _url_preview_internal__copy(attribute, sizeof(attribute), attribute_start, (uint64)(cursor - attribute_start));

            // find the equals sign
            while (clib__isspace((ubyte)*cursor))
            {
                cursor++;
            }
            if (*cursor != '=')
            {
                continue;
            }
            cursor++;
            while (clib__isspace((ubyte)*cursor))
            {
                cursor++;
            }

            // read attribute value
            quote = *cursor == '"' || *cursor == '\'' ? *cursor++ : 0;
            value = cursor;
            while (*cursor && (quote ? *cursor != quote : !clib__isspace((ubyte)*cursor) && *cursor != '>'))
            {
                cursor++;
            }
            length = (uint64)(cursor - value);

            // store known attributes
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

            // skip closing quote
            if (quote && *cursor == quote)
            {
                cursor++;
            }
        }

        // check for text end
        if (!*cursor)
        {
            break;
        }
        cursor++;

        // skip scripts
        if (_url_preview_internal__same(tag, "script") || _url_preview_internal__same(tag, "style"))
        {
            cursor = _url_preview_internal__find(cursor, _url_preview_internal__same(tag, "script") ? "</script" : "</style");
            if (cursor == NULL)
            {
                break;
            }
            continue;
        }

        // read page title
        if (_url_preview_internal__same(tag, "title"))
        {
            end = _url_preview_internal__find(cursor, "</title");
            if (end == NULL)
            {
                // an unclosed title consumes the remaining text. do not search
                // the same suffix again for every nested opening title tag
                break;
            }

            // store title if none
            if (title_priority == 0)
            {
                _url_preview_internal__text(cursor, (uint64)(end - cursor), preview->title, sizeof(preview->title));
                title_priority = preview->title[0] ? 1 : 0;
            }
            cursor = end;
        }
        else if (_url_preview_internal__same(tag, "meta"))
        {
            // choose preview fields
            key = property[0] ? property : name;
            if (_url_preview_internal__same(key, "og:title") || _url_preview_internal__same(key, "twitter:title"))
            {
                _url_preview_internal__choose(preview->title, sizeof(preview->title), content, _url_preview_internal__same(key, "og:title") ? 3 : 2, &title_priority);
            }
            else if (_url_preview_internal__same(key, "og:description") || _url_preview_internal__same(key, "twitter:description") || _url_preview_internal__same(key, "description"))
            {
                description_rank = _url_preview_internal__same(key, "og:description") ? 3 : _url_preview_internal__same(key, "twitter:description") ? 2 : 1;
                _url_preview_internal__choose(preview->description, sizeof(preview->description), content, description_rank, &description_priority);
            }
            else if (_url_preview_internal__same(key, "og:image") || _url_preview_internal__same(key, "og:image:secure_url") || _url_preview_internal__same(key, "twitter:image"))
            {
                priority = _url_preview_internal__same(key, "og:image:secure_url") ? 4 : _url_preview_internal__same(key, "og:image") ? 3 : 2;
                _url_preview_internal__choose(preview->image_url, sizeof(preview->image_url), content, priority, &image_priority);
            }
            else if (_url_preview_internal__same(key, "og:site_name"))
            {
                _url_preview_internal__text(content, clib__strlen(content), preview->site_name, sizeof(preview->site_name));
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
static void _url_preview_internal__remove_directory_steps(char *url)
{
    char *end = NULL_POINTER;
    char *read = NULL_POINTER;
    char *write = NULL_POINTER;
    uint64 remaining = 0;
    char *authority = (char *)clib__strstr(url, "://") + 3;
    char *path = (char *)clib__strchr(authority, '/');
    char *query = (char *)clib__strchr(authority, '?');

    // no path to change
    if (path == NULL || (query != NULL && query < path))
    {
        return;
    }

    // find path end
    end = path + strcspn(path, "?");
    read = path;
    write = path;

    // resolve directory steps
    while (read < end)
    {
        remaining = (uint64)(end - read);
        if ((remaining >= 3 && !clib__strncmp(read, "/./", 3)) || (remaining == 2 && !clib__strncmp(read, "/.", 2)))
        {
            // skip current directory
            read += 2;
            if (read == end)
            {
                *write++ = '/';
            }
        }
        else if ((remaining >= 4 && !clib__strncmp(read, "/../", 4)) || (remaining == 3 && !clib__strncmp(read, "/..", 3)))
        {
            // remove previous directory
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
            // copy path part
            *write++ = *read++;
            while (read < end && *read != '/')
            {
                *write++ = *read++;
            }
        }
    }

    // keep query unchanged
    clib__memmove(write, end, clib__strlen(end) + 1);
}

/**
 * @brief Turns a page-relative picture or redirect link into a complete HTTP or HTTPS URL.
 *
 * Fragments and directory steps are removed from the completed URL.
 *
 * @param const char* base -> the absolute URL of the page containing the link
 * @param const char* reference -> the absolute or relative link to resolve
 * @param char* output -> receives the resolved URL and an ending zero byte
 * @param uint64 capacity -> the size of output in bytes
 *
 * @return boole -> TRUE when the complete URL fits and passes validation, otherwise FALSE
 */
boole url_preview__absolute_url(const char *base, const char *reference, char *output, uint64 capacity)
{
    char clean[2049] = { 0 };
    uint64 length = 0;
    int64 written = 0;
    char *query = NULL_POINTER;
    char *slash = NULL_POINTER;
    httpclient_url_t parsed;

    // check arguments
    if (httpclient__parse_url(base, &parsed) != HTTPCLIENT_OK || reference == NULL || capacity == 0 || capacity > SIZE_MAX)
    {
        return 0;
    }

    // remove URL fragment
    length = strcspn(reference, "#");
    if (length >= sizeof(clean))
    {
        return 0;
    }
    _url_preview_internal__copy(clean, sizeof(clean), reference, length);

    // resolve relative link
    if (_url_preview_internal__starts_with(clean, "http://") || _url_preview_internal__starts_with(clean, "https://"))
    {
        written = clib__string_format(output, capacity, "%s", clean);
    }
    else if (clean[0] == '/' && clean[1] == '/')
    {
        written = clib__string_format(output, capacity, "%s:%s", parsed.is_https ? "https" : "http", clean);
    }
    else
    {
        // a colon before the first slash means a different URL scheme, not a
        // relative filename. never turn file:, data: or javascript: into a fetch
        if (strcspn(clean, ":") < strcspn(clean, "/?"))
        {
            return 0;
        }

        // remove base query
        query = (char *)clib__strchr(parsed.target, '?');
        if (query != NULL && clean[0] != 0)
        {
            *query = 0;
        }

        // keep base directory
        if (clean[0] != '/' && clean[0] != '?' && clean[0] != 0)
        {
            slash = (char *)clib__strrchr(parsed.target, '/');
            slash[1] = 0;
        }

        // build complete URL
        written = clib__string_format(output, capacity, "%s://%s%s%s:%s%s%s", parsed.is_https ? "https" : "http", parsed.is_ipv6 ? "[" : "", parsed.host, parsed.is_ipv6 ? "]" : "", parsed.port, clean[0] == '/' ? "" : parsed.target, clean);
    }

    // validate complete URL
    if (written <= 0 || (uint64)written >= capacity || httpclient__parse_url(output, &parsed) != HTTPCLIENT_OK)
    {
        return 0;
    }

    // resolve directory steps
    _url_preview_internal__remove_directory_steps(output);
    return 1;
}

/**
 * @brief Copies the value of a named HTTP response header without surrounding spaces.
 *
 * @param const char* headers -> the response status line and headers, separated by CRLF line endings
 * @param const char* name -> the header name to find, ignoring uppercase and lowercase letters
 * @param char* output -> the buffer that receives the value, or an empty string when no value fits
 * @param uint64 capacity -> the size of output in bytes, which must be greater than zero
 *
 * @return boole -> TRUE when the header was found and its complete value fits, otherwise FALSE
 */
static boole _url_preview_internal__header(const char *headers, const char *name, char *output, uint64 capacity)
{
    const char *line = NULL_POINTER;
    const char *end = NULL_POINTER;
    const char *colon = NULL_POINTER;
    char field[128] = { 0 };
    const char *value = NULL_POINTER;

    // clear the output
    output[0] = 0;

    // skip status line
    line = clib__strstr(headers, "\r\n");

    // check every header line
    while (line != NULL && line[2])
    {
        // find line end
        line += 2;
        end = clib__strstr(line, "\r\n");
        colon = clib__strchr(line, ':');
        if (end == NULL)
        {
            break;
        }
        if (colon != NULL && colon < end)
        {
            // copy header name
            clib__memset(field, 0, sizeof(field));
            _url_preview_internal__copy(field, sizeof(field), line, (uint64)(colon - line));

            // match header name
            if (_url_preview_internal__same(field, name))
            {
                // trim surrounding spaces
                value = colon + 1;
                while (value < end && clib__isspace((ubyte)*value))
                {
                    value++;
                }
                while (end > value && clib__isspace((ubyte)end[-1]))
                {
                    end--;
                }

                // refuse long value
                if ((uint64)(end - value) >= capacity)
                {
                    return 0;
                }

                // copy the value
                _url_preview_internal__copy(output, capacity, value, (uint64)(end - value));
                return 1;
            }
        }

        // go to next line
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
 * @param uint64 limit -> the maximum response size accepted by the HTTP client
 * @param httpclient_response_t* response -> receives the response; the caller must free it even on failure
 *
 * @return boole -> TRUE when a usable response was received, otherwise FALSE
 */
static boole _url_preview_internal__download(char *url, uint64 url_capacity, uint64 limit, httpclient_response_t *response)
{
    uint64 redirect = 0;
    httpclient_request_t request = { 0 };
    char encoding[64] = { 0 };
    char location[2049] = { 0 };
    char next[2049] = { 0 };
    boole follows = FALSE;
    const httpclient_header_t headers[] = { { "User-Agent", "LemonChat-LinkPreview/1.0" }, { "Accept", "text/html,application/xhtml+xml,image/png,image/jpeg,image/gif,image/webp;q=0.8,*/*;q=0.1" } };

    // limit redirect count
    for (redirect = 0; redirect <= PREVIEW_REDIRECT_LIMIT; redirect++)
    {
        // set download limits
        request = (httpclient_request_t){ 0 };
        request.url = url;
        request.timeout_ms = 8000;
        request.max_response_bytes = limit;
        request.public_addresses_only = 1;
        request.headers = headers;
        request.header_count = sizeof(headers) / sizeof(headers[0]);

        // send the request
        if (httpclient__request(&request, response) != HTTPCLIENT_OK)
        {
            return 0;
        }

        // check response encoding
        if (response->status_code == 200)
        {
            clib__memset(encoding, 0, sizeof(encoding));
            return !_url_preview_internal__header(response->headers, "Content-Encoding", encoding, sizeof(encoding)) || _url_preview_internal__same(encoding, "identity");
        }

        // resolve redirect target
        clib__memset(location, 0, sizeof(location));
        clib__memset(next, 0, sizeof(next));
        follows = (response->status_code == 301 || response->status_code == 302 || response->status_code == 303 || response->status_code == 307 || response->status_code == 308) && _url_preview_internal__header(response->headers, "Location", location, sizeof(location)) && url_preview__absolute_url(url, location, next, sizeof(next));

        // release previous response
        httpclient__response_free(response);

        // no redirect to follow
        if (!follows)
        {
            return 0;
        }

        // use the new URL
        clib__string_copy(url, url_capacity, next);
    }

    // too many redirects
    return 0;
}

/**
 * @brief Downloads a website page and an optional picture to build the preview shown below a chat
 *        link.
 *
 * Downloads use public addresses and limited redirects. Supported pictures become base64 data URLs.
 *
 * @param const char* url -> the absolute website URL, limited to 2048 characters
 * @param url_preview_t* preview -> an empty preview that receives text and optional allocated image data; release it with url_preview__free()
 *
 * @return boole -> TRUE when a title or description was found, even if no picture was available, otherwise FALSE
 */
boole url_preview__resolve(const char *url, url_preview_t *preview)
{
    httpclient_response_t response = { 0 };
    boole success = FALSE;
    char content_type[128] = { 0 };
    char *separator = NULL_POINTER;
    char image_url[2049] = { 0 };
    const char *mime = NULL_POINTER;
    const ubyte *bytes = NULL_POINTER;
    uint64 capacity = 0;
    int64 prefix = 0;
    size_t written = 0; // mbedTLS requires size_t*

    // clear the preview
    clib__memset(preview, 0, sizeof(*preview));

    // check the URL
    if (url == NULL || clib__strlen(url) > 2048)
    {
        return 0;
    }

    // copy the URL
    clib__string_copy(preview->url, sizeof(preview->url), url);

    // download preview page
    success = _url_preview_internal__download(preview->url, sizeof(preview->url), PREVIEW_PAGE_LIMIT, &response);

    // check HTML type
    if (success && _url_preview_internal__header(response.headers, "Content-Type", content_type, sizeof(content_type)))
    {
        separator = (char *)clib__strchr(content_type, ';');
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
        // extract preview fields
        url_preview__read_html((const char *)response.body, preview);
        success = preview->title[0] || preview->description[0];
    }

    // free the response
    httpclient__response_free(&response);

    // preview has no text
    if (!success)
    {
        return 0;
    }

    // no usable picture URL
    if (!preview->image_url[0] || !url_preview__absolute_url(preview->url, preview->image_url, image_url, sizeof(image_url)))
    {
        return 1;
    }

    // download preview image
    if (_url_preview_internal__download(image_url, sizeof(image_url), PREVIEW_IMAGE_LIMIT, &response))
    {
        mime = NULL;
        bytes = response.body;

        // check image signature
        if (response.body_size >= 8 && !clib__memcmp(bytes, "\x89PNG\r\n\x1a\n", 8))
        {
            mime = "image/png";
        }
        else if (response.body_size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
        {
            mime = "image/jpeg";
        }
        else if (response.body_size >= 6 && (!clib__memcmp(bytes, "GIF87a", 6) || !clib__memcmp(bytes, "GIF89a", 6)))
        {
            mime = "image/gif";
        }
        else if (response.body_size >= 12 && !clib__memcmp(bytes, "RIFF", 4) && !clib__memcmp(bytes + 8, "WEBP", 4))
        {
            mime = "image/webp";
        }
        if (mime != NULL)
        {
            // encode image data
            capacity = ((uint64)response.body_size + 2) / 3 * 4 + 64;
            preview->image_data = malloc(capacity);
            if (preview->image_data != NULL)
            {
                prefix = clib__string_format(preview->image_data, capacity, "data:%s;base64,", mime);
                written = 0;
                if (mbedtls_base64_encode((ubyte *)preview->image_data + prefix, capacity - (uint64)prefix, &written, response.body, response.body_size) != 0)
                {
                    free(preview->image_data);
                    preview->image_data = NULL;
                }
            }
        }
    }

    // free the response
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
void url_preview__free(url_preview_t *preview)
{
    free(preview->image_data);
    clib__memset(preview, 0, sizeof(*preview));
}
