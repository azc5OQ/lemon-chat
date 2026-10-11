#ifndef AUDIO_WEBSOCKET_H
#define AUDIO_WEBSOCKET_H 1

void audio_websocket__request(client_t* client);
void audio_websocket__onopen(ws_cli_conn_t* socket);
void audio_websocket__onmessage(ws_cli_conn_t* socket, unsigned char* data, uint64 size, int type);
void audio_websocket__onclose(ws_cli_conn_t* socket);
void audio_websocket__disconnect(client_t* client);
void audio_websocket__clear_queue(client_t* client);
boole audio_websocket__send(client_t* client, const unsigned char* data, uint64 size, boole is_music);
boole audio_websocket__owns_socket(client_t* client, ws_cli_conn_t* socket);

#endif
