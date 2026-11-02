#ifndef LEMON_DESKTOP_MEMBER_VIEW_H
#define LEMON_DESKTOP_MEMBER_VIEW_H

#include "overlay_data.h"

typedef struct gui_node_t gui_node_t;

void desktop_member_view__update(gui_node_t* container, const desktop_overlay_snapshot_t* snapshot);

#endif
