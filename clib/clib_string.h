#ifndef CLIB_STRING_H

#define CLIB_STRING_H

#include <stdbool.h>

bool clib__is_str_number(char* string_to_check);
void clib__int_to_hex_string(int n, char* hex_string);

#endif
