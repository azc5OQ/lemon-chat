#include "definitions.h"

#include "../third-party/libmaxminddb-1.12.2/include/maxminddb.h"

#include "ip_tools.h"

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"

#include "../third-party/rxi-log/log.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief appends a null-terminated string to a StringBuffer, growing the backing buffer if needed.
 *
 *        when the resulting length no longer fits in the current capacity the buffer is reallocated
 *        to twice the new length, then the string is concatenated with strcat and the stored length
 *        is updated. a failed reallocation is logged through DBG_IP_TOOLS and terminates the process
 *        with exit(1).
 *
 * @param StringBuffer* sb -> the string buffer to append to
 * @param const char* str -> the null-terminated string to append
 *
 * @return void
 */
void append_to_string_buffer(StringBuffer* sb, const char* str)
{
    uint64 str_len = strlen(str);
    uint64 new_length = sb->length + str_len;

    // Resize the buffer if necessary
    if (new_length >= sb->capacity)
    {
        sb->capacity = new_length * 2; // Double the capacity
        sb->buffer = (char*)realloc(sb->buffer, sb->capacity);
        if (sb->buffer == NULL_POINTER)
        {
            // Handle memory allocation failure
            DBG_IP_TOOLS printf("%s", "Memory allocation failed \n");
            exit(1);
        }
    }

    // Append the string to the buffer
    strcat(sb->buffer, str);
    sb->length = new_length;
}

/**
 * @brief returns the StringBuffer's current contents.
 *
 *        hands back the backing buffer pointer itself, no copy is made, so the caller must not free
 *        it and must not keep it across a later append that can reallocate the buffer.
 *
 * @param const StringBuffer* sb -> the string buffer to read the contents from
 *
 * @return const char* -> pointer to the buffer's null-terminated contents
 */
const char* get_string_from_buffer(const StringBuffer* sb)
{
    return sb->buffer;
}

/**
 * @brief cleans up a StringBuffer by releasing its backing buffer.
 *
 *        only the allocated buffer is freed, the StringBuffer struct itself is not, and the capacity
 *        and length fields are left untouched, so the struct must be re-initialised with
 *        init_string_buffer before it is used again.
 *
 * @param StringBuffer* sb -> the string buffer whose backing buffer is freed
 *
 * @return void
 */
void free_string_buffer(StringBuffer* sb)
{
    free(sb->buffer);
}

/**
 * @brief initialises a StringBuffer and allocates its backing buffer.
 *
 *        capacity is set to INITIAL_CAPACITY, length to zero, and a buffer of that capacity is
 *        allocated and started as an empty string. a failed allocation is logged through
 *        DBG_IP_TOOLS and terminates the process with exit(1).
 *
 * @param StringBuffer* sb -> the string buffer to initialize and allocate
 *
 * @return void
 */
void init_string_buffer(StringBuffer* sb)
{
    sb->capacity = INITIAL_CAPACITY;
    sb->length = 0;
    sb->buffer = (char*)malloc(sb->capacity);
    if (sb->buffer == NULL_POINTER)
    {
        // Handle memory allocation failure
        DBG_IP_TOOLS printf("%s", "void init_string_buffer(StringBuffer* sb) Memory allocation failed \n");
        exit(1);
    }
    sb->buffer[0] = '\0'; // Empty string to start with
}

/**
 * @brief resolves an IP address to its 2-character ISO country code through the local GeoIP database.
 *
 *        opens the dbip-country-lite-2025-06.mmdb file, looks the address up, dumps the matched entry
 *        data list into a StringBuffer as JSON, parses it with cJSON and copies the "country" object's
 *        "iso_code" string into the caller's buffer. every failure step - null address, open failure,
 *        lookup error, no entry found, missing or non-string iso_code - logs through DBG_IP_TOOLS and
 *        jumps to the common exit label, which closes the database and leaves the output buffer as the
 *        caller supplied it. when DEBUG_ASSIGN_RANDOM_COUNTRY_CODE is defined the lookup is skipped
 *        entirely and a random real ISO code from a fixed 20-entry table is written instead, so country
 *        flags still render during offline or LAN testing. see definitions.h.
 *
 * @param char* ip_address_to_resolve -> the IP address string to look up in the MMDB
 * @param char* out_memory_to_write_country_code_to -> buffer that receives the 2-character ISO country code
 *
 * @note the normal path copies exactly 2 bytes and does not null-terminate, the debug path writes 2
 *       characters plus a terminator, so the buffer must hold at least 3 bytes.
 *
 * @return void
 */
void ip_tools_load_iso_country_code(char* ip_address_to_resolve, char* out_memory_to_write_country_code_to)
{
#ifdef DEBUG_ASSIGN_RANDOM_COUNTRY_CODE
    // offline/LAN testing: skip the GeoIP lookup entirely and hand out a real ISO code (same
    // uppercase 2-char form the MMDB path below writes) so country flags render. see definitions.h
    {
        static const char* debug_iso_codes[] = {
            "US", "GB", "DE", "FR", "JP", "BR", "CA", "AU", "IN", "IT",
            "ES", "NL", "SE", "PL", "RU", "CN", "KR", "MX", "ZA", "TR"
        };
        // deliberately NOT rand(): its state is per-thread on windows and every client is handled on a
        // fresh thread, so each one started from the default seed and always got the same code (GB)
        static pthread_mutex_t debug_iso_mutex = PTHREAD_MUTEX_INITIALIZER;
        static uint64 debug_iso_counter = 0;
        const char* picked = NULL_POINTER;
        uint64 code_count = sizeof(debug_iso_codes) / sizeof(debug_iso_codes[0]);
        uint64 index = 0;

        pthread_mutex_lock(&debug_iso_mutex);
        index = debug_iso_counter % code_count;
        debug_iso_counter = debug_iso_counter + 1;
        pthread_mutex_unlock(&debug_iso_mutex);

        // cycling beats a random pick for this: every flag gets shown, and none repeats until all have
        picked = debug_iso_codes[index];
        out_memory_to_write_country_code_to[0] = picked[0];
        out_memory_to_write_country_code_to[1] = picked[1];
        out_memory_to_write_country_code_to[2] = 0;
        return;
    }
#endif
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

    // Perform the lookup in the MMDB

    gresult = MMDB_lookup_string(&mmdb, &ip_address_to_resolve[0], &gai_error, &mmdb_error);

    // Check for errors in the lookup process
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

    // Declare a pointer for the data list

    // If no entry is found, return
    if (gresult.found_entry == FALSE)
    {
        DBG_IP_TOOLS log_info("%s", "!gresult.found_entry \n");
        goto label_ip_tools_load_iso_country_code_end;
    }

    // Get the entry data list from the MMDB entry
    gstatus = MMDB_get_entry_data_list(&gresult.entry, &gdata);
    if (gstatus != MMDB_SUCCESS)
    {
        DBG_IP_TOOLS log_info("%s", "gstatus != MMDB_SUCCESS \n");
        MMDB_free_entry_data_list(gdata); // Clean up if getting the data fails
        goto label_ip_tools_load_iso_country_code_end;
    }

    sb.capacity = INITIAL_CAPACITY;
    sb.length = 0;
    sb.buffer = 0;

    init_string_buffer(&sb);

    // Dump the entry data list to stdout (for debugging)
    json_mmdb_entry_data_list = MMDB_dump_entry_data_list(&sb, gdata, 0);
    DBG_IP_TOOLS log_info("%s %s %s", "json is", json_mmdb_entry_data_list, "\n");

    // Clean up the entry data list
    MMDB_free_entry_data_list(gdata);

    // Define the function that processes the entry data

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
