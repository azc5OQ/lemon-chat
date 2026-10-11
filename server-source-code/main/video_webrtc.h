#ifndef VIDEO_WEBRTC_H
#define VIDEO_WEBRTC_H 1

void video_webrtc__create_data_channel(webrtc_peer_t* peer);
boole video_webrtc__is_connected(uint64 client_id);
boole video_webrtc__is_data_channel_current(uint64 client_id, int data_channel);
boole video_webrtc__can_send(uint64 client_id);
boole video_webrtc__send(uint64 client_id, const unsigned char* packet, uint64 size);

#endif
