#include "platform/memory.h"
#include "audio/devices_native.h"
#include "platform/thread.h"
#include "platform/system.h"
#include <stdlib.h>

typedef struct desktop_device_catalog_t
{
    desktop_devices_view_t view;
    desktop_device_selection_t selections[DESKTOP_DEVICE_LIMIT];
} desktop_device_catalog_t;

struct desktop_devices_t
{
    desktop_mutex_t *mutex;
    desktop_thread_t *thread;
    boole stop;
    boole requested;
    desktop_device_catalog_t catalog;
};

// declarations
static void _devices_internal__worker(void *context);
static void _devices_internal__append(desktop_device_catalog_t *catalog, ma_backend backend, const ma_device_info *info, boole capture);

/**
 * @brief encodes only the active native ID field, excluding union padding and device display names.
 * @param ma_backend backend -> backend that supplied the ID
 * @param const ma_device_id* id -> borrowed native device ID
 * @param char* key -> destination; empty on failure
 * @param uint64 capacity -> actual destination capacity
 * @return boole -> TRUE for a supported, complete key
 */
boole desktop_device_key__make(ma_backend backend, const ma_device_id *id, char *key, uint64 capacity)
{
    static const char hex[] = "0123456789abcdef";
    const char *tag = NULL_POINTER;
    const ubyte *bytes = NULL_POINTER;
    const char *text = NULL_POINTER;
    const char *end = NULL_POINTER;
    ubyte number[4] = { 0 };
    uint64 length = 0;
    uint64 maximum = 0;
    uint64 offset = 0;
    uint64 index = 0;
    uint value = 0;

    // check the destination
    if (!key || !capacity)
    {
        return FALSE;
    }

    // clear the key
    key[0] = 0;

    // check the device ID
    if (!id)
    {
        return FALSE;
    }

    // choose the ID field
    switch (backend)
    {
    case ma_backend_wasapi:
        // count the ID characters
        tag = "wasapi:";
        while (length < 64 && id->wasapi[length])
        {
            length++;
        }

        // check length and capacity
        if (!length || length == 64 || capacity < 8 + length * 4)
        {
            return FALSE;
        }

        // write the WASAPI key
        clib__string_copy(key, capacity, tag);
        offset = 7;
        for (index = 0; index < length; index++)
        {
            value = id->wasapi[index];
            key[offset++] = hex[(value >> 4) & 15];
            key[offset++] = hex[value & 15];
            key[offset++] = hex[(value >> 12) & 15];
            key[offset++] = hex[(value >> 8) & 15];
        }
        key[offset] = 0;
        return TRUE;
    case ma_backend_dsound:
        tag = "dsound:";
        bytes = id->dsound;
        length = sizeof(id->dsound);
        break;
    case ma_backend_winmm:
        tag = "winmm:";
        for (index = 0; index < 4; index++)
        {
            number[index] = (ubyte)(id->winmm >> (index * 8));
        }
        bytes = number;
        length = sizeof(number);
        break;
    case ma_backend_coreaudio:
        tag = "coreaudio:";
        text = id->coreaudio;
        maximum = sizeof(id->coreaudio);
        break;
    case ma_backend_pulseaudio:
        tag = "pulse:";
        text = id->pulse;
        maximum = sizeof(id->pulse);
        break;
    case ma_backend_alsa:
        tag = "alsa:";
        text = id->alsa;
        maximum = sizeof(id->alsa);
        break;
    case ma_backend_sndio:
        tag = "sndio:";
        text = id->sndio;
        maximum = sizeof(id->sndio);
        break;
    case ma_backend_audio4:
        tag = "audio4:";
        text = id->audio4;
        maximum = sizeof(id->audio4);
        break;
    case ma_backend_oss:
        tag = "oss:";
        text = id->oss;
        maximum = sizeof(id->oss);
        break;
    default:
        return FALSE;
    }

    // find the text length
    if (text)
    {
        end = clib__memchr(text, 0, maximum);
        if (!end || end == text)
        {
            return FALSE;
        }
        length = (uint64)(end - text);
        bytes = (const ubyte *)text;
    }

    // check the key capacity
    offset = (uint64)clib__strlen(tag);
    if (capacity < offset + length * 2 + 1)
    {
        return FALSE;
    }

    // write the hex key
    clib__string_copy(key, capacity, tag);
    for (index = 0; index < length; index++)
    {
        key[offset++] = hex[bytes[index] >> 4];
        key[offset++] = hex[bytes[index] & 15];
    }
    key[offset] = 0;
    return TRUE;
}

/**
 * @brief copies a bounded device record while enumeration storage is still valid.
 * @param desktop_device_catalog_t* catalog -> unpublished worker-owned catalog
 * @param ma_backend backend -> enumeration backend
 * @param const ma_device_info* info -> borrowed basic device information
 * @param boole capture -> TRUE for a microphone, FALSE for an output
 * @return void
 */
static void _devices_internal__append(desktop_device_catalog_t *catalog, ma_backend backend, const ma_device_info *info, boole capture)
{
    desktop_device_t *item = NULL_POINTER;
    int64 index = 0;

    // check the device limit
    if (catalog->view.count >= DESKTOP_DEVICE_LIMIT)
    {
        catalog->view.truncated = TRUE;
        return;
    }

    // build the device key
    item = &catalog->view.items[catalog->view.count];
    if (!desktop_device_key__make(backend, &info->id, item->key, sizeof(item->key)))
    {
        return;
    }

    // skip a repeated device
    for (index = 0; index < catalog->view.count; index++)
    {
        if (catalog->view.items[index].capture == capture && clib__strcmp(item->key, catalog->view.items[index].key) == 0)
        {
            clib__null_memory(item, sizeof(*item));
            return;
        }
    }

    // store the device record
    clib__string_copy_n(item->name, sizeof(item->name), info->name, sizeof(info->name) - 1);
    item->capture = capture;
    item->default_device = info->isDefault != 0;
    catalog->selections[catalog->view.count].backend = backend;
    catalog->selections[catalog->view.count].id = info->id;
    catalog->view.count++;
}

/**
 * @brief enumerates on a separate worker without opening recording or playback streams.
 * @param void* context -> manager retained until this worker joins
 * @return void
 */
static void _devices_internal__worker(void *context)
{
    desktop_devices_t *devices = context;
    desktop_device_catalog_t *catalog = calloc(1, sizeof(*catalog));
    ma_context *native = calloc(1, sizeof(*native));
    ma_device_info *outputs = NULL_POINTER;
    ma_device_info *inputs = NULL_POINTER;
    ma_uint32 output_count = 0;
    ma_uint32 input_count = 0;
    uint64 index = 0;
    boole stop = FALSE;
    boole requested = FALSE;
    boole initialized = FALSE;
    uint64 revision = 0;

    // loop until stopped
    while (TRUE)
    {
        // read stop and request
        desktop_mutex__lock(devices->mutex);
        stop = devices->stop;
        requested = devices->requested;
        devices->requested = FALSE;
        desktop_mutex__unlock(devices->mutex);

        // stop was requested
        if (stop)
        {
            break;
        }

        // wait for a request
        if (!requested)
        {
            desktop_system__sleep(20);
            continue;
        }

        // clear the catalog
        if (catalog)
        {
            clib__null_memory(catalog, sizeof(*catalog));
        }

        // list the devices
        initialized = native && catalog && ma_context_init(NULL, 0, NULL, native) == MA_SUCCESS;
        if (initialized && ma_context_get_devices(native, &outputs, &output_count, &inputs, &input_count) == MA_SUCCESS)
        {
            // set the catalog flags
            catalog->view.ready = TRUE;
            catalog->view.truncated = output_count > DESKTOP_DEVICE_LIMIT / 2 || input_count > DESKTOP_DEVICE_LIMIT / 2;

            // add the outputs
            for (index = 0; index < output_count && index < DESKTOP_DEVICE_LIMIT / 2; index++)
            {
                _devices_internal__append(catalog, native->backend, &outputs[index], FALSE);
            }

            // add the inputs
            for (index = 0; index < input_count && index < DESKTOP_DEVICE_LIMIT / 2; index++)
            {
                _devices_internal__append(catalog, native->backend, &inputs[index], TRUE);
            }

            // set the detail text
            clib__string_copy(catalog->view.detail, sizeof(catalog->view.detail), catalog->view.truncated ? "Showing the first 32 devices of each kind. Choices apply when saved." : "Device choices apply when saved.");
        }
        else if (catalog)
        {
            clib__string_copy(catalog->view.detail, sizeof(catalog->view.detail), "Could not list audio devices. Choose Refresh to retry.");
        }

        // close the audio context
        if (initialized)
        {
            ma_context_uninit(native);
        }

        // store the new catalog
        desktop_mutex__lock(devices->mutex);
        revision = devices->catalog.view.revision + 1;
        if (catalog)
        {
            devices->catalog = *catalog;
        }
        else
        {
            clib__string_copy(devices->catalog.view.detail, sizeof(devices->catalog.view.detail), "Could not allocate the audio device list.");
        }
        devices->catalog.view.scanning = FALSE;
        devices->catalog.view.revision = revision;
        desktop_mutex__unlock(devices->mutex);
    }

    // erase and free memory
    if (catalog)
    {
        clib__null_memory(catalog, sizeof(*catalog));
    }
    desktop_memory__erase(catalog, sizeof(*catalog));
    free(catalog);
    desktop_memory__erase(native, sizeof(*native));
    free(native);
}

/**
 * @brief creates a lazy device-enumeration worker; FALSE never accesses native audio APIs.
 * @param boole native -> TRUE to allow native enumeration on request
 * @return desktop_devices_t* -> owned manager, or NULL
 */
desktop_devices_t *desktop_devices__create(boole native)
{
    desktop_devices_t *devices = calloc(1, sizeof(*devices));

    // check the allocation
    if (!devices)
    {
        return NULL;
    }

    // create the mutex
    devices->mutex = desktop_mutex__create();
    devices->catalog.view.revision = 1;

    // start the worker thread
    if (devices->mutex && native)
    {
        devices->thread = desktop_thread__start(_devices_internal__worker, devices);
    }

    // destroy on failure
    if (!devices->mutex || (native && !devices->thread))
    {
        desktop_devices__destroy(devices);
        return NULL;
    }

    // set the detail text
    clib__string_copy(devices->catalog.view.detail, sizeof(devices->catalog.view.detail), "Device list has not been refreshed.");
    return devices;
}

/**
 * @brief joins enumeration before freeing its catalog; callers must finish resolving devices first.
 * @param desktop_devices_t* devices -> owned manager, or NULL
 * @return void
 */
void desktop_devices__destroy(desktop_devices_t *devices)
{
    // nothing to destroy
    if (!devices)
    {
        return;
    }

    // set the stop flag
    if (devices->mutex)
    {
        desktop_mutex__lock(devices->mutex);
        devices->stop = TRUE;
        desktop_mutex__unlock(devices->mutex);
    }

    // wait for the thread
    desktop_thread__join(devices->thread);

    // free the manager
    desktop_mutex__destroy(devices->mutex);
    clib__null_memory(devices, sizeof(*devices));
    desktop_memory__erase(devices, sizeof(*devices));
    free(devices);
}

/**
 * @brief copies a changed catalog and optionally requests an asynchronous refresh.
 * @param desktop_devices_t* devices -> live manager
 * @param desktop_devices_view_t* view -> caller-owned snapshot, or NULL to request only
 * @param boole refresh -> TRUE to refresh; concurrent requests coalesce
 * @return boole -> TRUE when the snapshot changed
 */
boole desktop_devices__view(desktop_devices_t *devices, desktop_devices_view_t *view, boole refresh)
{
    boole changed = FALSE;

    // lock and request refresh
    desktop_mutex__lock(devices->mutex);
    if (refresh && devices->thread && !devices->catalog.view.scanning)
    {
        devices->requested = TRUE;
        devices->catalog.view.scanning = TRUE;
        devices->catalog.view.revision++;
        clib__string_copy(devices->catalog.view.detail, sizeof(devices->catalog.view.detail), "Looking for audio devices...");
    }

    // copy the changed catalog
    if (view && view->revision != devices->catalog.view.revision)
    {
        *view = devices->catalog.view;
        changed = TRUE;
    }
    desktop_mutex__unlock(devices->mutex);
    return changed;
}

/**
 * @brief resolves a saved key only against devices actually enumerated in the requested direction.
 * @param desktop_devices_t* devices -> live manager
 * @param const char* key -> terminated saved key; never interpreted as a native path
 * @param boole capture -> TRUE for input, FALSE for output
 * @param desktop_device_selection_t* selection -> native ID copied before releasing the catalog lock
 * @return boole -> TRUE for an exact match; missing devices never fall back silently
 */
boole desktop_devices__resolve(desktop_devices_t *devices, const char *key, boole capture, desktop_device_selection_t *selection)
{
    int64 index = 0;
    boole found = FALSE;

    // clear the selection
    clib__null_memory(selection, sizeof(*selection));

    // find the device key
    desktop_mutex__lock(devices->mutex);
    for (index = 0; index < devices->catalog.view.count; index++)
    {
        if (devices->catalog.view.items[index].capture == capture && clib__strcmp(devices->catalog.view.items[index].key, key) == 0)
        {
            *selection = devices->catalog.selections[index];
            found = TRUE;
            break;
        }
    }
    desktop_mutex__unlock(devices->mutex);
    return found;
}
