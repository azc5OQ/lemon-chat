#ifndef AUDIO_CHANNEL_H

#define AUDIO_CHANNEL_H 1

extern webrtc_peer_t *webrtc_muggles_array;

boole audio_channel__initialize_webrtc_datachannel_connection(client_t *client);
void audio_channel__process_sdp_answer_from_remote_peer(client_t *client, cJSON *RTCSessionDescription);
void audio_channel__process_ice_candidate_from_remote_peer(client_t *client, cJSON *json_root);
void audio_channel__set_is_client_sending_audio(int client_id, boole is_active);
void audio_channel__process_client_channel_join(client_t *client);
void audio_channel__process_client_disconnect(client_t *client);
void audio_channel__data_sending_thread(void);

void audio_channel__send_music_bot_data(int channel_id, unsigned char *data, int length);

#endif