#ifndef AUDIO_CHANNEL_H

#define AUDIO_CHANNEL_H 1

extern webrtc_peer_t* g_webrtc_muggles_array;

// handles for one disconnected peer's libdatachannel objects, passed to the detached teardown thread
typedef struct webrtc_teardown_arg_t
{
    int peer_connection_handle;   // 0x0
    int data_channel_handle;      // 0x4
} webrtc_teardown_arg_t;

boole audio_channel__initialize_webrtc_datachannel_connection(client_t* client);
void audio_channel__process_sdp_answer_from_remote_peer(client_t* client, cJSON* RTCSessionDescription);
void audio_channel__process_ice_candidate_from_remote_peer(client_t* client, cJSON* json_root);
void audio_channel__set_is_client_sending_audio(uint64 client_id, boole is_active);
void audio_channel__process_client_channel_join(client_t* client);
void audio_channel__process_client_disconnect(client_t* client);
void audio_channel__data_sending_thread(void);

void audio_channel__send_music_bot_data(uint64 sender_music_bot_client_id, uint64 channel_id, uint64 sequence_number, unsigned char* data, int length);

#endif