#include "memory_manager.h"
#include "clib/clib_memory.h"
#include "log/log.h"
#include "dave-g-json/cJSON.h"
#include <pthread.h>
#include "base.h"

//static char *dummy_string = "d";

//global,m ale co uz
memorymanager_entry_t *memory_manager_entries;
int memory_manager_entries_count = 0;

#define LIST_MAX_COUNT 30000

/**
 * @brief Global mutex.
 */
static pthread_mutex_t lemutex = PTHREAD_MUTEX_INITIALIZER;

//aj do xorstringulistu by sa mala predat kriticka sekcia.. ale to necham na zajtra

void memorymanager__init(void)
{
	memory_manager_entries = calloc(sizeof(memorymanager_entry_t) * LIST_MAX_COUNT, 1);

	if (pthread_mutex_init(&lemutex, NULL))
	{
		printf("%s", "pthread_mutex_init fail \n");
		exit(0);
	}
}

nuint memorymanager__realloc(nuint address, nuint newsize)
{
	nuint result = 0;
	pthread_mutex_lock(&lemutex);

	for (int i = 0; i < LIST_MAX_COUNT; i++)
	{
		memorymanager_entry_t *entry = &memory_manager_entries[i];

		if (entry->address == address && entry->allocated == TRUE)
		{
			void *new_address = realloc((void *)entry->address, newsize);
			entry->address = (nuint)new_address;
			entry->size = newsize;
			result = entry->address;
			break;
		}
	}

	pthread_mutex_unlock(&lemutex);

	return result;
}

nuint memorymanager__allocate(nuint size, nuint type)
{
	nuint result = 0;
	pthread_mutex_lock(&lemutex);

	for (int i = 0; i < LIST_MAX_COUNT; i++)
	{
		memorymanager_entry_t *entry = &memory_manager_entries[i];
		if (entry->allocated == 0)
		{
			entry->type = type;
			entry->allocated = TRUE;
			entry->address = (nuint)calloc(size, 1);
			entry->size = size;
			result = entry->address;
			DBG_MEMORY_MANAGER printf("%s %p %s %llu %s %llu %s", "memorymanager__allocate address ", (void *)entry->address, " size ", entry->size, "type: ", entry->type, "\n");
			memory_manager_entries_count++;
			break;
		}
	}

	pthread_mutex_unlock(&lemutex);

	return result;
}

void memorymanager__print_all_allocations()
{
	pthread_mutex_lock(&lemutex);

	for (int i = 0; i < LIST_MAX_COUNT; i++)
	{
		memorymanager_entry_t *entry = &memory_manager_entries[i];

		if (entry->allocated == TRUE)
		{
			printf("%s %p %s %llu %llu %s", "address ", (void *)entry->address, " size ", entry->size, entry->type, "\n");
		}
	}
	pthread_mutex_unlock(&lemutex);
}

void memorymanager__print_allocations_count()
{
	printf("%s %d %s", "memorymanager__print_allocations_count ", memory_manager_entries_count, "\n");
}

boole memorymanager__free(nuint address)
{
	boole result = FALSE;
	pthread_mutex_lock(&lemutex);

	for (int i = 0; i < LIST_MAX_COUNT; i++)
	{
		memorymanager_entry_t *entry = &memory_manager_entries[i];

		if (entry->address == address && entry->allocated == TRUE)
		{
			DBG_MEMORY_MANAGER printf("%s %p %s %llu %s %llu %s", "memorymanager__free address ", (void *)entry->address, " size  ", entry->size, "  type; ", entry->type, "\n");

			clib__null_memory((void *)entry->address, entry->size);
			free((void *)entry->address);
			//null entry itself but dont free it..
			clib__null_memory((void *)entry, sizeof(memorymanager_entry_t));
			result = TRUE;
			memory_manager_entries_count--;

			break;
		}
	}
	pthread_mutex_unlock(&lemutex);

	return result;
}
