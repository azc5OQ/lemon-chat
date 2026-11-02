#ifndef LEMON_CLIB_H
#define LEMON_CLIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef signed char boole;
typedef unsigned char ubyte;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned long long uint64;
typedef signed long long int64;
typedef uintptr_t nuint;
typedef intptr_t nint;
typedef unsigned long long timestamp;
typedef const char* cstring;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL_POINTER
#define NULL_POINTER 0
#endif

/* Caller-owned buffers. Capacity includes the terminator; these functions never allocate. */
/* -1 means invalid/unterminated. Reads at most capacity bytes. */
int64 clib__string_length(const char* string, uint64 capacity);
/* Copies are all-or-nothing. Failure clears a writable destination. Overlap is supported. */
boole clib__string_copy(char* destination, uint64 capacity, cstring source);
boole clib__string_copy_n(char* destination, uint64 capacity, const char* source, uint64 length);
/* Append leaves the existing string unchanged on failure. */
boole clib__string_append(char* destination, uint64 capacity, cstring source);
/* Returns required bytes excluding NUL, or -1 on invalid input. Overflow clears output. */
int64 clib__string_format(char* destination, uint64 capacity, cstring format, ...);
int64 clib__string_vformat(char* destination, uint64 capacity, cstring format, va_list arguments);
void clib__string_trim_newline(char* string, uint64 capacity);

/* Local byte/ASCII routines. Raw pointers must refer to the stated extent or a terminated string.
 * Allocation, files, networking, environment and threads remain host responsibilities.
 * Formatting supports integers, strings, characters, pointers and fixed-point %f; not %g/%e/%a.
 * See docs/clib.md for parsing, math and platform contracts. */
uint clib__abs(int value);
uint clib__bswap32(uint value);
uint64 clib__bswap64(uint64 value);
uint64 clib__udivmod64(uint64 numerator, uint64 denominator, uint64* remainder);
int64 clib__divmod64(int64 numerator, int64 denominator, int64* remainder);
uint64 clib__udiv64(uint64 numerator, uint64 denominator);
uint64 clib__urem64(uint64 numerator, uint64 denominator);
int64 clib__div64(int64 numerator, int64 denominator);
int64 clib__rem64(int64 numerator, int64 denominator);
void* clib__memcpy(void* dst, const void* src, int64 count);
void* clib__memset(void* dst, int value, int64 count);
void clib__null_memory(void* dst, int64 count);
void* clib__memmove(void* dst, const void* src, int64 count);
int clib__memcmp(const void* a, const void* b, int64 count);
int64 clib__strlen(const char* s);
int clib__strcmp(const char* a, const char* b);
int clib__strncmp(const char* a, const char* b, int64 n);
const char* clib__strstr(const char* haystack, const char* needle);
const char* clib__strchr(const char* s, int ch);
const char* clib__strrchr(const char* s, int ch);
const void* clib__memchr(const void* block, int ch, int64 count);
void clib__qsort(void* base, nuint count, nuint size, int (*compare)(const void*, const void*));
int clib__isspace(int c);
int clib__isdigit(int c);
int clib__isxdigit(int c);
int clib__isalpha(int c);
int clib__isalnum(int c);
int clib__tolower(int c);
int clib__toupper(int c);
int clib__atoi(const char* s);
long clib__strtol(const char* s, char** endptr, int base);
int64 clib__strtoll(const char* s, char** endptr, int base);
double clib__strtod(const char* s, char** endptr);
float clib__strtof(const char* s, char** endptr);
double clib__atof(const char* s);
float clib__sqrtf(float x);
float clib__fabsf(float x);
float clib__floorf(float x);
float clib__ceilf(float x);
float clib__fmodf(float x, float y);
double clib__sqrt(double x);
double clib__fabs(double x);
double clib__floor(double x);
double clib__ceil(double x);
double clib__fmod(double x, double y);
float clib__fminf(float x, float y);
float clib__fmaxf(float x, float y);
float clib__sinf(float x);
float clib__cosf(float x);
float clib__tanf(float x);
float clib__atanf(float x);
float clib__atan2f(float y, float x);
float clib__powf(float base, float exponent);
float clib__expf(float x);
float clib__logf(float x);
float clib__acosf(float x);
double clib__exp(double x);
double clib__log(double x);
double clib__pow_positive(double x, double exponent);
double clib__cos_bounded(double x);
double clib__hypot(double x, double y);
double clib__copysign(double magnitude, double sign);
int clib__isfinite(double x);
double clib__nan(void);

void clib__set_memory(void* destination, int64 length, ubyte value);
void clib__copy_memory(const void* source, void* destination, uint64 length, uint64 capacity);
boole clib__is_string_equal(cstring first, cstring second);
uint64 clib__utf8_string_length(cstring string);
int64 clib__utf8_string_length_check_max_length(cstring string, int maximum);
unsigned long clib__strtoul(cstring string, char** end, int base);

#endif
