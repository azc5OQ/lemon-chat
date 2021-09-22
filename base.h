#ifndef MAIN_H
#define MAIN_H

//#define DEBUG_PROGRAM 1

struct key_t
{
    int   		key_type;
    unsigned char  	key[32];
    unsigned char  	iv[16];
};

struct server_settings_t
{
    int keys_count;
    struct key_t keys[100];
    int encryption_algoryhm_choice;
    bool is_logging_of_failed_attempts_active;
    char client_verificaton_message_cleartext[1024]; //should be enough
    int port_number_WebSocket;
    int websocket_message_max_length;
    int websocket_chat_message_string_max_length;
    int chat_cooldown_milliseconds;
    char default_client_name[30];
};

struct client_t
{
    int user_id;
    int fd;
    bool is_authenticated;
    unsigned long long timestamp_connected;
    unsigned long long timestamp_last_sent_chat_message;
    unsigned long long timestamp_username_changed;
    unsigned long long timestamp_last_maintain_connection_message_received;
    char username[100];
    char public_key[1000];
};

struct message_t
{
    unsigned long long timestamp_sent;
    int id_sender;
};


void init_setup();
void setup_server(struct server_settings_t* server_settings);

void onopen(int fd);
void onclose(int fd);
void onmessage(int fd, const unsigned char *msg, uint64_t size, int type);

void process_authenticated_client_message(int fd, int client_index,unsigned char *decrypted_websocket_data_buffer,uint64_t size,int type);
void process_not_authenticated_client_message(int fd, int index, unsigned char *decrypted_websocket_data_buffer, uint64_t size, int type);


char* encrypt_websocket_msg(unsigned char* msg, int* out_allocated_buffer_size);
void decrypt_websocket_msg(const unsigned char *msg, char* out_buffer, int out_buffer_length);

//
//broadcast functions send data to all connected clients
//

void broadcast_server_info(char* info);
void broadcast_client_connect(struct client_t* client);
void broadcast_client_disconnect(struct client_t* client);
void broadcast_username_change(struct client_t* client);

void send_channel_chat_picture(char* username, char* chat_picture_base64, int picture_id);
void send_channel_chat_picture_meta_information(char* username, size_t picture_size);
void send_channel_chat_message(char* username, char* chat_message_value);

void send_direct_chat_message(char* username, char* chat_message_value, int sender_id, int receiver_id);
void send_direct_chat_picture_meta_information(char* username, size_t picture_size, int picture_id, int sender_id, int receiver_id);
void send_direct_chat_picture(char* username, char* chat_picture_base64, int picture_id, int sender_id, int receiver_id);

void send_client_list_to_client(struct client_t* client);


#endif
