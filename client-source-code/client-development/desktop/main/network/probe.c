// Small TCP WebSocket echo probe for development endpoints.
#include "clib.h"
#include "platform/system.h"
#include <stdio.h>
#include "network/transport.h"

/**
 * @brief runs a bounded echo probe against a development endpoint.
 *
 * @param int argc -> command-line argument count
 * @param char** argv -> command-line arguments
 *
 * @return int -> 0 on success; nonzero on failure
 */
int main(int argc, char **argv)
{
    static const char probe[] = "{\"message\":{\"type\":\"desktop_transport_probe\"}}";
    desktop_transport_t *transport = NULL;
    boole sent = 0;
    int result = 0;
    int64 attempt = 0;
    desktop_transport_status_t status = { 0 };

    if (argc != 2)
    {
        fprintf(stderr, "Usage: lemon-chat-probe ws://host:port/path\nUse with a test WebSocket echo endpoint, not an authenticated chat session.\n");
        return 2;
    }
    transport = desktop_transport__create();
    if (!transport || !desktop_transport__connect(transport, argv[1]))
    {
        desktop_transport__destroy(transport);
        return 2;
    }
    sent = 0;
    result = 1;
    for (attempt = 0; attempt < 500; attempt++)
    {
        clib__null_memory(&status, sizeof(status));
        desktop_transport__status(transport, &status);
        if (status.state == DESKTOP_TRANSPORT_CONNECTED && !sent)
        {
            sent = desktop_transport__send(transport, probe, sizeof(probe) - 1);
            if (!sent)
            {
                break;
            }
        }
        if (status.messages_received)
        {
            printf("Received message type: %s\n", status.last_type);
            result = 0;
            break;
        }
        if (status.state == DESKTOP_TRANSPORT_FAILED || status.state == DESKTOP_TRANSPORT_DISCONNECTED)
        {
            fprintf(stderr, "%s\n", status.detail);
            break;
        }
        desktop_system__sleep(20);
    }
    desktop_transport__destroy(transport);
    return result;
}
