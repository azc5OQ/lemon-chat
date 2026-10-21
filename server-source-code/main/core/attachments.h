#ifndef ATTACHMENTS_H
#define ATTACHMENTS_H

#include "../definitions.h"
#include "musicbot.h"
#include "../../third-party/dave-g-json/cJSON.h"

void file_transfer__process_channel_chat_picture(uint64 client_sender_id, uint64 local_message_id, char* message_value);
void file_transfer__process_direct_chat_picture(uint64 sender_client_id, uint64 receiver_id, uint64 local_message_id, char* message_value);
void file_transfer__process_channel_chat_file(uint64 client_sender_id, uint64 local_message_id, char* message_value, char* file_header);
void file_transfer__process_direct_chat_file(uint64 sender_client_id, uint64 receiver_id, uint64 local_message_id, char* message_value, char* file_header);

uint64 file_transfer__get_max_file_upload_length(void);

uint64 file_transfer__get_max_chat_file_encrypted_length(void);

void file_transfer__add_uploaded_song(musicbot_add_song_arg_struct_t* arguments);

#endif
