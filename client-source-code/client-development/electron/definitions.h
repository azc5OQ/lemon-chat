#ifndef LEMON_ELECTRON_DEFINITIONS_H
#define LEMON_ELECTRON_DEFINITIONS_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

// basic types of the push-to-talk helpers and the Windows starter, the same as in the desktop client's
// main/definitions.h
typedef signed char boole;
typedef unsigned char ubyte;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned long long uint64;
typedef signed long long int64;
// match the compiled target's address width
#if defined(_WIN64) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
typedef unsigned long long nuint;
typedef signed long long nint;
#elif defined(_WIN32) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4)
typedef unsigned int nuint;
typedef signed int nint;
#else
#error Unsupported target address width
#endif

_Static_assert(CHAR_BIT == 8 && sizeof(ushort) == 2 && sizeof(uint) == 4 && sizeof(uint64) == 8 && sizeof(int64) == 8, "Unsupported integer widths");
_Static_assert(sizeof(nuint) == sizeof(void *) && sizeof(nint) == sizeof(void *), "Native integers must match pointer width");

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif
