#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

    //hex_string[j + 1] = 0;
    //add null terminator to end of string
}


bool clib__is_str_number(char* string_to_check)
{
    if(strcmp("0", string_to_check) == 0)
    {
        return true;
    }


    bool result = false;
    char allowed_numbers[] = "0123456789";
    bool check_ended = false;

    for (int i = 0; i < strlen(string_to_check); i++)
    {
        if(check_ended == true)
        {
            result = false;
            break;
        }

        for(int x = 0; x < strlen(allowed_numbers); x++)
        {
            if(string_to_check[i] == allowed_numbers[x])
            {
                break;
            }
            else
            {
                if(x == strlen(allowed_numbers) - 1)
                {
                    check_ended = true;
                }
            }
            if(i == strlen(string_to_check) - 1)
            {
                result = true;
            }
        }
    }
    return result;
}
