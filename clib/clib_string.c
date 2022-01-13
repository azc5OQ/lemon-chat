#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clib_string.h"

void clib__int_to_hex_string(int n, char* hex_string)
{
    int i = 12;
    int j = 0;

    do
    {
        hex_string[i] = "0123456789ABCDEF"[n % 16];
        i--;
        n = n / 16;
    } while(n > 0);

    while(i++ < 13)
    {
        hex_string[j++] = hex_string[i];
    }

    hex_string[j + 1] = 0;
    //add null terminator to end of string
}


BOOL clib__is_str_number(char* string_to_check)
{
    BOOL result = TRUE;
    char numbers[] = "0123456789";
    BOOL is_check_finished = FALSE;
    int i;
    int x;

    if(strcmp("0", string_to_check) == 0)
    {
        return TRUE;
    }

    for (i = 0; i < strlen(string_to_check); i++)
    {
        if(is_check_finished == TRUE)
        {
            break;
        }

        for(x = 0; x < strlen(numbers); x++)
        {
            if(string_to_check[i] == numbers[x])
            {
                break;
            }
            if((x + 1) == strlen(numbers))
            {
                is_check_finished = TRUE;
                result = FALSE;
            }
        }
    }
    return result;
}

void clib__sanitize_stdin(char* input)
{
    if(input == 0)
    {
        return;
    }
    if(strlen(input) > 0)
    {
        if(input[strlen(input) - 1] == 10)
        {
            input[strlen(input) - 1] = 0;
        }
    }
}
