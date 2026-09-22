#ifndef VIDEO_STREAM_H

#define VIDEO_STREAM_H 1

// video streaming: a client streams its screen or a video file to the channel it sits in, one stream
// per channel at a time, and every other member opts in (pull). the server relays encrypted frames
// blindly on a second data channel next to the audio one and never holds a key. the full design is in
// VIDEO_STREAMING_DESIGN.md at the repo root
//
// every function below that changes state expects the caller to hold the clients write lock AND the
// channels write lock (lock order: clients, muggles, channels), because a stop touches the channel,
// the streamer, every viewer in the channel, and sends messages. the three data channel callbacks
// take their own locks; the relay callback is lock free like the audio one

boole video_stream__start(client_t* streamer, cstring source, cstring codec, int64 width, int64 height, int64 fps, cstring* out_refusal_reason);
void video_stream__stop(uint64 channel_id, cstring reason);
void video_stream__stop_all(cstring reason);
void video_stream__process_channel_toggle_changed(uint64 channel_id);
void video_stream__process_client_leaving_channel(client_t* client, uint64 old_channel_id);
void video_stream__process_client_joined_channel(client_t* client);
boole video_stream__set_viewer_watching(client_t* viewer, boole is_watching, cstring* out_refusal_reason);
boole video_stream__allow_viewer(client_t* streamer, uint64 viewer_client_id, boole is_allowed);
void video_stream__forward_keyframe_request(client_t* viewer);
void video_stream__process_transport_lost(uint64 client_id);

// the second data channel's callbacks, installed by audio_channel.c on the handle it creates. only
// visible to files that included libdatachannel's rtc.h (RTC_API comes from there)
#ifdef RTC_API
void RTC_API video_stream__datachannel_on_open_callback(int id, void* ptr);
void RTC_API video_stream__datachannel_on_closed_callback(int id, void* ptr);
void RTC_API video_stream__datachannel_on_message_callback(int id, const char* message, int size, void* ptr);
void RTC_API video_stream__datachannel_on_error_callback(int id, const char* error, void* ptr);
#endif

#endif
