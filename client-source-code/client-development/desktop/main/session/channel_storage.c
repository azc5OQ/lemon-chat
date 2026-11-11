#include "session/session.h"
#include "platform/memory.h"
#include <stdlib.h>

/** @brief Reserves channel records without changing count or losing existing data on failure. */
boole desktop_session__reserve_channels(desktop_session_view_t *view, int64 count)
{
    uint64 limit = SIZE_MAX / sizeof(desktop_channel_t);
    int64 capacity = 0;
    desktop_channel_t *replacement = NULL_POINTER;

    // set the count limit
    if (limit > INT_MAX)
    {
        limit = INT_MAX;
    }

    // check view and count
    if (!view || count < 0 || (uint64)count > limit)
    {
        return FALSE;
    }

    // nothing to reserve
    if (count <= view->channel_capacity)
    {
        return TRUE;
    }

    // choose the new capacity
    capacity = view->channel_capacity > 0 ? view->channel_capacity : 16;
    while (capacity < count)
    {
        if ((uint64)capacity > limit / 2)
        {
            capacity = count;
            break;
        }
        capacity *= 2;
    }

    // allocate the larger array
    replacement = realloc(view->channels, (size_t)capacity * sizeof(*replacement));
    if (!replacement)
    {
        return FALSE;
    }

    // clear the added records
    clib__null_memory(replacement + view->channel_capacity, (capacity - view->channel_capacity) * sizeof(*replacement));

    // store the new array
    view->channels = replacement;
    view->channel_capacity = capacity;
    return TRUE;
}

/** @brief Releases only a view's owned channel storage; safe to call repeatedly. */
void desktop_session__view_release(desktop_session_view_t *view)
{
    // erase and free channels
    if (view)
    {
        desktop_memory__erase(view->channels, (uint64)view->channel_capacity * sizeof(*view->channels));
        free(view->channels);
        view->channels = NULL;
        view->channel_capacity = 0;
        view->channel_count = 0;
    }
}

/** @brief Deep-copies into a zero-initialized or previously owned view; failure retains the old view. */
boole desktop_session__view_copy(desktop_session_view_t *destination, const desktop_session_view_t *source)
{
    desktop_channel_t *channels = NULL_POINTER;
    int64 capacity = 0;

    // check both views
    if (!destination || !source || source->channel_count < 0 || (source->channel_count > 0 && !source->channels))
    {
        return FALSE;
    }

    // nothing to copy
    if (destination == source)
    {
        return TRUE;
    }

    // reserve the channel storage
    if (!desktop_session__reserve_channels(destination, source->channel_count))
    {
        return FALSE;
    }

    // copy the channel records
    if (source->channel_count == 0)
    {
        desktop_session__view_release(destination);
    }
    else
    {
        clib__memcpy(destination->channels, source->channels, source->channel_count * sizeof(*source->channels));
    }

    // copy the remaining fields
    channels = destination->channels;
    capacity = destination->channel_capacity;
    *destination = *source;
    destination->channels = channels;
    destination->channel_capacity = capacity;
    return TRUE;
}

/** @brief Compares snapshot values and active channel records without comparing allocation addresses. */
boole desktop_session__view_equal(const desktop_session_view_t *left, const desktop_session_view_t *right)
{
    return clib__memcmp(left, right, offsetof(desktop_session_view_t, channels)) == 0 && (left->channel_count == 0 || clib__memcmp(left->channels, right->channels, left->channel_count * sizeof(*left->channels)) == 0);
}
