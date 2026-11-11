// Initializes and validates the bounded display format used across process boundaries.
#include "overlay_data.h"
#include "clib.h"

/**
 * @brief clears a snapshot and initializes its versioned, disabled envelope.
 *
 * @param desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 *
 * @return void
 */
void desktop_overlay_data__clear(desktop_overlay_snapshot_t *snapshot)
{
    // reset the snapshot fields
    clib__null_memory(snapshot, sizeof(*snapshot));
    snapshot->magic = DESKTOP_OVERLAY_MAGIC;
    snapshot->version = DESKTOP_OVERLAY_VERSION;
    snapshot->size = sizeof(*snapshot);
    clib__string_copy(snapshot->channel, sizeof(snapshot->channel), "Not connected");
}

/**
 * @brief checks the snapshot version, bounds and string terminators before it is used.
 *
 * @param const desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 *
 * @return boole -> 1 for a valid snapshot; 0 otherwise
 */
boole desktop_overlay_data__valid(const desktop_overlay_snapshot_t *snapshot)
{
    uint index = 0;

    // check the header fields
    if (!snapshot || snapshot->magic != DESKTOP_OVERLAY_MAGIC || snapshot->version != DESKTOP_OVERLAY_VERSION || snapshot->size != sizeof(*snapshot) || snapshot->member_count > DESKTOP_OVERLAY_MEMBERS || snapshot->total_member_count < snapshot->member_count || snapshot->enabled > 1 || snapshot->preview > 1 || !clib__memchr(snapshot->channel, 0, sizeof(snapshot->channel)))
    {
        return 0;
    }

    // check every member
    for (index = 0; index < snapshot->member_count; index++)
    {
        if (!clib__memchr(snapshot->members[index].name, 0, sizeof(snapshot->members[index].name)) || (snapshot->members[index].flags & ~(DESKTOP_OVERLAY_SPEAKING | DESKTOP_OVERLAY_MUTED | DESKTOP_OVERLAY_LOCAL | DESKTOP_OVERLAY_DEAFENED | DESKTOP_OVERLAY_LOCAL_MUTE)))
        {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief rejects disabled, invalid, stale or future-dated display snapshots.
 *
 * @param const desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 * @param uint64 now_ms -> monotonic time in milliseconds
 *
 * @return boole -> 1 when the snapshot may be drawn; 0 otherwise
 */
boole desktop_overlay_data__visible(const desktop_overlay_snapshot_t *snapshot, uint64 now_ms)
{
    return desktop_overlay_data__valid(snapshot) && snapshot->enabled && snapshot->heartbeat_ms <= now_ms && now_ms - snapshot->heartbeat_ms < 2000;
}

/**
 * @brief builds a marked sample channel with a rotating speaking indicator.
 *
 * @param desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 * @param uint64 now_ms -> monotonic time in milliseconds
 *
 * @return void
 */
void desktop_overlay_data__preview(desktop_overlay_snapshot_t *snapshot, uint64 now_ms)
{
    const char *names[] = { "You", "Alice", "Sam", "Taylor", "Jordan", "Morgan" };
    uint index = 0;

    // set the sample channel
    desktop_overlay_data__clear(snapshot);
    snapshot->preview = 1;
    snapshot->member_count = 6;
    snapshot->total_member_count = 6;
    snapshot->revision = (uint)(now_ms / 1100);
    clib__string_copy(snapshot->channel, sizeof(snapshot->channel), "Gaming / squad");

    // add the sample members
    for (index = 0; index < snapshot->member_count; index++)
    {
        snapshot->members[index].id = index + 1;
        snapshot->members[index].flags = index == 0 ? DESKTOP_OVERLAY_LOCAL : 0;
        if (snapshot->revision % 4 == index)
        {
            snapshot->members[index].flags |= DESKTOP_OVERLAY_SPEAKING;
        }

        // set the muted members
        if (index == 3)
        {
            snapshot->members[index].flags = DESKTOP_OVERLAY_MUTED;
        }
        else if (index == 4)
        {
            snapshot->members[index].flags = DESKTOP_OVERLAY_MUTED | DESKTOP_OVERLAY_DEAFENED;
        }
        else if (index == 5)
        {
            snapshot->members[index].flags = DESKTOP_OVERLAY_LOCAL_MUTE;
        }

        // copy the member name
        clib__string_copy(snapshot->members[index].name, sizeof(snapshot->members[index].name), names[index]);
    }
}
