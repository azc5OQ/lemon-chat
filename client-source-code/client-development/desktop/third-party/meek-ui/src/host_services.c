// host_services.c - the one global the library keeps for the services a
// host hands it (heap, log sink, file reader). The logger and the file
// layer read them from here; the heap slots are passed on to the memory
// manager, which keeps its own copy (clib/memory_manager.h).
#include "host_services.h"
#include "clib/stdlib.h"

static meek_host_services_t _host_services_internal__services;
static boole                _host_services_internal__installed = FALSE;

void host_services__set(const meek_host_services_t* services)
{
    memory_manager_functions_t functions;

    if (services == NULL_POINTER)
    {
        stdlib__null_memory(&_host_services_internal__services, sizeof(_host_services_internal__services));
        _host_services_internal__installed = FALSE;
        memory_manager__delegate_to(NULL_POINTER);
        return;
    }
    // copied, so the caller's struct may live on the stack
    stdlib__memcpy(&_host_services_internal__services, services, sizeof(_host_services_internal__services));
    _host_services_internal__installed = TRUE;
    // the heap slots go to the memory manager; it decides at its init
    // whether they are usable
    functions.allocate = services->allocate;
    functions.allocate_zeroed = services->allocate_zeroed;
    functions.reallocate = services->reallocate;
    functions.release = services->release;
    functions.owner = services->owner;
    memory_manager__delegate_to(&functions);
}

const meek_host_services_t* host_services__get(void)
{
    return &_host_services_internal__services;
}

boole host_services__has_heap(void)
{
    return (boole)(_host_services_internal__installed == TRUE
        && _host_services_internal__services.allocate != NULL_POINTER
        && _host_services_internal__services.reallocate != NULL_POINTER
        && _host_services_internal__services.release != NULL_POINTER);
}

void memory_manager__export_services(meek_host_services_t* out)
{
    memory_manager_functions_t functions;

    if (out == NULL_POINTER)
    {
        return;
    }
    memory_manager__export(&functions);
    out->allocate = functions.allocate;
    out->allocate_zeroed = functions.allocate_zeroed;
    out->reallocate = functions.reallocate;
    out->release = functions.release;
    out->owner = functions.owner;
}
