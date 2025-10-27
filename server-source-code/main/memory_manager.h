#ifndef MEMORY_MANAGER_H

#define MEMORY_MANAGER_H

#include "mytypedef.h"

#include <stdlib.h>

typedef enum type_e
{
	MEMALLOC_TYPE_CJSON,
	MEMALLOC_TYPE_MAIN,
	MEMALLOC_TYPE_WEBSOCKET,
	MEMALLOC_TYPE_OTHER,
	MEMALLOC_TYPE_CHALLENGE,
	MEMALLOC_TYPE_ENCRYPT,
	MEMALLOC_TYPE_DECRYPT,
	MEMALLOC_MARKED_CHANNEL_INDICES,
	MEMALLOC_DHPROCESS,
	MEMALLOC_PUBLIC_KEY_ENCRYPT,
	MEMALLOC_CLIENTS_ARRAY,
	MEMALLOC_CHANNELS_ARRAY,
	MEMALLOC_CLIENT_STORED_DATA_ARRAY,
	MEMALLOC_FIND_MAINTAINER,
	MEMALLOC_MARKED_CLIENT_INDICES,
	MEMALLOC_OPUS_DATA_BUFFER_ENTRY,
	MEMALLOC_WEBRTC_PEERS,
	MEMALLOC_AUDIOCHANNEL_ONMESSAGE,
} type_e;

typedef struct memorymanager_entry_t
{
	nuint allocated;
	nuint address;
	nuint size;
	nuint type;
	nuint timestamp;
	nuint client_id;
} memorymanager_entry_t;

void memorymanager__init(void);
nuint memorymanager__allocate(nuint size, nuint type);
void memorymanager__print_all_allocations();
boole memorymanager__free(nuint address);
nuint memorymanager__realloc(nuint address, nuint newsize);
void memorymanager__print_allocations_count();

#endif