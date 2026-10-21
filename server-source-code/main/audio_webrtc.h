#ifndef AUDIO_WEBRTC_H
#define AUDIO_WEBRTC_H 1

extern webrtc_peer_t* g_webrtc_peers_array;

boole audio_webrtc__initialize_datachannel_connection(client_t* client);
void audio_webrtc__set_remote_description(client_t* client, cstring sdp, cstring type);
void audio_webrtc__add_remote_candidate(client_t* client, cstring candidate);
void audio_webrtc__set_is_client_sending_audio(uint64 client_id, boole is_active);
void audio_webrtc__process_client_channel_join(client_t* client);
void audio_webrtc__process_client_disconnect(client_t* client);

boole audio_webrtc__is_connected(uint64 client_id);
boole audio_webrtc__is_data_channel_current(uint64 client_id, int data_channel);
boole audio_webrtc__send(uint64 client_id, const unsigned char* packet, uint64 size);

#endif
