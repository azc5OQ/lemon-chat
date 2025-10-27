#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include "maxminddb.h"
#include <errno.h>
#include <getopt.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#include <malloc.h>
#else
#include <libgen.h>
#include <unistd.h>
#endif

static MMDB_s open_or_die(const char *fname);
static bool lookup_from_file(MMDB_s *const mmdb, char const *const ip_file, bool const dump);
static MMDB_lookup_result_s lookup_or_die(MMDB_s *mmdb, const char *ipstr);


static MMDB_lookup_result_s lookup_or_die(MMDB_s *mmdb, const char *ipstr) 
{
    int gai_error, mmdb_error;

    MMDB_lookup_result_s result = MMDB_lookup_string(mmdb, ipstr, &gai_error, &mmdb_error);

    if (0 != gai_error)
    {

        printf("%s %s %s","Error from call to getaddrinfo for", ipstr, "\n");
        exit(3);
    }

    if (MMDB_SUCCESS != mmdb_error) 
    {
        printf("%s %s %s","Got an error from the maxminddb library:", MMDB_strerror(mmdb_error), "\n");
        exit(4);
    }

    return result;
}

static MMDB_s open_or_die(const char *fname) {
    MMDB_s mmdb;
    int status = MMDB_open(fname, MMDB_MODE_MMAP, &mmdb);

    if (MMDB_SUCCESS != status) {
        fprintf(
            stderr, "\n  Can't open %s - %s\n", fname, MMDB_strerror(status));

        if (MMDB_IO_ERROR == status) {
            fprintf(stderr, "    IO error: %s\n", strerror(errno));
        }

        fprintf(stderr, "\n");

        exit(2);
    }

    return mmdb;
}

void append_to_string_buffer(StringBuffer *sb, const char *str) {
    size_t str_len = strlen(str);
    size_t new_length = sb->length + str_len;

    // Resize the buffer if necessary
    if (new_length >= sb->capacity) {
        sb->capacity = new_length * 2; // Double the capacity
        sb->buffer = (char *)realloc(sb->buffer, sb->capacity);
        if (!sb->buffer) {
            // Handle memory allocation failure
            fprintf(stderr, "Memory allocation failed\n");
            exit(1);
        }
    }

    // Append the string to the buffer
    strcat(sb->buffer, str);
    sb->length = new_length;
}

// Retrieve the contents of the buffer as a string
const char *get_string_from_buffer(const StringBuffer *sb) {
    return sb->buffer;
}

// Clean up the StringBuffer
void free_string_buffer(StringBuffer *sb) {
    free(sb->buffer);
}

// Initialize the StringBuffer
void init_string_buffer(StringBuffer *sb) {
    sb->capacity = INITIAL_CAPACITY;
    sb->length = 0;
    sb->buffer = (char *)malloc(sb->capacity);
    if (!sb->buffer) {
        // Handle memory allocation failure
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }
    sb->buffer[0] = '\0'; // Empty string to start with
}


int main()
{
    char ip[] = "150.4.1.2";
    char mmdb_file[] = "/Users/user/Desktop/playground/dbip-country-lite-2025-06.mmdb";
    MMDB_s mmdb = open_or_die(mmdb_file);

    int gai_error = 0;
    int mmdb_error = 0;


    // Perform the lookup in the MMDB
    MMDB_lookup_result_s gresult = MMDB_lookup_string(&mmdb, ip, &gai_error, &mmdb_error);

    // Check for errors in the lookup process
    if (gai_error != 0) 
    {
        return 0;  // Error in getting address information
    }
    if (mmdb_error != MMDB_SUCCESS) 
    {
        return 0;  // MMDB lookup failed
    }

    // Declare a pointer for the data list
    MMDB_entry_data_list_s *gdata = NULL;

    // If no entry is found, return
    if (!gresult.found_entry) 
    {
        return 0;
    }

    // Get the entry data list from the MMDB entry
    int gstatus = MMDB_get_entry_data_list(&gresult.entry, &gdata);
    if (gstatus != MMDB_SUCCESS) 
    {
        MMDB_free_entry_data_list(gdata);  // Clean up if getting the data fails
        return 0;
    }


    StringBuffer sb;
    sb.capacity = INITIAL_CAPACITY;
    sb.length = 0;
    sb.buffer = 0;

    init_string_buffer(&sb);

    // Dump the entry data list to stdout (for debugging)
    char* test = MMDB_dump_entry_data_list(&sb, gdata, 0);
    printf("%s %s %s", "json is", test, "\n");

    // Clean up the entry data list
    MMDB_free_entry_data_list(gdata);

    // Define the function that processes the entry data
    return 0;
}

