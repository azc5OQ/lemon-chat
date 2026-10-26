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

/**
 * @brief Gets the field Sec-WebSocket-Accept on response, by
 * an previously informed key.
 *
 * @param wsKey Sec-WebSocket-Key
 * @param dest source to be stored the value.
 *
 * @return Returns 0 if success and a negative number
 * otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
int get_handshake_accept(char *wsKey, unsigned char **dest)
{
	unsigned char hash[SHA1HashSize]; /* SHA-1 Hash.                   */
	SHA1Context ctx; /* SHA-1 Context.                */
	char *str; /* WebSocket key + magic string. */

	/* Invalid key. */
	if (!wsKey)
	{
		return (-1);
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
	*(*dest + strlen((const char *)*dest) - 1) = '\0';
	free(str);
	return (0);
}

/**
 * @brief Finds the ocorrence of @p needle in @p haystack, case
 * insensitive.
 *
 * @param haystack Target string to be searched.
 * @param needle   Substring to search for.
 *
 * @returns If found, returns a pointer at the beginning of the
 * found substring. Otherwise, returns NULL.
 */
static char *strstricase(const char *haystack, const char *needle)
{
	size_t length;
	for (length = strlen(needle); *haystack; haystack++)
	{
		if (!strncasecmp(haystack, needle, length))
		{
			return (char *)haystack;
		}
	}
	return (NULL);
}

/**
 * @brief Gets the complete response to accomplish a succesfully
 * handshake.
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

/**
 * @brief Gets the complete response to accomplish a succesfully
 * handshake.
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
	unsigned char *accept; /* Accept message.     */
	char *saveptr; /* strtok_r() pointer. */
	char *s; /* Current string.     */
	int ret; /* Return value.       */

	saveptr = NULL;
	for (s = strtok_r(hsrequest, "\r\n", &saveptr); s != NULL; s = strtok_r(NULL, "\r\n", &saveptr))
	{
		if (strstricase(s, WS_HS_REQ) != NULL)
		{
			break;
		}
	}

	/* Ensure that we have a valid pointer. */
	if (s == NULL)
	{
		return (-1);
	}

	saveptr = NULL;
	s = strtok_r(s, " ", &saveptr);
	s = strtok_r(NULL, " ", &saveptr);

	ret = get_handshake_accept(s, &accept);
	if (ret < 0)
	{
		return (ret);
	}

	*hsresponse = calloc(sizeof(char) * WS_HS_ACCLEN, 1);
	if (*hsresponse == NULL)
	{
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
