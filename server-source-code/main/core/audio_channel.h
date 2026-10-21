#ifndef AUDIO_CHANNEL_H
#define AUDIO_CHANNEL_H 1

#define AUDIO_PACKET_MAX 4096

const char* audio_channel__policy_value(void);
void audio_channel__apply_policy_change(void);
void audio_channel__relay_voice(uint64 sender_id, ws_cli_conn_t* socket, int data_channel, const unsigned char* data, uint64 size);
void audio_channel__send_music_bot_data(uint64 sender_music_bot_client_id, uint64 channel_id, uint64 sequence_number, unsigned char* data, int length);
void audio_channel__process_client_channel_join(client_t* client);
void audio_channel__process_client_disconnect(client_t* client);

#endif
