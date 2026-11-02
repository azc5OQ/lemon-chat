#ifndef LEMON_DESKTOP_TRANSPORT_H
#define LEMON_DESKTOP_TRANSPORT_H

#include "clib.h"
#include "network/message_queue.h"

typedef enum desktop_transport_state_t
{
    DESKTOP_TRANSPORT_DISCONNECTED,
    DESKTOP_TRANSPORT_CONNECTING,
    DESKTOP_TRANSPORT_CONNECTED,
    DESKTOP_TRANSPORT_FAILED
} desktop_transport_state_t;

typedef struct desktop_transport_status_t
{
    desktop_transport_state_t state;
    boole finished;
    uint64 messages_received;
    char detail[256];
    char last_type[64];
} desktop_transport_status_t;

typedef struct desktop_transport_t desktop_transport_t;

desktop_transport_t* desktop_transport__create(void);
void desktop_transport__destroy(desktop_transport_t* transport);
/* Starts a worker; the GUI thread never performs a connection or receive. */
boole desktop_transport__connect(desktop_transport_t* transport, const char* url);
/* Requests cancellation without waiting. Destroy joins before releasing storage. */
void desktop_transport__disconnect(desktop_transport_t* transport);
void desktop_transport__status(desktop_transport_t* transport, desktop_transport_status_t* status);
/* Copies into a bounded queue. The connection worker owns all network operations. */
boole desktop_transport__send(desktop_transport_t* transport, const char* bytes, uint64 size);
boole desktop_transport__send_binary(desktop_transport_t* transport, const unsigned char* bytes, uint64 size);
boole desktop_transport__send_media(desktop_transport_t* transport, const unsigned char* bytes, uint64 size, uint64 expires);
void desktop_transport__discard_media(desktop_transport_t* transport);
boole desktop_transport__receive(desktop_transport_t* transport, desktop_message_t* message);

#endif
