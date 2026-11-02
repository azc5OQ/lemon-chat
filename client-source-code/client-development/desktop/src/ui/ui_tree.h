#ifndef LEMON_DESKTOP_UI_TREE_H
#define LEMON_DESKTOP_UI_TREE_H

#include "session/session.h"

typedef struct gui_node_t gui_node_t;

void desktop_ui_tree__initialize(desktop_session_t* session, const desktop_session_view_t* view);
void desktop_ui_tree__begin(const desktop_session_view_t* view);
void* desktop_ui_tree__prepare_icon(int64 id);
void desktop_ui_tree__member(gui_node_t* row, const desktop_session_view_t* view, int64 member, int64 depth);
void desktop_ui_tree__channel(gui_node_t* row, const desktop_channel_t* channel, int64 depth);
boole desktop_ui_tree__tick(boole visible);
void desktop_ui_tree__shutdown(void);

#endif
