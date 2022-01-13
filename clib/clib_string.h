#ifndef CLIB_STRING_H

#define CLIB_STRING_H

#ifndef TRUE
typedef int BOOL;
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

BOOL clib__is_str_number(char* string_to_check);
void clib__int_to_hex_string(int n, char* hex_string);
void clib__sanitize_stdin(char* input);

#endif
