#include "../libmaxminddb-1.12.2/include/maxminddb.h"
#include "ip_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/timeb.h>

#include <pthread.h>

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "dave-g-json/cJSON.h"

/* @brief self explanatory
 *
 * @param StringBuffer* sb
 * @param const char *str
 *
 * @return void
 * */
void append_to_string_buffer(StringBuffer *sb, const char *str)
{
	size_t str_len = strlen(str);
	size_t new_length = sb->length + str_len;

	// Resize the buffer if necessary
	if (new_length >= sb->capacity)
	{
		sb->capacity = new_length * 2; // Double the capacity
		sb->buffer = (char *)realloc(sb->buffer, sb->capacity);
		if (!sb->buffer)
		{
			// Handle memory allocation failure
			DBG_IP_TOOLS printf("Memory allocation failed \n");
			exit(1);
		}
	}

	// Append the string to the buffer
	strcat(sb->buffer, str);
	sb->length = new_length;
}

/* @brief self explanatory
 *
 * @param StringBuffer* sb
 *
 * @return const char *
 * */
const char *get_string_from_buffer(const StringBuffer *sb)
{
	return sb->buffer;
}

/* @brief Clean up the StringBuffer
 *
 * @param StringBuffer* sb
 *
 * @return void
 * */
void free_string_buffer(StringBuffer *sb)
{
	free(sb->buffer);
}

/* @brief Initialize the StringBuffer
 *
 * @param StringBuffer* sb
 *
 * @return void
 * */
void init_string_buffer(StringBuffer *sb)
{
	sb->capacity = INITIAL_CAPACITY;
	sb->length = 0;
	sb->buffer = (char *)malloc(sb->capacity);
	if (!sb->buffer)
	{
		// Handle memory allocation failure
		DBG_IP_TOOLS printf("void init_string_buffer(StringBuffer *sb) Memory allocation failed \n");
		exit(1);
	}
	sb->buffer[0] = '\0'; // Empty string to start with
}

/* @brief receives ip address, tries to get country from it
 *
 * @param char *ip_address_to_resolve
 * @param char *memory_to_write_country_code_to
 *
 * @return void
 * */
void ip_tools_load_iso_country_code(char *ip_address_to_resolve, char *memory_to_write_country_code_to)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object = 0;
	cJSON *json_country_iso_code = 0;

	char mmdb_file[] = "dbip-country-lite-2025-06.mmdb";

	DBG_IP_TOOLS printf("ip_tools_load_iso_country_code \n");

	MMDB_s mmdb;
	int status = MMDB_open(mmdb_file, MMDB_MODE_MMAP, &mmdb);

	if (MMDB_SUCCESS != status)
	{
		DBG_IP_TOOLS printf("_ip_tools__open_or_die__internal failed \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	int gai_error = 0;
	int mmdb_error = 0;

	// Perform the lookup in the MMDB

	char actualiptoresolve[] = "84.245.121.196";

	MMDB_lookup_result_s gresult = MMDB_lookup_string(&mmdb, &ip_address_to_resolve[0], &gai_error, &mmdb_error);

	// Check for errors in the lookup process
	if (gai_error != 0)
	{
		DBG_IP_TOOLS printf("gai_error != 0 \n");
		goto label_ip_tools_load_iso_country_code_end;
	}
	if (mmdb_error != MMDB_SUCCESS)
	{
		DBG_IP_TOOLS printf("mmdb_error != MMDB_SUCCESS \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	// Declare a pointer for the data list
	MMDB_entry_data_list_s *gdata = NULL;

	// If no entry is found, return
	if (!gresult.found_entry)
	{
		DBG_IP_TOOLS printf("!gresult.found_entry \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	// Get the entry data list from the MMDB entry
	int gstatus = MMDB_get_entry_data_list(&gresult.entry, &gdata);
	if (gstatus != MMDB_SUCCESS)
	{
		DBG_IP_TOOLS printf("gstatus != MMDB_SUCCESS \n");
		MMDB_free_entry_data_list(gdata); // Clean up if getting the data fails
		goto label_ip_tools_load_iso_country_code_end;
	}

	StringBuffer sb;
	sb.capacity = INITIAL_CAPACITY;
	sb.length = 0;
	sb.buffer = 0;

	init_string_buffer(&sb);

	// Dump the entry data list to stdout (for debugging)
	char *json_mmdb_entry_data_list = MMDB_dump_entry_data_list(&sb, gdata, 0);
	DBG_IP_TOOLS printf("%s %s %s", "json is", json_mmdb_entry_data_list, "\n");

	// Clean up the entry data list
	MMDB_free_entry_data_list(gdata);

	// Define the function that processes the entry data

	if (json_mmdb_entry_data_list == NULL_POINTER)
	{
		DBG_IP_TOOLS printf("json_mmdb_entry_data_list == NULL_POINTER \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	json_root_object1 = cJSON_Parse(json_mmdb_entry_data_list);

	if (json_root_object1 == 0)
	{
		DBG_IP_TOOLS printf("json_root_object1 == 0 \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root_object1, "country");
	if (json_message_object == 0)
	{
		DBG_IP_TOOLS printf("json_message_object == 0 \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	if (!cJSON_IsObject(json_message_object))
	{
		DBG_IP_TOOLS printf("cJSON_IsObject(json_message_object) \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	json_country_iso_code = cJSON_GetObjectItemCaseSensitive(json_message_object, "iso_code");
	if (!cJSON_IsString(json_country_iso_code))
	{
		DBG_IP_TOOLS printf("cJSON_IsString(json_country_iso_code) \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	if (json_country_iso_code->valuestring == NULL)
	{
		DBG_IP_TOOLS printf("json_country_iso_code->valuestring == NULL \n");
		goto label_ip_tools_load_iso_country_code_end;
	}

	DBG_IP_TOOLS printf("%s %s %s", "json_country_iso_code->valuestring", json_country_iso_code->valuestring, "\n");

	clib__copy_memory((void *)&json_country_iso_code->valuestring[0], (void *)memory_to_write_country_code_to, 2, 2);

	cJSON_Delete(json_root_object1);
	free_string_buffer(&sb);

label_ip_tools_load_iso_country_code_end:

	return;
}
