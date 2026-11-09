/*
 * Copyright (C) 2016-2024  Davidson Francis <davidsondfgl@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#define _POSIX_C_SOURCE 200809L
#include <base64.h>
#include <sha1.h>
#include <ws.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ctype.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

/**
 * @dir src/
 * @brief Handshake routines directory
 *
 * @file handshake.c
 * @brief Handshake routines.
 */

/** @brief Validates the canonical Base64 encoding of a 16-byte client nonce. */
static bool valid_websocket_key(const char* key, size_t length)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (length != WS_KEY_LEN || key[22] != '=' || key[23] != '=')
    {
        return false;
    }
    for (size_t index = 0; index < 22; index++)
    {
        const char* digit = strchr(alphabet, key[index]);
        if (key[index] == '\0' || digit == NULL || (index == 21 && ((digit - alphabet) & 15) != 0))
        {
            return false;
        }
    }
    return true;
}

/** @brief Allocates the accept value for a valid key; failure leaves the output null. */
int get_handshake_accept(char *wsKey, unsigned char **dest)
{
	unsigned char hash[SHA1HashSize]; /* SHA-1 Hash.                   */
	SHA1Context ctx; /* SHA-1 Context.                */
	char *str; /* WebSocket key + magic string. */

	/* Invalid key. */
	if (!dest)
	{
		return (-1);
	}

	*dest = NULL;
    if (wsKey == NULL || !valid_websocket_key(wsKey, strlen(wsKey)))
    {
        return -1;
    }
	str = calloc(1, sizeof(char) * (WS_KEY_LEN + WS_MS_LEN + 1));
	if (!str)
	{
		return (-1);
	}

	strncpy(str, wsKey, WS_KEY_LEN);
	strcat(str, MAGIC_STRING);

	SHA1Reset(&ctx);
	SHA1Input(&ctx, (const uint8_t *)str, WS_KEYMS_LEN);
	SHA1Result(&ctx, hash);

	*dest = base64_encode(hash, SHA1HashSize, NULL);
	free(str);
	if (*dest == NULL)
	{
		return -1;
	}
	(*dest)[strcspn((const char*)*dest, "\r\n")] = '\0';
	return (0);
}

/** @brief Matches one complete header name or token without regard to case. */
static bool header_equals(const char* value, size_t length, const char* expected)
{
    return strlen(expected) == length && strncasecmp(value, expected, length) == 0;
}

static bool header_has_token(const char* value, size_t length, const char* expected)
{
    const char* end = value + length;
    while (value < end)
    {
        const char* token_end = memchr(value, ',', (size_t)(end - value));
        const char* next = token_end != NULL ? token_end + 1 : end;
        if (token_end == NULL)
        {
            token_end = end;
        }
        while (value < token_end && (*value == ' ' || *value == '\t'))
        {
            value++;
        }
        while (token_end > value && (token_end[-1] == ' ' || token_end[-1] == '\t'))
        {
            token_end--;
        }
        if (header_equals(value, (size_t)(token_end - value), expected))
        {
            return true;
        }
        value = next;
    }
    return false;
}

static bool header_name_byte(unsigned char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9') || (value != 0 && strchr("!#$%&'*+-.^_`|~", value) != NULL);
}

/**
 * @brief Validates the complete HTTP upgrade and allocates its response.
 *
 * @param hsrequest  Client request.
 * @param hsresponse Server response.
 *
 * @return Returns 0 if success and a negative number
 * otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
int get_handshake_response(char *hsrequest, char **hsresponse)
{
    unsigned char* accept = NULL;
    char key[WS_KEY_LEN + 1];
    bool have_key = false;
    bool have_host = false;
    bool have_version = false;
    bool have_upgrade = false;
    bool have_connection = false;

    if (hsresponse == NULL)
    {
        return -1;
    }
    *hsresponse = NULL;
    if (hsrequest == NULL || strncmp(hsrequest, "GET ", 4) != 0)
    {
        return -1;
    }
    const char* line = hsrequest + 4;
    const char* target_end = strchr(line, ' ');
    if (target_end == NULL || target_end == line || strncmp(target_end, " HTTP/1.1\r\n", 11) != 0)
    {
        return -1;
    }
    for (const char* position = line; position < target_end; position++)
    {
        if ((unsigned char)*position <= 32 || (unsigned char)*position >= 127 || *position == '#')
        {
            return -1;
        }
    }
    line = target_end + 11;
    while (strncmp(line, "\r\n", 2) != 0)
    {
        const char* end = strstr(line, "\r\n");
        const char* colon = end != NULL ? memchr(line, ':', (size_t)(end - line)) : NULL;
        if (colon == NULL || colon == line)
        {
            return -1;
        }
        for (const char* position = line; position < colon; position++)
        {
            if (!header_name_byte((unsigned char)*position))
            {
                return -1;
            }
        }
        const char* value = colon + 1;
        const char* value_end = end;
        for (const char* position = value; position < end; position++)
        {
            if (((unsigned char)*position < 32 && *position != '\t') || *position == 127)
            {
                return -1;
            }
        }
        while (value < value_end && (*value == ' ' || *value == '\t'))
        {
            value++;
        }
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
        {
            value_end--;
        }
        size_t name_length = (size_t)(colon - line);
        size_t value_length = (size_t)(value_end - value);
        if (header_equals(line, name_length, "Host"))
        {
            if (have_host || value_length == 0)
            {
                return -1;
            }
            for (const char* position = value; position < value_end; position++)
            {
                if ((unsigned char)*position <= 32 || (unsigned char)*position >= 127 || strchr("/@,\\?#", *position) != NULL)
                {
                    return -1;
                }
            }
            have_host = true;
        }
        else if (header_equals(line, name_length, WS_HS_REQ))
        {
            if (have_key || !valid_websocket_key(value, value_length))
            {
                return -1;
            }
            memcpy(key, value, WS_KEY_LEN);
            key[WS_KEY_LEN] = '\0';
            have_key = true;
        }
        else if (header_equals(line, name_length, "Sec-WebSocket-Version"))
        {
            if (have_version || value_length != 2 || memcmp(value, "13", 2) != 0)
            {
                return -1;
            }
            have_version = true;
        }
        else if (header_equals(line, name_length, "Upgrade"))
        {
            have_upgrade |= header_has_token(value, value_length, "websocket");
        }
        else if (header_equals(line, name_length, "Connection"))
        {
            have_connection |= header_has_token(value, value_length, "upgrade");
        }
        line = end + 2;
    }
    if (line[2] != '\0' || !have_host || !have_key || !have_version || !have_upgrade || !have_connection
        || get_handshake_accept(key, &accept) < 0)
    {
        return -1;
    }

	*hsresponse = calloc(sizeof(char) * WS_HS_ACCLEN, 1);
	if (*hsresponse == NULL)
	{
		free(accept);
		return (-1);
	}

	strcpy(*hsresponse, WS_HS_ACCEPT);
	strcat(*hsresponse, (const char *)accept);
	strcat(*hsresponse, "\r\n\r\n");

	free(accept);
	return (0);
}

/**
 * @brief Reads one IP address from the selected proxy header.
 * @param request Received HTTP request bytes.
 * @param request_length Number of available request bytes.
 * @param header_name Proxy header to read without regard to letter case.
 * @param address Receives the normalized address.
 * @param address_size Available bytes in the output buffer.
 * @return One if found, zero if absent, or negative one if invalid.
 */
int get_forwarded_ip_address(const char* request, size_t request_length,
    const char* header_name, char* address, size_t address_size)
{
    const char* position;
    const char* end;
    const char* line_end;
    const char* colon;
    const char* value_start;
    const char* value_end;
    size_t name_length;
    size_t index;
    size_t value_length;
    unsigned char actual;
    unsigned char expected;
    char value[INET6_ADDRSTRLEN];
    char normalized[INET6_ADDRSTRLEN];
    struct in6_addr binary_address;
    int family;
    bool is_found = false;
    bool is_complete = false;

    if (request == NULL || header_name == NULL || address == NULL || address_size == 0)
    {
        return -1;
    }
    address[0] = '\0';
    name_length = strlen(header_name);
    if (name_length == 0)
    {
        return -1;
    }
    end = request + request_length;
    line_end = memchr(request, '\n', request_length);
    if (line_end == NULL || line_end == request || line_end[-1] != '\r'
        || memchr(request, '\0', (size_t)(line_end - request)) != NULL)
    {
        return -1;
    }
    position = line_end + 1;
    while (position < end)
    {
        line_end = memchr(position, '\n', (size_t)(end - position));
        if (line_end == NULL || line_end == position || line_end[-1] != '\r'
            || memchr(position, '\0', (size_t)(line_end - position)) != NULL)
        {
            return -1;
        }
        if (line_end == position + 1)
        {
            is_complete = true;
            break;
        }
        if (*position == ' ' || *position == '\t')
        {
            return -1;
        }
        colon = memchr(position, ':', (size_t)(line_end - position));
        if (colon != NULL && (size_t)(colon - position) == name_length)
        {
            for (index = 0; index < name_length; index++)
            {
                actual = (unsigned char)position[index];
                expected = (unsigned char)header_name[index];
                if (actual >= 'A' && actual <= 'Z')
                {
                    actual += 'a' - 'A';
                }
                if (expected >= 'A' && expected <= 'Z')
                {
                    expected += 'a' - 'A';
                }
                if (actual != expected)
                {
                    break;
                }
            }
            if (index == name_length)
            {
                if (is_found)
                {
                    return -1;
                }
                value_start = colon + 1;
                value_end = line_end - 1;
                while (value_start < value_end && (*value_start == ' ' || *value_start == '\t'))
                {
                    value_start++;
                }
                while (value_end > value_start && (value_end[-1] == ' ' || value_end[-1] == '\t'))
                {
                    value_end--;
                }
                value_length = (size_t)(value_end - value_start);
                if (value_length == 0 || value_length >= sizeof(value))
                {
                    return -1;
                }
                memcpy(value, value_start, value_length);
                value[value_length] = '\0';
                is_found = true;
            }
        }
        position = line_end + 1;
    }
    if (!is_complete)
    {
        return -1;
    }
    if (!is_found)
    {
        return 0;
    }
    family = AF_INET;
    if (inet_pton(family, value, &binary_address) != 1)
    {
        family = AF_INET6;
        if (inet_pton(family, value, &binary_address) != 1)
        {
            return -1;
        }
    }
    if (inet_ntop(family, &binary_address, normalized, sizeof(normalized)) == NULL)
    {
        return -1;
    }
    value_length = strlen(normalized);
    if (value_length >= address_size)
    {
        return -1;
    }
    memcpy(address, normalized, value_length + 1);
    return 1;
}
