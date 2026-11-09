#ifndef LEMON_DESKTOP_DEFINITIONS_H
#define LEMON_DESKTOP_DEFINITIONS_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

// Basic types shared by the desktop client's main modules.
typedef signed char boole;
typedef unsigned char ubyte;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned long long uint64;
typedef signed long long int64;
// Match the compiled target's address width.
#if defined(_WIN64) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
typedef unsigned long long nuint;
typedef signed long long nint;
#elif defined(_WIN32) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4)
typedef unsigned int nuint;
typedef signed int nint;
#elif (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 2)
typedef unsigned short nuint;
typedef signed short nint;
#else
#error Unsupported target address width
#endif
typedef unsigned long long timestamp;
typedef const char *cstring;

// Wire integers and audio samples have the same widths on every supported target.
_Static_assert(CHAR_BIT == 8 && sizeof(ushort) == 2 && sizeof(short) == 2 && sizeof(uint) == 4 && sizeof(int) == 4 && sizeof(uint64) == 8 && sizeof(int64) == 8, "Unsupported integer widths");
_Static_assert(sizeof(nuint) == sizeof(void *) && sizeof(nint) == sizeof(void *), "Native integers must match pointer width");

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL_POINTER
#define NULL_POINTER 0
#endif

// --- Connection encryption ---

enum
{
    LEMON_AES256 = 1,
    LEMON_CHACHA20 = 3,
    LEMON_CAMELLIA256 = 4,
    LEMON_TWOFISH256 = 5,
    LEMON_SERPENT256 = 6,
    LEMON_TRIVIUM80 = 7
};

#define LEMON_MAX_DH_EXCHANGES 4
#define LEMON_MAX_SESSION_LAYERS 8
#define LEMON_MAX_CONNECTION_LAYERS (LEMON_MAX_SESSION_LAYERS + 1)

typedef struct lemon_session_policy
{
    uint64 exchanges;
    uint64 layers;
    int64 cipher;
    boole random_layer;
} lemon_session_policy;

typedef struct lemon_session_layer
{
    int64 type;
    ubyte bytes[32];
} lemon_session_layer;

typedef struct lemon_session_keys
{
    uint64 count;
    lemon_session_layer client_to_server[LEMON_MAX_CONNECTION_LAYERS];
    lemon_session_layer server_to_client[LEMON_MAX_CONNECTION_LAYERS];
    ubyte client_mac[32];
    ubyte server_mac[32];
} lemon_session_keys;

#endif
