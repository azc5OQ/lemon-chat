#ifndef VIDEO_CHANNEL_H
#define VIDEO_CHANNEL_H 1

boole video_channel__start(client_t* streamer, cstring source, cstring codec, int64 width, int64 height, int64 fps, boole is_sound_included, cstring* out_refusal_reason);
void video_channel__stop(uint64 channel_id, cstring reason);
void video_channel__stop_all(cstring reason);
void video_channel__process_channel_toggle_changed(uint64 channel_id);
void video_channel__process_client_leaving_channel(client_t* client, uint64 old_channel_id);
void video_channel__process_client_joined_channel(client_t* client);
boole video_channel__set_viewer_watching(client_t* viewer, boole is_watching, cstring* out_refusal_reason);
boole video_channel__allow_viewer(client_t* streamer, uint64 viewer_client_id, boole is_allowed);
void video_channel__forward_keyframe_request(client_t* viewer);
void video_channel__process_transport_lost(uint64 client_id);

void video_channel__relay_packet(uint64 sender_id, ws_cli_conn_t* socket, int data_channel, const unsigned char* data, uint64 size);

#endif
