#ifndef BASE_H
#define BASE_H

//#define DEBUG_PROGRAM 1

#ifndef TRUE
typedef int BOOL;
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define MAX_CHANNELS 100
#define MAX_CLIENTS 500

typedef struct key_data
{
    int              key_data_type;
    unsigned char    key_value[32];
    unsigned char    key_iv[16];
} key_data_t;

typedef struct server_settings
{
    int keys_count;
    key_data_t keys[100];
    BOOL is_logging_of_failed_attempts_active;
    char client_verificaton_message_cleartext[1024]; //should be enough
    int websocket_port;
    int websocket_audio_port_number;
    int websocket_message_max_length;
    int websocket_chat_message_string_max_length;
    unsigned long long chat_cooldown_milliseconds;
    unsigned long long join_channel_request_cooldown_milliseconds;
    unsigned long long delete_channel_request_cooldown_milliseconds;
    unsigned long long create_channel_request_cooldown_milliseconds;
    char default_client_name[30];
    BOOL is_voice_chat_active;
} server_settings_t;

typedef struct client
{
    int user_id;
    int fd;
    int websocket_audio_fd;
    BOOL is_authenticated;
    BOOL is_admin;
    unsigned long long timestamp_connected;
    unsigned long long timestamp_last_sent_chat_message;
    unsigned long long timestamp_username_changed;
    unsigned long long timestamp_last_maintain_connection_message_received;
    unsigned long long timestamp_last_sent_join_channel_request;
    unsigned long long timestamp_last_sent_delete_channel_request;
    unsigned long long timestamp_last_sent_create_channel_request;
    char username[100];
    char public_key[1000];
    int channel_id;
    BOOL microphone_active;
    char websocket_audio_auth_string_base64[100];
    BOOL is_audio_websocket_authenticated;
} client_t;

typedef struct channel
{
    BOOL is_channel;
    int channel_id;
    int parent_channel_id;
    char name[128];
    int current_clients;
    int max_clients;
    char password[128];
    char description[1000];
    BOOL is_using_password;
    int type;
    int maintainer_id;
} channel_t;

typedef struct message
{
    unsigned long long timestamp_sent;
    int id_sender;
} message_t;


void onopen(int fd);
void onclose(int fd);
void onmessage(int fd, const unsigned char *msg, unsigned long size, int type);

void onopen_audio(int fd);
void onclose_audio(int fd);
void onmessage_audio(int fd, const unsigned char *msg, unsigned long size, int type);


void process_authenticated_client_message(int fd, int client_index,unsigned char *decrypted_websocket_data_buffer,unsigned long long size,int type);
void process_not_authenticated_client_message(int fd, int index, unsigned char *decrypted_websocket_data_buffer, unsigned long long size, int type);

char* encrypt_websocket_msg(unsigned char* msg, int* out_allocated_buffer_size);
void decrypt_websocket_msg(const unsigned char *msg, char* out_buffer, int out_buffer_length);

void broadcast_server_info(char* info);

void broadcast_client_connect(client_t* client);
void broadcast_client_disconnect(client_t* client);
void broadcast_client_rename(client_t* client);
void broadcast_channel_join(client_t* client);
void broadcast_channel_delete(int channel_id);
void broadcast_channel_create(channel_t* channel);

void send_channel_chat_picture(int channel_id, client_t* sender, char* chat_picture_base64, int picture_id);
void send_channel_chat_picture_metadata(int channel_id, client_t* sender, unsigned long long picture_size, int picture_id);
void send_channel_chat_message(int channel_id, client_t* sender, char* chat_message_value);
void send_channel_maintainer_id(channel_t* channel);
void send_channel_microphone_usage(client_t* client);

void send_direct_chat_message(char* username, char* chat_message_value, int sender_id, int receiver_id);
void send_direct_chat_picture_metadata(char* username, unsigned long long picture_size, int picture_id, int sender_id, int receiver_id);
void send_direct_chat_picture(char* username, char* chat_picture_base64, int picture_id, int sender_id, int receiver_id);

void send_client_list_to_client(client_t* receiver);
void send_channel_list_to_client(client_t* receiver);
void send_maintainer_id_to_client(client_t* receiver, channel_t* channel);

int get_channel_index_by_channel_id(int channel_id);
void mark_channels_for_deletion(int channel_id, int* current_index, int* channel_indices);

void set_server_settings(void);
void init_channel_list(void);

#endif
