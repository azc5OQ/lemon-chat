#ifndef CHANNEL_H
#define CHANNEL_H

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

uint64 channel__get_client_count_for_channel(uint64 channel_id);
void channel__mark_channels_for_deletion(uint64 channel_id, uint64* out_current_index, uint64* out_channel_indices);
client_t* channel__find_music_bot_in_channel(uint64 channel_id);
void channel__destroy_temp_channel(uint64 temp_channel_id);
void channel__move_client_into_channel(uint64 client_id, uint64 destination_channel_id);
uint64 channel__get_other_clients_in_channel(int client_to_ignore, uint64 channel_id, int64* out_receiving_client_ids);
boole channel__check_channel_access(client_t* client, channel_t* channel, char* password, boole password_already_proven);

boole channel__replace_departed_maintainer(channel_t* channel, uint64 client_id);
void channel__welcome_client(client_t* client);

boole channel__find_new_maintainer_for_channel(uint64* _out__new_index_of_maintainer, uint64 channel_id, uint64 index_of_client_that_left, boole do_not_include_client_that_left_when_searching_for_new_maintainer);

void channel__send_hidden_members(client_t* client, channel_t* channel);

void channel__set_maintainer(channel_t* channel, int64 client_id);

#endif
