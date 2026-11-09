#ifndef LEMON_DESKTOP_OVERLAY_IPC_H
#define LEMON_DESKTOP_OVERLAY_IPC_H

#include "clib.h"
#include "overlay_data.h"

typedef struct desktop_overlay_ipc_t desktop_overlay_ipc_t;

desktop_overlay_ipc_t *desktop_overlay_ipc__open(uint game_pid, boole writer);
void desktop_overlay_ipc__close(desktop_overlay_ipc_t *ipc);
boole desktop_overlay_ipc__write(desktop_overlay_ipc_t *ipc, const desktop_overlay_snapshot_t *snapshot);
boole desktop_overlay_ipc__read(desktop_overlay_ipc_t *ipc, desktop_overlay_snapshot_t *snapshot);

#endif
