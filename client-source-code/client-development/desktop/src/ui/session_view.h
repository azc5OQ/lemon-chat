#ifndef LEMON_DESKTOP_SESSION_VIEW_H
#define LEMON_DESKTOP_SESSION_VIEW_H

#include "clib.h"
#include "session/session.h"

void desktop_session_view__update(const desktop_session_view_t* view, boole force);
boole desktop_session_view__open(const desktop_client_t* client);
void desktop_session_view__select(int64 tab);
void desktop_session_view__select_member(const desktop_client_t* client);
void desktop_session_view__select_channel(const desktop_channel_t* channel);
void desktop_session_view__close(void);
void desktop_session_view__close_tab(int64 tab);
int64 desktop_session_view__peer(uint64* serial);
uint64 desktop_session_view__unread(const desktop_client_t* client);
void desktop_session_view__shutdown(void);

#endif
