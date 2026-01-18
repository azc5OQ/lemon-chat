#ifndef AUDIO_CHANNEL_H


typedef struct webrtc_peer_t
{
	int peer_connection_handle;
	int data_channel_handle;
	boole connected;
	audio_state_e last_sent_audio_state;
	boole is_sending_audio_right_now;
	int channel_id;
	int client_id;
	boole is_existing;
	char dh_shared_secret[1000];
	ws_cli_conn_t *p_ws_connection;
} webrtc_peer_t;

extern webrtc_peer_t *webrtc_muggles_array;

boole audio_channel__initialize_webrtc_datachannel_connection(client_t *client);
void audio_channel__process_sdp_answer_from_remote_peer(client_t *client, cJSON *RTCSessionDescription);
void audio_channel__process_ice_candidate_from_remote_peer(client_t *client, cJSON *json_root);
void audio_channel__set_is_client_sending_audio(int client_id, boole is_active);
void audio_channel__process_client_channel_join(client_t *client);
void audio_channel__process_client_disconnect(client_t *client);
void audio_channel__data_sending_thread(void);

#endif