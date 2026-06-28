#include "definitions.h"

#include "../third-party/libmaxminddb-1.12.2/include/maxminddb.h"

#include "ip_tools.h"

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"

#include "../third-party/rxi-log/log.h"

#include <stdio.h>
#include <string.h>

/* @brief appends a string to a StringBuffer, growing the backing buffer if needed
 *
 * @param StringBuffer* sb -> the string buffer to append to
 * @param const char* str -> the null-terminated string to append
 *
 * @return void
 * */
void append_to_string_buffer(StringBuffer* sb, const char* str)
{
    uint64 str_len = strlen(str);
    uint64 new_length = sb->length + str_len;

    /* Resize the buffer if necessary */
    if (new_length >= sb->capacity)
    {
        sb->capacity = new_length * 2; /* Double the capacity */
        sb->buffer = (char*)realloc(sb->buffer, sb->capacity);
        if (sb->buffer == NULL_POINTER)
        {
            /* Handle memory allocation failure */
            DBG_IP_TOOLS printf("%s", "Memory allocation failed \n");
            exit(1);
        }
    }

    /* Append the string to the buffer */
    strcat(sb->buffer, str);
    sb->length = new_length;
}

/* @brief returns the StringBuffer's current contents
 *
 * @param const StringBuffer* sb -> the string buffer to read the contents from
 *
 * @return const char *
 * */
const char* get_string_from_buffer(const StringBuffer* sb)
{
    return sb->buffer;
}

/* @brief Clean up the StringBuffer
 *
 * @param StringBuffer* sb -> the string buffer whose backing buffer is freed
 *
 * @return void
 * */
void free_string_buffer(StringBuffer* sb)
{
    free(sb->buffer);
}

/* @brief Initialize the StringBuffer
 *
 * @param StringBuffer* sb -> the string buffer to initialize and allocate
 *
 * @return void
 * */
void init_string_buffer(StringBuffer* sb)
{
    sb->capacity = INITIAL_CAPACITY;
    sb->length = 0;
    sb->buffer = (char*)malloc(sb->capacity);
    if (sb->buffer == NULL_POINTER)
    {
        /* Handle memory allocation failure */
        DBG_IP_TOOLS printf("%s", "void init_string_buffer(StringBuffer* sb) Memory allocation failed \n");
        exit(1);
    }
    sb->buffer[0] = '\0'; /* Empty string to start with */
}

/* @brief receives ip address, tries to get country from it
 *
 * @param char* ip_address_to_resolve -> the IP address string to look up in the MMDB
 * @param char* out_memory_to_write_country_code_to -> buffer that receives the 2-character ISO country code
 *
 * @return void
 * */
void ip_tools_load_iso_country_code(char* ip_address_to_resolve, char* out_memory_to_write_country_code_to)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object = 0;
    cJSON* json_country_iso_code = 0;
    char mmdb_file[] = "dbip-country-lite-2025-06.mmdb";
    MMDB_s mmdb;
    int status = 0;
    int gai_error = 0;
    int mmdb_error = 0;
    MMDB_lookup_result_s gresult;
    MMDB_entry_data_list_s* gdata = NULL_POINTER;
    int gstatus = 0;
    StringBuffer sb;
    char* json_mmdb_entry_data_list = 0;

    if (ip_address_to_resolve == NULL_POINTER)
    {
        DBG_IP_TOOLS log_info("%s", "ip_tools_load_iso_country_code char* ip_address_to_resolve is null pointer \n");

        goto label_ip_tools_load_iso_country_code_end;
    }

    DBG_IP_TOOLS log_info("%s %s %s", "ip_tools_load_iso_country_code ip_address_to_resolve -> ", ip_address_to_resolve, "\n");

    status = MMDB_open(mmdb_file, MMDB_MODE_MMAP, &mmdb);

    if (MMDB_SUCCESS != status)
    {
        DBG_IP_TOOLS log_info("%s", "_ip_tools__open_or_die__internal failed \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    /* Perform the lookup in the MMDB */

    gresult = MMDB_lookup_string(&mmdb, &ip_address_to_resolve[0], &gai_error, &mmdb_error);

    /* Check for errors in the lookup process */
    if (gai_error != 0)
    {
        DBG_IP_TOOLS log_info("%s", "gai_error != 0 \n");
        goto label_ip_tools_load_iso_country_code_end;
    }
    if (mmdb_error != MMDB_SUCCESS)
    {
        DBG_IP_TOOLS log_info("%s", "mmdb_error != MMDB_SUCCESS \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    /* Declare a pointer for the data list */

    /* If no entry is found, return */
    if (gresult.found_entry == FALSE)
    {
        DBG_IP_TOOLS log_info("%s", "!gresult.found_entry \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    /* Get the entry data list from the MMDB entry */
    gstatus = MMDB_get_entry_data_list(&gresult.entry, &gdata);
    if (gstatus != MMDB_SUCCESS)
    {
        DBG_IP_TOOLS log_info("%s", "gstatus != MMDB_SUCCESS \n");
        MMDB_free_entry_data_list(gdata); /* Clean up if getting the data fails */
        goto label_ip_tools_load_iso_country_code_end;
    }

    sb.capacity = INITIAL_CAPACITY;
    sb.length = 0;
    sb.buffer = 0;

    init_string_buffer(&sb);

    /* Dump the entry data list to stdout (for debugging) */
    json_mmdb_entry_data_list = MMDB_dump_entry_data_list(&sb, gdata, 0);
    DBG_IP_TOOLS log_info("%s %s %s", "json is", json_mmdb_entry_data_list, "\n");

    /* Clean up the entry data list */
    MMDB_free_entry_data_list(gdata);

    /* Define the function that processes the entry data */

    if (json_mmdb_entry_data_list == NULL_POINTER)
    {
        DBG_IP_TOOLS log_info("%s", "json_mmdb_entry_data_list == NULL_POINTER \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    json_root_object1 = cJSON_Parse(json_mmdb_entry_data_list);

    if (json_root_object1 == 0)
    {
        DBG_IP_TOOLS log_info("%s", "json_root_object1 == 0 \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root_object1, "country");
    if (json_message_object == 0)
    {
        DBG_IP_TOOLS log_info("%s", "json_message_object == 0 \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    if (cJSON_IsObject(json_message_object) == FALSE)
    {
        DBG_IP_TOOLS log_info("%s", "cJSON_IsObject(json_message_object) \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    json_country_iso_code = cJSON_GetObjectItemCaseSensitive(json_message_object, "iso_code");
    if (cJSON_IsString(json_country_iso_code) == FALSE)
    {
        DBG_IP_TOOLS log_info("%s", "cJSON_IsString(json_country_iso_code) \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    if (json_country_iso_code->valuestring == NULL_POINTER)
    {
        DBG_IP_TOOLS log_info("%s", "json_country_iso_code->valuestring == NULL \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    DBG_IP_TOOLS log_info("%s %s %s", "json_country_iso_code->valuestring ", json_country_iso_code->valuestring, "\n");

    clib__copy_memory((void*)&json_country_iso_code->valuestring[0], (void*)out_memory_to_write_country_code_to, 2, 2);

    cJSON_Delete(json_root_object1);
    free_string_buffer(&sb);

label_ip_tools_load_iso_country_code_end:

    MMDB_close(&mmdb);

    return;
}
