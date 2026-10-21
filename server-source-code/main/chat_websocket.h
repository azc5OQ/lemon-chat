#ifndef CHAT_WEBSOCKET_H
#define CHAT_WEBSOCKET_H

#include "definitions.h"

void* chat_websocket__listen_thread(void* argument);
void* chat_websocket__check_connections_thread(void* argument);

#endif
