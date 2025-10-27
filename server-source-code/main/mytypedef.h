#ifndef MYTYPEDEF_H
#define MYTYPEDEF_H

//#define DEBUG_ACTIVE 1

//#define WINDOWS_BUILD 1
//#define LINUX_BUILD 1

#define DONT_USE_AUDIO_CHANNEL 1

#define ARCHITECTURE_AMD64 1
//#define ARCHITECTURE_I386 1

#ifndef MYTYPEDEF_DEFINITIONS
#define MYTYPEDEF_DEFINITIONS 1

#define FALSE 0
#define TRUE 1
#define NULL_POINTER 0
#define SEEK_END 2
#define SEEK_SET 0

#endif

#define MATH_PI 3.141592f


typedef union safe_byte_t
{
	signed char safe1;
	unsigned char saf2;
} safe_byte_t;

#ifdef ARCHITECTURE_AMD64
typedef unsigned long long nuint; // size of address, native unsigned integer
typedef signed long long nint; // native (signed) integer
#endif

#ifdef ARCHITECTURE_I386
typedef unsigned int nuint;
typedef signed int nint;
#endif

typedef signed char boole; // 1 byte always (George Boole)
typedef unsigned char ubyte; // 1 byte always
typedef unsigned int uint; // 4 bytes always
typedef unsigned short ushort; // 2 bytes always
typedef unsigned long long uint64; //8 bytes always
typedef signed long long int64; //8 bytes always
typedef unsigned long long timestamp; //8 bytes always
typedef const char *cstring;
//typedef wchar_t*                    wstring;

#ifdef DEBUG_ACTIVE
#define DBG_DLLMAIN if (1)
#define DBG_CLIENT_MESSAGE if (1)
#define DBG_CLIENT_MESSAGE_MAIN_FUNCTION if (1)
#define DBG_AUTHENTICATION if (1)
#define DBG_ENCRYPTION if (0)
#define DBG_SERVER_MESSAGE if (1)
#define DBG_CLOSE_CONNECTION if (0)
#define DBG_ONMESSAGE if (1)
#define DBG_MEMORY_MANAGER if (0)
#define DBG_CONNECTION_CHECK_THREAD if (1)
#define DBG_CLIENT_DISCONNECT if (1)
#define DBG_AUDIOCHANNEL_WEBRTC if (0)
#define DBG_VIOLET if (0)
#define DBG_DBG_MEMORY_ALLOCATIONS if (0)
#define DBG_IP_TOOLS if (1)
#endif

#ifndef DEBUG_ACTIVE
#define DBG_DLLMAIN if (0)
#define DBG_CLIENT_MESSAGE if (0)
#define DBG_CLIENT_MESSAGE_MAIN_FUNCTION if (0)
#define DBG_AUTHENTICATION if (0)
#define DBG_ENCRYPTION if (0)
#define DBG_SERVER_MESSAGE if (0)
#define DBG_CLOSE_CONNECTION if (0)
#define DBG_ONMESSAGE if (0)
#define DBG_MEMORY_MANAGER if (0)
#define DBG_CONNECTION_CHECK_THREAD if (0)
#define DBG_CLIENT_DISCONNECT if (0)
#define DBG_AUDIOCHANNEL_WEBRTC if (0)
#define DBG_VIOLET if (0)
#define DBG_DBG_MEMORY_ALLOCATIONS if (0)
#define DBG_IP_TOOLS if (0)
#endif

#ifdef DONT_USE_AUDIO_CHANNEL
#define AUDIO_CHANNEL_CHECK_IF_RETURN_NEEDED if (1)
#endif

#ifndef DONT_USE_AUDIO_CHANNEL
#define AUDIO_CHANNEL_CHECK_IF_RETURN_NEEDED if (0)
#endif

int mytypedef__check_data_types_for_consistency(void);

#endif
