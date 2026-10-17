#include "../definitions.h"
#include "clib_string.h"

#include <string.h>

/**
 * @brief Writes uppercase hexadecimal digits for a nonnegative integer.
 * @param int n -> number to convert; must not be negative
 * @param char* out_hex_string -> zeroed output buffer with at least 14 bytes
 * @return void
 */
void clib__int_to_hex_string(int n, char* out_hex_string)
{
    uint64 i = 12;
    uint64 j = 0;

    do
    {
        out_hex_string[i] = "0123456789ABCDEF"[n % 16];
        i--;
        n = n / 16;
    } while (n > 0);

    while (i++ < 13)
    {
        out_hex_string[j++] = out_hex_string[i];
    }

    out_hex_string[j + 1] = 0;
    /* add null terminator to end of string */
}

/**
 * @brief Checks that a string contains only decimal digits.
 * @param char* string_to_check -> non-NULL, null-terminated string
 * @return boole -> TRUE for digits or an empty string; FALSE for any other character
 */
boole clib__is_str_number(char* string_to_check)
{
    boole result = TRUE;
    char numbers[] = "0123456789";
    boole is_check_finished = FALSE;
    uint64 i = 0;
    uint64 x = 0;

    if (strcmp("0", string_to_check) == 0)
    {
        return TRUE;
    }

    for (i = 0; i < strlen(string_to_check); i++)
    {
        if (is_check_finished == TRUE)
        {
            break;
        }

        for (x = 0; x < strlen(numbers); x++)
        {
            if (string_to_check[i] == numbers[x])
            {
                break;
            }
            if ((x + 1) == strlen(numbers))
            {
                is_check_finished = TRUE;
                result = FALSE;
            }
        }
    }
    return result;
}

/**
 * @brief Removes one trailing line-feed character from input, if present.
 * @param char* input -> writable, null-terminated input; NULL is ignored
 * @return void
 */
void clib__sanitize_stdin(char* input)
{
    if (input == 0)
    {
        return;
    }

    if (strlen(input) > 0)
    {
        if (input[strlen(input) - 1] == 10)
        {
            input[strlen(input) - 1] = 0;
        }
    }
}

/**
 * @brief Counts string bytes until its terminator or the size limit is exceeded.
 * @param cstring arg_string -> non-NULL, null-terminated string
 * @param int max_length -> largest accepted byte count, excluding the terminator
 * @return int64 -> byte count, or -1 when the string exceeds max_length
 */
int64 clib__utf8_string_length_check_max_length(cstring arg_string, int max_length)
{
    /* ASCII string */
    int64 curr_string_length = 0;
    int64 i = 0;

    /* string length limit, 1000 */

    /* printf("%s %s %s", "arg_string", arg_string, "\n"); */

    for (;;)
    {
        char single_char = 0;
        single_char = *(arg_string + i);
        /* printf("%s %d %s", "single_char", single_char, "\n"); */

        if (single_char == 0)
        {
            break;
        }
        curr_string_length++;

        if (curr_string_length > max_length)
        {
            return -1;
        }
        i++;
    }
    return curr_string_length;
}

/**
 * @brief Counts string bytes, excluding the terminating zero.
 * @param cstring arg_string -> null-terminated string, or NULL
 * @return uint64 -> byte count; 0 for NULL or an empty string
 */
uint64 clib__utf8_string_length(cstring arg_string)
{
    /* ASCII string */
    uint64 curr_string_length = 0;

    uint64 i = 0;

    /* guard against a NULL argument: strlen(NULL) is undefined and would crash; treat it as an empty string */
    if (arg_string == 0)
    {
        return 0;
    }

    for (;;)
    {
        char single_char = 0;
        single_char = *(arg_string + i);

        /* printf("%s %d %s", "single_char", single_char, "\n"); */

        if (single_char == 0)
        {
            break;
        }

        curr_string_length++;
        i++;
    }
    return curr_string_length;
}

/**
 * @brief Compares two strings byte for byte, including letter case.
 * @param cstring str1 -> first null-terminated string
 * @param cstring str2 -> second null-terminated string
 * @return boole -> TRUE when both strings match; FALSE when either is NULL
 */
boole clib__is_string_equal(cstring str1, cstring str2)
{
    boole result = TRUE;
    uint64 a = 0;
    uint64 b = 0;
    uint64 i = 0;

    if (str1 == NULL_POINTER || str2 == NULL_POINTER)
    {
        result = FALSE;
        return result;
    }

    a = (uint64)clib__utf8_string_length(str1);
    b = (uint64)clib__utf8_string_length(str2);

    if (a != b)
    {
        result = FALSE;
    }
    else
    {
        for (i = 0; i < a; i++)
        {
            char byte1 = 0;
            char byte2 = 0;
            byte1 = str1[i];
            byte2 = str2[i];
            if (byte1 != byte2)
            {
                result = FALSE;
                break;
            }
        }
    }
    return result;
}
