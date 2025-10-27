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
	SHA1Context ctx;                  /* SHA-1 Context.                */
	char *str;                        /* WebSocket key + magic string. */

	/* Invalid key. */
	if (!wsKey)
		return (-1);

	str = calloc(1, sizeof(char) * (WS_KEY_LEN + WS_MS_LEN + 1));
	if (!str)
		return (-1);

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
		if (!strncasecmp(haystack, needle, length))
			return (char*)haystack;
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
	char *saveptr;         /* strtok_r() pointer. */
	char *s;               /* Current string.     */
	int ret;               /* Return value.       */

	saveptr = NULL;
	for (s = strtok_r(hsrequest, "\r\n", &saveptr); s != NULL;
		 s = strtok_r(NULL, "\r\n", &saveptr))
	{
		if (strstricase(s, WS_HS_REQ) != NULL)
			break;
	}

	/* Ensure that we have a valid pointer. */
	if (s == NULL)
		return (-1);

	saveptr = NULL;
	s       = strtok_r(s, " ", &saveptr);
	s       = strtok_r(NULL, " ", &saveptr);

	ret = get_handshake_accept(s, &accept);
	if (ret < 0)
		return (ret);

	*hsresponse = malloc(sizeof(char) * WS_HS_ACCLEN);
	if (*hsresponse == NULL)
		return (-1);

	strcpy(*hsresponse, WS_HS_ACCEPT);
	strcat(*hsresponse, (const char *)accept);
	strcat(*hsresponse, "\r\n\r\n");

	free(accept);
	return (0);
}


int is_valid_ipv4_char(char c) {
    return (isdigit((unsigned char)c) || c == '.');
}

char* find_header_value(char* http_response_headers, char* header_name)
{
    char* position = http_response_headers;
    size_t header_name_length = strlen(header_name);

    while (*position != '\0')
    {
        if (strncmp(position, header_name, header_name_length) == 0 && position[header_name_length] == ':')
        {
            char* value_start = position + header_name_length + 1;

            // Skip leading spaces or tabs
            while (*value_start == ' ' || *value_start == '\t')
            {
                value_start++;
            }

            // Find end of line or string
            char* line_end = strchr(value_start, '\n');
            if (line_end == NULL)
            {
                line_end = value_start + strlen(value_start);
            }

            // Trim trailing whitespace (space, tab, \r, \n)
            char* value_end = line_end;
            while (value_end > value_start && 
                  (*(value_end - 1) == ' ' || *(value_end - 1) == '\t' || *(value_end - 1) == '\r' || *(value_end - 1) == '\n'))
            {
                value_end--;
            }

            size_t value_length = value_end - value_start;

            // Allocate buffer and copy value
            char* value_copy = (char*)malloc(value_length + 1);
            if (value_copy == NULL)
            {
                return NULL;
            }

            memcpy(value_copy, value_start, value_length);
            value_copy[value_length] = '\0';

            return value_copy;
        }

        char* next_line = strchr(position, '\n');
        if (next_line == NULL)
        {
            break;
        }

        position = next_line + 1;
    }

    return NULL;
}

char* get_forwarded_ip_address(char *hsrequest)
{
    char* ip_str = find_header_value(hsrequest, "X-Forwarded-For");
    if (ip_str == NULL)
    {
     	DEBUG_THELDUS_WEBSOCKET printf("%s","failed to find header \n");
     	return NULL;
    }
    else
    {
		DEBUG_THELDUS_WEBSOCKET printf("%s %d %s", "ip_str length is -> ", strlen(ip_str), "\n");
		DEBUG_THELDUS_WEBSOCKET printf("%s%s%s","header found[", ip_str, "] \n");	
    }
	
    return ip_str;
}
