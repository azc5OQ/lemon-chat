#ifndef LEMON_DESKTOP_OVERLAY_DATA_H
#define LEMON_DESKTOP_OVERLAY_DATA_H

#include "clib.h"

_Static_assert(sizeof(uint) == 4 && sizeof(ushort) == 2 && sizeof(uint64) == 8, "Overlay and media fields require the server's fixed-width aliases.");

#define DESKTOP_OVERLAY_MAGIC 0x4c434f56U
#define DESKTOP_OVERLAY_VERSION 3U
#define DESKTOP_OVERLAY_MEMBERS 32U
#define DESKTOP_OVERLAY_NAME 96U
#define DESKTOP_OVERLAY_SPEAKING 1U
#define DESKTOP_OVERLAY_MUTED 2U
#define DESKTOP_OVERLAY_LOCAL 4U
#define DESKTOP_OVERLAY_DEAFENED 8U
#define DESKTOP_OVERLAY_LOCAL_MUTE 16U

typedef struct desktop_overlay_member_t
{
    uint id;
    uint flags;
    char name[DESKTOP_OVERLAY_NAME];
} desktop_overlay_member_t;

/* Fixed-size values only: no pointers, audio, credentials, or allocator ownership across processes. */
typedef struct desktop_overlay_snapshot_t
{
    uint magic;
    uint version;
    uint size;
    uint owner_pid;
    uint64 heartbeat_ms;
    uint enabled;
    uint member_count;
    uint total_member_count;
    uint revision;
    uint preview;
    char channel[DESKTOP_OVERLAY_NAME];
    desktop_overlay_member_t members[DESKTOP_OVERLAY_MEMBERS];
} desktop_overlay_snapshot_t;

void desktop_overlay_data__clear(desktop_overlay_snapshot_t *snapshot);
boole desktop_overlay_data__valid(const desktop_overlay_snapshot_t *snapshot);
boole desktop_overlay_data__visible(const desktop_overlay_snapshot_t *snapshot, uint64 now_ms);
void desktop_overlay_data__preview(desktop_overlay_snapshot_t *snapshot, uint64 now_ms);

#endif
