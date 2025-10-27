#ifndef CLIB_STRING_H

#define CLIB_STRING_H

#ifndef TRUE
typedef int BOOL;
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#include "../mytypedef.h"
boole clib__is_str_number(char *string_to_check);
void clib__int_to_hex_string(int n, char *hex_string);
void clib__sanitize_stdin(char *input);

boole clib__is_string_equal(cstring str1, cstring str2);
nuint clib__utf8_string_length(cstring arg_string);
int64 clib__utf8_string_length_check_max_length(cstring arg_string, int max_length);

#endif
