#ifndef VIDEO_WEBSOCKET_H
#define VIDEO_WEBSOCKET_H 1

void video_websocket__request(client_t* client);
void video_websocket__onopen(ws_cli_conn_t* socket);
void video_websocket__onmessage(ws_cli_conn_t* socket, unsigned char* data, uint64 size, int type);
void video_websocket__onclose(ws_cli_conn_t* socket);
void video_websocket__disconnect(client_t* client);
void video_websocket__disconnect_all(void);
void video_websocket__clear_queue(client_t* client);
boole video_websocket__send(client_t* client, const unsigned char* frame, uint64 size);
boole video_websocket__owns_socket(client_t* client, ws_cli_conn_t* socket);

#endif
