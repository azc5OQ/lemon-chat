#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/timeb.h>

#include <pthread.h>

#include "clib/clib_string.h"
#include "clib/clib_memory.h"

#include "kokke-tiny-aes-c/aes.h"

#include "zhicheng/base64.h"
#include "theldus-websocket/ws.h"
#include "dave-g-json/cJSON.h"
#include "ITH-sha/sha256.h"
#include "base.h"

//#define DEBUG_PROGRAM 1
//#define DEBUG_HEAP 1


int g_count_connected_clients = 0;

struct server_settings_t g_server_settings;
struct client_t g_client_list[MAX_CLIENTS];

int g_id_last_sent_picture = 0;

//
//finds first unused id
//ids range from 1 to 500
//returns 0 if no id was found
//

int get_client_index_with_fd(int fd)
{
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].fd == fd)
        {
            return g_client_list[i].user_id;
        }
    }
    return -1;
}


int get_new_index_for_client()
{
    int result = 0;
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].timestamp_connected == 0)
        {
            return i;
        }
    }
    return -1;
}

unsigned long long get_timestamp_ms()
{
    static struct timeb timer_msec;

    static unsigned long long timestamp_msec;
    timestamp_msec = 0;
    if(!ftime(&timer_msec))
    {
        timestamp_msec = ((unsigned long long) timer_msec.time) * 1000ll + (unsigned long long) timer_msec.millitm;
    }
    else
    {
        timestamp_msec = -1;
    }
    return timestamp_msec;
}

void send_channel_chat_picture_meta_data(char* username, size_t picture_size, int picture_id)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %s","send_channel_chat_picture_meta_data", "\n");
    #endif

    char picture_size_string[16];
    clib__null_memory(picture_size_string, sizeof(picture_size_string));
    sprintf(picture_size_string,"%zu",picture_size);

    char picture_id_string[16];
    clib__null_memory(picture_id_string, sizeof(picture_id_string));
    sprintf(picture_id_string,"%d",picture_id);

    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "chat_picture_meta_data_broadcast"); //info about type of info received
    cJSON_AddStringToObject(json_message_object1, "size", picture_size_string); //size of picture in bytes
    cJSON_AddStringToObject(json_message_object1, "picture_id", picture_id_string); //name of sender
    cJSON_AddStringToObject(json_message_object1, "username", username); //name of sender
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1); //should not this be freed?

    //#ifdef DEBUG_PROGRAM
    // printf("%s %s", json_root_object1_string , "\n");
    //#endif

    if(json_root_object1 != 0)
    {
        cJSON_Delete(json_root_object1);
        json_root_object1 = 0;
        json_message_object1 = 0;
    }

    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    if(json_root_object1_string != 0)
    {
        #ifdef DEBUG_HEAP
      printf("135 send_channel_chat_picture_meta_data free(json_root_object1_string); \n");
    #endif
        clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));
        free(json_root_object1_string);
        json_root_object1_string = 0;
    }

  #ifdef DEBUG_PROGRAM
    printf("%s %s",msg_text , "\n");
  #endif

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].is_authenticated)
        {
      //
            if(strcmp(g_client_list[i].username, username) != 0)
            {
                ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);
            }
        }
    }

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void broadcast_username_change(struct client_t* client)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "broadcast_username_change");
    cJSON_AddStringToObject(json_message_object1, "new_username", client->username);

    char user_id_string[16];
    clib__null_memory(user_id_string, sizeof(user_id_string));
    sprintf(user_id_string,"%d",client->user_id);

    cJSON_AddStringToObject(json_message_object1, "user_id", user_id_string);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(!g_client_list[i].is_authenticated)
        {
            continue;
        }

        //wont send message to ourselves
        //alternatively, could compare it to "i" which is equal to user_id
        if(g_client_list[i].user_id == client->user_id)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_client_list_to_client: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","send_client_list_to_client g_client_list[i].fd ", g_client_list[i].fd , "\n");
        #endif

        ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", g_client_list[i].fd , "\n");
        #endif
    }

    //check null pointers
    clib__null_memory(msg_text, size_of_allocated_message_buffer); //clear bytes

    free(msg_text); //free memory
    msg_text = 0; //ged rid of dangling pointer

    free(json_root_object1_string);
    json_root_object1_string = 0;

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_client_list_to_client() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }
}


void send_client_list_to_client(struct client_t* client)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %d %s", "sending client list to -> ", client->user_id, "\n");
    #endif

    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    cJSON *json_client_array = 0; //cant be craeted here, because cJSON_Delete deletes the array repeadedly in loop along with the root object
    //if just local_username could be removed and added to the array,

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(!g_client_list[i].is_authenticated)
        {
            continue;
        }

        //
        //client list information is only sent to single user.
        //single client only allows list to be retrieved once. Client discards other client_list messages, if server would send them
        //

        if(g_client_list[i].user_id != client->user_id)
        {
            continue;
        }

        json_root_object1 = 0;
        json_message_object1 = 0;
        json_client_array = 0;

        json_root_object1 = cJSON_CreateObject();
        json_message_object1 = cJSON_CreateObject();
        json_client_array = cJSON_CreateArray();

        // create array of clients
        for(int x = 0; x < MAX_CLIENTS; x++)
        {
            if(g_client_list[x].is_authenticated)
            {
                cJSON *single_client = cJSON_CreateObject();
                cJSON_AddStringToObject(single_client, "username", g_client_list[x].username);
                cJSON_AddStringToObject(single_client, "public_key", g_client_list[x].public_key);

                char id_string[16];
                clib__null_memory(id_string, sizeof(id_string));
                sprintf(id_string,"%d",x);

                cJSON_AddStringToObject(single_client, "user_id", id_string);
                cJSON_AddItemToArray(json_client_array, single_client);
            }
        }

        cJSON_AddStringToObject(json_message_object1, "type", "client_list");
        cJSON_AddItemToObject(json_message_object1, "clients", json_client_array);
        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);
        cJSON_AddStringToObject(json_message_object1, "local_username", g_client_list[i].username);

        char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

        int size_of_allocated_message_buffer = 0;
        char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_client_list_to_client: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","send_client_list_to_client g_client_list[i].fd ", g_client_list[i].fd , "\n");
        #endif
        ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", g_client_list[i].fd , "\n");
        #endif

        clib__null_memory(msg_text, size_of_allocated_message_buffer);

        free(msg_text);
        msg_text = 0;

        free(json_root_object1_string);
        json_root_object1_string = 0;

        if(json_root_object1 != 0)
        {
            #ifdef DEBUG_HEAP
            printf("send_client_list_to_client() cJSON_Delete(json_root_object1); \n");
            #endif
            cJSON_Delete(json_root_object1);
        }
    }
}


void broadcast_client_connect(struct client_t* client)
{

    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_connect");
    cJSON_AddStringToObject(json_message_object1, "username", client->username);
    cJSON_AddStringToObject(json_message_object1, "public_key", client->public_key);

    char user_id_string[16];
    clib__null_memory(user_id_string, sizeof(user_id_string));
    sprintf(user_id_string,"%d",client->user_id);



    cJSON_AddStringToObject(json_message_object1, "user_id", user_id_string);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(!g_client_list[i].is_authenticated)
        {
            continue;
        }

        if(g_client_list[i].user_id == client->user_id)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_client_list_to_client: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","send_client_list_to_client g_client_list[i].fd ", g_client_list[i].fd , "\n");
        #endif

        ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", g_client_list[i].fd , "\n");
        #endif
    }


    clib__null_memory(msg_text, size_of_allocated_message_buffer); //clear bytes
    free(msg_text); //free memory
    msg_text = 0; //ged rid of dangling pointer

    free(json_root_object1_string);
    json_root_object1_string = 0;

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_client_list_to_client() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }
}


void broadcast_client_disconnect(struct client_t* client)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_disconnect");

    char user_id_string[16];
    clib__null_memory(user_id_string, sizeof(user_id_string));
    sprintf(user_id_string,"%d",client->user_id);

    cJSON_AddStringToObject(json_message_object1, "user_id", user_id_string);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(!g_client_list[i].is_authenticated)
        {
            continue;
        }

        if(g_client_list[i].user_id == client->user_id)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_client_list_to_client: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","send_client_list_to_client g_client_list[i].fd ", g_client_list[i].fd , "\n");
        #endif

        ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", g_client_list[i].fd , "\n");
        #endif
    }

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;

    free(json_root_object1_string);
    json_root_object1_string = 0;

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_client_list_to_client() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }
}

void send_direct_chat_picture_meta_information(char* username, size_t picture_size, int picture_id, int sender_id, int receiver_id)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %s","[i] send_direct_chat_picture_meta_information() ", "\n");
    #endif

    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();

    char sender_id_string[16];
    clib__null_memory(sender_id_string, sizeof(sender_id_string));
    sprintf(sender_id_string,"%d",sender_id);

    char picture_id_string[16];
    clib__null_memory(picture_id_string, sizeof(picture_id_string));
    sprintf(picture_id_string,"%d",picture_id);

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_picture_meta_information");
    cJSON_AddStringToObject(json_message_object1, "sender_username", username);
    cJSON_AddStringToObject(json_message_object1, "sender_id", sender_id_string);
    cJSON_AddStringToObject(json_message_object1, "picture_id", picture_id_string); //name of sender

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].is_authenticated && i == receiver_id)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s %d %s", "[i] sending direct chat picture meta information to user :", i, "\n");
            #endif
            ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);
            break;
        }
    }

    cJSON_Delete(json_root_object1);

    clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));

    cJSON_free(json_root_object1_string);
    json_root_object1_string = 0;

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void send_direct_chat_picture(char* username, char* chat_picture_base64, int picture_id, int sender_id, int receiver_id)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %s","[i] send_direct_chat_message()", "\n");
    #endif

    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();

    char sender_id_string[16];
    clib__null_memory(sender_id_string, sizeof(sender_id_string));
    sprintf(sender_id_string,"%d",sender_id);

    char picture_id_string[16];
    clib__null_memory(picture_id_string, sizeof(picture_id_string));
    sprintf(picture_id_string,"%d",picture_id);

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_picture");
    cJSON_AddStringToObject(json_message_object1, "value", chat_picture_base64);
    cJSON_AddStringToObject(json_message_object1, "sender_username", username);
    cJSON_AddStringToObject(json_message_object1, "sender_id", sender_id_string);
    cJSON_AddStringToObject(json_message_object1, "picture_id", picture_id_string); //name of sender
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].is_authenticated && i == receiver_id)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s %d %s", "[i] sending direct chat picture to user :", i, "\n");
            #endif
            ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);
            break;
        }
    }

    cJSON_Delete(json_root_object1);

    cJSON_free(json_root_object1_string);
    json_root_object1_string = 0;

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}


void send_direct_chat_message(char* username, char* chat_message_value, int sender_id, int receiver_id)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %s","send_direct_chat_message", "\n");
    #endif

    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();

    char sender_id_string[16];
    clib__null_memory(sender_id_string, sizeof(sender_id_string));
    sprintf(sender_id_string,"%d",sender_id);

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_message");
    cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
    cJSON_AddStringToObject(json_message_object1, "sender_username", username);
    cJSON_AddStringToObject(json_message_object1, "sender_id", sender_id_string);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].is_authenticated && i == receiver_id)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s %d %s", "[i] sending DM to user :", i, "\n");
            #endif
            ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);
            break;
        }
    }

    cJSON_Delete(json_root_object1);

    cJSON_free(json_root_object1_string);
    json_root_object1_string = 0;

    clib__null_memory(msg_text, size_of_allocated_message_buffer); // null contents of memory

    free(msg_text); //free memory
    msg_text = 0; //null pointer
}

void send_channel_chat_message(char* username, char* chat_message_value)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %s","send_channel_chat_message", "\n");
    #endif

    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_message");
    cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
    cJSON_AddStringToObject(json_message_object1, "receiver_channel", "main");
    cJSON_AddStringToObject(json_message_object1, "sender_username", username);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].is_authenticated)
        {
            ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);
        }
    }

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_channel_chat_message() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }

    free(json_root_object1_string);
    json_root_object1_string = 0;

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void send_channel_chat_picture(char* username, char* chat_picture_base64, int picture_id)
{

    #ifdef DEBUG_PROGRAM
    printf("%s %s","send_channel_chat_picture", "\n");
    #endif

    char picture_id_string[16];
    clib__null_memory(picture_id_string, sizeof(picture_id_string));
    sprintf(picture_id_string,"%d",picture_id);

    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "chat_picture_broadcast");
    cJSON_AddStringToObject(json_message_object1, "value", chat_picture_base64);
    cJSON_AddStringToObject(json_message_object1, "username", username);
    cJSON_AddStringToObject(json_message_object1, "picture_id", picture_id_string);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    //#ifdef DEBUG_PROGRAM
    // printf("%s %s", json_root_object1_string , "\n");
    //#endif

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("476 send_channel_chat_picture() cJSON_Delete(json_root_object1); \n");
        cJSON_Delete(json_root_object1);
        #endif

        json_root_object1 = 0;
        json_message_object1 = 0;
    }

    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    if(json_root_object1_string != 0)
    {
        #ifdef DEBUG_HEAP
        printf("490 send_channel_chat_picture free(json_root_object1_string); \n");
        #endif
        clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));
        free(json_root_object1_string);
        json_root_object1_string = 0;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s %s",msg_text , "\n");
    #endif

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].is_authenticated)
        {
            if(strcmp(g_client_list[i].username, username) != 0)
            {
                ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);
            }
            else
            {
                cJSON *json_root_object2 = 0;
                cJSON *json_message_object2 = 0;

                json_root_object2 = cJSON_CreateObject();
                json_message_object2 = cJSON_CreateObject();

                cJSON_AddStringToObject(json_message_object2, "type", "chat_picture_accepted");
                cJSON_AddItemToObject(json_root_object2, "message", json_message_object2);

                char* json_root_object2_string = cJSON_PrintUnformatted(json_root_object2);

                int size_of_allocated_message_buffer = 0;
                char* msg_text2 = encrypt_websocket_msg(json_root_object2_string, &size_of_allocated_message_buffer);
                ws_sendframe(g_client_list[i].fd, msg_text2, strlen(msg_text2), false, 1);

                if(json_root_object2_string != 0)
                {
                    #ifdef DEBUG_HEAP
                    printf("send_channel_chat_picture free(json_root_object2_string); \n");
                    #endif

                    free(json_root_object2_string);
                    json_root_object2_string = 0;
                }

                if(json_root_object2 != 0)
                {
                    #ifdef DEBUG_HEAP
                    printf("send_channel_chat_picture() cJSON_free(json_root_object2); \n");
                    #endif

                    cJSON_free(json_message_object2);
                    cJSON_free(json_root_object2);
                }

                clib__null_memory(msg_text2, size_of_allocated_message_buffer);
                free(msg_text2);
                msg_text2 = 0;
            }
        }
    }

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void broadcast_server_info(char* info)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "server_info_broadcast");
    cJSON_AddStringToObject(json_message_object1, "value", info);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("broadcast_server_info() cJSON_Delete(json_root_object1); \n");
        #endif

        cJSON_Delete(json_root_object1);
        json_root_object1 = 0;
        json_message_object1 = 0;
    }

    int size_of_allocated_message_buffer = 0;
    char* msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(g_client_list[i].is_authenticated)
        {
            ws_sendframe(g_client_list[i].fd, msg_text, strlen(msg_text), false, 1);
        }
    }

    free(json_root_object1_string);
    json_root_object1_string = 0;

    clib__null_memory(msg_text, size_of_allocated_message_buffer);
    free(msg_text);
    msg_text = 0;
}

void setup_server(struct server_settings_t* server_settings)
{
    char verification_message[] = "welcome";

    clib__copy_memory(verification_message, server_settings->client_verificaton_message_cleartext, strlen(verification_message));

    server_settings->websocket_message_max_length = 50000000;
    server_settings->websocket_chat_message_string_max_length = 4000;
    server_settings->chat_cooldown_milliseconds = 100;

    char default_client_name[30] = "anon";

    clib__copy_memory(default_client_name,server_settings->default_client_name, strlen(default_client_name));
}

void onopen(int fd)
{
    int index = get_new_index_for_client();

    if(index == -1)
    {
        printf("get_new_index_for_client returned -1, closing socket");
        ws_close_client(fd);
        return;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s%d%s","[i] onopen : new client id: ", index, "\n");
    #endif

    g_client_list[index].is_authenticated = false;
    g_client_list[index].timestamp_connected = get_timestamp_ms();
    g_client_list[index].fd = fd;
    g_client_list[index].user_id = index;
    char *cli;
    cli = ws_getaddress(fd);
    #ifdef DEBUG_PROGRAM
    printf("Connection opened, client: %d | addr: %s\n", fd, cli);
    #endif
	free(cli);
    cli = 0;

    //broadcast_client_connect(&g_client_list[index]);
}

void onclose(int fd)
{
    int index = get_client_index_with_fd(fd);

    char *cli;
    cli = ws_getaddress(fd);
    #ifdef DEBUG_PROGRAM
    printf("%s%d%s%d%s", "[i] closing connection with client. client.user_id : ", index, " fd : ", fd , "\n");
    #endif
	free(cli);
    cli = 0;

    if(index != -1)
    {

        char server_info[400];
        clib__null_memory(server_info, sizeof(server_info));
        sprintf(server_info,"%s%s","[i] disconnected : ", g_client_list[index].username);
        broadcast_server_info(server_info);

        #ifdef DEBUG_PROGRAM
        printf("clib__null_memory(&g_client_list[fd], sizeof(struct client_t),0);");
        #endif

        broadcast_client_disconnect(&g_client_list[index]);

        //
        //set all bytes of client_t structure to null, nulling out every data of his
        //
        clib__null_memory(&g_client_list[index], sizeof(struct client_t));
    }
}

void process_authenticated_client_message(int fd, int index, unsigned char *decrypted_websocket_data_buffer,uint64_t size,int type)
{
    char* encrypted_data_to_send_buffer = 0;
    int size_of_allocated_message_buffer = 0;

    //#ifdef DEBUG_PROGRAM
    //printf("%s%s%s", "[i] decrypted received data: " , decrypted_websocket_data_buffer, "\n");
    // #endif

    cJSON *json_root = cJSON_Parse(decrypted_websocket_data_buffer);

    #ifdef DEBUG_HEAP
    printf("%s", "[i] free (decrypted_websocket_data_buffer); \n");
    #endif

    clib__null_memory(decrypted_websocket_data_buffer,size);
    free(decrypted_websocket_data_buffer);
    decrypted_websocket_data_buffer = 0;

    if (json_root == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_root is null\n");
        #endif

        ws_close_client(fd);
        return;
    }

    cJSON *json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    if(json_message_object == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_message_object is null\n");
        #endif

        ws_close_client(fd);
        cJSON_Delete(json_root);

        return;
    }

    if (!cJSON_IsObject(json_message_object))
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " cJSON_IsObject(json_message_object) == false\n");
        #endif
        cJSON_Delete(json_root);
        ws_close_client(fd);
        return;
    }

    const cJSON *json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
    if (!cJSON_IsString(json_message_type) || json_message_type->valuestring == NULL || strlen(json_message_type->valuestring) == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_message_type != string \n");
        #endif

        ws_close_client(fd);
        cJSON_Delete(json_root);

        return;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s%d%s%s%s","client : ", index, " json_message_type = " , json_message_type->valuestring , "\n");
    #endif

    if(strcmp(json_message_type->valuestring, "client_chat_picture") == 0)
    {
        unsigned long long timestamp_now = get_timestamp_ms();

        if(g_client_list[index].timestamp_last_sent_chat_message + g_server_settings.chat_cooldown_milliseconds > timestamp_now)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " can not sent chat message. cooldown active \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        cJSON* json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
        cJSON* receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");
        cJSON* receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id"); //can be string or id

        //check receiver type
        if (!cJSON_IsString(receiver_type) || receiver_type->valuestring == NULL || strlen(receiver_type->valuestring) == 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " receiver_type is wrong \n");
            #endif
            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        if(strcmp(receiver_type->valuestring, "user") != 0 && strcmp(receiver_type->valuestring, "channel") != 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " receiver_type is wrong \n");
            #endif
            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        if (!cJSON_IsString(json_chat_message_value) || json_chat_message_value->valuestring == NULL || strlen(json_chat_message_value->valuestring) == 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " client_chat_picture != string \n");
            #endif
            ws_close_client(fd);
            cJSON_Delete(json_root);

            return;
        }

        if(strcmp(receiver_type->valuestring, "user") == 0)
        {
            bool is_string_number = clib__is_str_number(receiver_id->valuestring);
            if(is_string_number == false)
            {
                //#ifdef DEBUG_PROGRAM
                printf("%s%d%s%s%s","[!] client : ", index, " receiver_id ", receiver_id->valuestring , "is wrong \n");
                // #endif
                ws_close_client(fd);
                cJSON_Delete(json_root);
                return;
            }
            g_client_list[index].timestamp_last_sent_chat_message = get_timestamp_ms();
            g_id_last_sent_picture++;

            int receiver_id_int = atoi(receiver_id->valuestring);

            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[i] sending direct chat picture to user ", receiver_id_int, "\n");
            #endif

            send_direct_chat_picture_meta_information(g_client_list[index].username, strlen(json_chat_message_value->valuestring),g_id_last_sent_picture,index, receiver_id_int);
            send_direct_chat_picture(g_client_list[index].username,json_chat_message_value->valuestring, g_id_last_sent_picture, index, receiver_id_int);

        }

        if(strcmp(receiver_type->valuestring, "channel") == 0)
        {
            g_client_list[index].timestamp_last_sent_chat_message = get_timestamp_ms();
            g_id_last_sent_picture++;
            send_channel_chat_picture_meta_data(g_client_list[index].username, strlen(json_chat_message_value->valuestring), g_id_last_sent_picture);
            send_channel_chat_picture(g_client_list[index].username, json_chat_message_value->valuestring, g_id_last_sent_picture);
        }

        cJSON_Delete(json_root);

        #ifdef DEBUG_HEAP
        printf("%s","cJSON_Delete(json_root); \n");
        #endif
    }
    else if(strcmp(json_message_type->valuestring, "get_client_list") == 0)
    {
        cJSON *json_root_object1 = 0;
        cJSON *json_message_object1 = 0;
        cJSON *json_client_array = 0;

        json_root_object1 = cJSON_CreateObject();
        json_message_object1 = cJSON_CreateObject();
        json_client_array = cJSON_CreateArray();

        for(int i = 0; i < MAX_CLIENTS; i++)
        {
            if(g_client_list[i].is_authenticated)
            {
                cJSON_AddItemToArray(json_client_array, cJSON_CreateString(g_client_list[i].username));
            }
        }

        cJSON_AddStringToObject(json_message_object1, "type", "client_list");
        cJSON_AddItemToObject(json_message_object1, "clients", json_client_array);
        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

        char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
        encrypted_data_to_send_buffer = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

        cJSON_Delete(json_root);
        cJSON_Delete(json_root_object1);
        cJSON_free(json_root_object1_string);

    }
    else if(strcmp(json_message_type->valuestring, "client_chat_message") == 0)
    {
        unsigned long long timestamp_now = get_timestamp_ms();

        if(g_client_list[index].timestamp_last_sent_chat_message + g_server_settings.chat_cooldown_milliseconds > timestamp_now)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " can not sent chat message. cooldown active \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        cJSON* json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
        cJSON* receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");
        cJSON* receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id"); //can be string or id

        if (!cJSON_IsString(json_chat_message_value) || json_chat_message_value->valuestring == NULL ||
        strlen(json_chat_message_value->valuestring) > g_server_settings.websocket_chat_message_string_max_length || strlen(json_chat_message_value->valuestring) == 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s",    "[!] client : ", index, " json_chat_message_value is wrong \n");
            #endif

            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        //
        //check receiver type
        //

        if (!cJSON_IsString(receiver_type) || receiver_type->valuestring == NULL || strlen(receiver_type->valuestring) == 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " receiver_type is wrong \n");
            #endif

            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        //
        //check if receiver type is channel or user
        //

        if(strcmp(receiver_type->valuestring, "user") != 0 && strcmp(receiver_type->valuestring, "channel") != 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " receiver_type is wrong \n");
            #endif

            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        //
        //check time
        //

        if(strcmp(receiver_type->valuestring, "user") == 0)
        {
            bool is_string_number = clib__is_str_number(receiver_id->valuestring);
            if(is_string_number == false)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s%s%s","[!] client : ", index, " receiver_id ", receiver_id->valuestring , "is wrong \n");
                #endif

                ws_close_client(fd);
                cJSON_Delete(json_root);
                return;
            }

            g_client_list[index].timestamp_last_sent_chat_message = get_timestamp_ms();

            int receiver_id_int = atoi(receiver_id->valuestring);

            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[i] sending direct message to user ", receiver_id_int, "\n");
            #endif

            send_direct_chat_message(g_client_list[index].username, json_chat_message_value->valuestring, index, receiver_id_int);
        }

        if(strcmp(receiver_type->valuestring, "channel") == 0)
        {
            g_client_list[index].timestamp_last_sent_chat_message = get_timestamp_ms();
            send_channel_chat_message(g_client_list[index].username, json_chat_message_value->valuestring);
        }

        cJSON_Delete(json_root);

    }
    else if(strcmp(json_message_type->valuestring, "change_client_username") == 0)
    {
        unsigned long long timestamp_now = get_timestamp_ms();

        //
        //avoid spam
        //

        if(g_client_list[index].timestamp_username_changed + 3000 > timestamp_now)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " could not change username. Cooldown active \n");
            #endif

            cJSON_Delete(json_root);
            return;
        }

        //
        //check if username is valid
        //

        cJSON *json_new_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");
        if (!cJSON_IsString(json_new_username) || json_new_username->valuestring == NULL || strlen(json_new_username->valuestring) == 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " json_new_username != string \n");
            #endif
            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        if(strlen(json_new_username->valuestring) > 50)
        {
            cJSON_Delete(json_root);
            ws_close_client(fd);
            return;
        }

        for(int i = 0; i < MAX_CLIENTS; i++)
        {
            if(!g_client_list[i].is_authenticated)
            {
                continue;
            }
            if(strcmp(json_new_username->valuestring, g_client_list[i].username) == 0)
            {
                #ifdef DEBUG_PROGRAM
                printf("[!] username already taken \n");
                #endif

                cJSON_Delete(json_root);
                return;
            }
        }

        #ifdef DEBUG_PROGRAM
        printf("%s%d%s%s%s%s%s","[i] username change for client " , index , ". ", g_client_list[index].username ," -> ", json_new_username->valuestring ,"\n");
        #endif

        char server_info[400] = {0};
        clib__null_memory(server_info, sizeof(server_info));
        sprintf(server_info,"%s%s%s%s","[i] ", g_client_list[index].username, " renamed to ", json_new_username->valuestring);
        broadcast_server_info(server_info);

        clib__null_memory(g_client_list[index].username,sizeof(g_client_list[index].username));
        clib__copy_memory(json_new_username->valuestring, g_client_list[index].username, strlen(json_new_username->valuestring));
        g_client_list[index].timestamp_username_changed = get_timestamp_ms();

        broadcast_username_change(&g_client_list[index]);

        cJSON_Delete(json_root);
    }
    else if(strcmp(json_message_type->valuestring, "client_connection_test") == 0)
    {
        unsigned long long timestamp_now = get_timestamp_ms();
        g_client_list[index].timestamp_last_maintain_connection_message_received = timestamp_now;
        cJSON_Delete(json_root);
    }
    else
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","[!] client: " , index , " sent unsupported message_type. Closing connection \n");
        #endif
        ws_close_client(fd);
        cJSON_Delete(json_root);
        return;
    }

    if(encrypted_data_to_send_buffer == 0)
    {
        return;
    }
    char *cli;
    cli = ws_getaddress(fd);

    if(cli != 0)
    {
        printf("864 free cli \n");
        free(cli);
    }

    ws_sendframe(fd, (char *)encrypted_data_to_send_buffer, strlen(encrypted_data_to_send_buffer), false, type);

    if(encrypted_data_to_send_buffer != 0)
    {
        clib__null_memory(encrypted_data_to_send_buffer, size_of_allocated_message_buffer);
        free(encrypted_data_to_send_buffer);
        encrypted_data_to_send_buffer = 0;
    }
}

void process_not_authenticated_client_message(int fd, int index, unsigned char *decrypted_websocket_data_buffer, uint64_t size, int type)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %d %s","[i] authenticating client ", fd, "\n");
    #endif

    #ifdef DEBUG_PROGRAM
    printf("%s %s %s","decrypted client verification message : ", decrypted_websocket_data_buffer, "\n");
    #endif

    cJSON *json_root = cJSON_Parse(decrypted_websocket_data_buffer);

    #ifdef DEBUG_HEAP
    printf("%s", "[i] free (decrypted_websocket_data_buffer); \n");
    #endif

    clib__null_memory(decrypted_websocket_data_buffer,size);
    free(decrypted_websocket_data_buffer);
    decrypted_websocket_data_buffer = 0;

    if (json_root == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_root is null\n");
        #endif
        ws_close_client(fd);
        return;
    }

    cJSON *json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if(json_message_object == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_message_object is null\n");
        #endif
        ws_close_client(fd);
        cJSON_Delete(json_root);
        return;
    }

    if (!cJSON_IsObject(json_message_object))
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " cJSON_IsObject(json_message_object) == false\n");
        #endif
        cJSON_Delete(json_root);

        ws_close_client(fd);
        return;
    }

    const cJSON *json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
    if (!cJSON_IsString(json_message_type) || json_message_type->valuestring == NULL || strlen(json_message_type->valuestring) == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_message_type != string \n");
        #endif

        ws_close_client(fd);
        cJSON_Delete(json_root);

        return;
    }

    if(strcmp(json_message_type->valuestring, "public_key_info") != 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " public_key_info message type required \n");
        #endif

        ws_close_client(fd);
        cJSON_Delete(json_root);
        return;
    }

    //
    //2048bit public RSA key stored in base64 format, should not have more than 600 bytes
    //

    const cJSON *json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (!cJSON_IsString(json_message_value) || json_message_value->valuestring == NULL || strlen(json_message_value->valuestring) == 0 ||  strlen(json_message_value->valuestring) > 600)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_message_type != string \n");
        #endif

        ws_close_client(fd);
        cJSON_Delete(json_root);
        return;
    }

    const cJSON *verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
    if (!cJSON_IsString(verification_string) || verification_string->valuestring == NULL || strlen(verification_string->valuestring) == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_message_type != string \n");
        #endif

        ws_close_client(fd);
        cJSON_Delete(json_root);

        return;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s%d%s%s%s","new client : ", index, " json_message_type = " , json_message_type->valuestring , "\n");
    printf("%s%d%s%s%s","new client : ", index, " json_message_value = " , json_message_value->valuestring , "\n");
    printf("%s%d%s%s%s","new client : ", index, " and_the_other_thing = " , verification_string->valuestring , "\n");
    #endif

    if(strcmp(verification_string->valuestring, g_server_settings.client_verificaton_message_cleartext) == 0)
    {
        //
        //at this point client is verified
        //give him some username, save his public key and send him list of connected clients
        //

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","[i] client ", index ," authenticated \n");
        #endif

        clib__copy_memory(g_server_settings.default_client_name ,  g_client_list[index].username, strlen(g_server_settings.default_client_name));
        char index_string[5] = {0};
        sprintf(index_string,"%d",index);
        clib__copy_memory(index_string,&g_client_list[index].username[strlen(g_client_list[index].username)], strlen(index_string));

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","connected user is: ", g_client_list[index].username, "\n");
        #endif

        clib__copy_memory(json_message_value->valuestring , g_client_list[index].public_key, strlen(json_message_value->valuestring));

        g_client_list[index].is_authenticated = true; //DO NOT set without setting up username first

        int size_of_allocated_message_buffer = 0;
        char* msg_text = encrypt_websocket_msg("auth_ok", &size_of_allocated_message_buffer);

        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","type :" , type ,"\n");
        #endif

        ws_sendframe(fd, msg_text, strlen(msg_text), false, type);

        clib__null_memory(msg_text, size_of_allocated_message_buffer);

        free(msg_text);
        msg_text = 0;

        broadcast_client_connect(&g_client_list[index]);

        send_client_list_to_client(&g_client_list[index]);

        char server_info[200];
        clib__null_memory(server_info, sizeof(server_info));
        sprintf(server_info,"%s%s","[i] connected : ", g_client_list[index].username);

        broadcast_server_info(server_info);
    }
    else
    {
        ws_close_client(fd);
    }
}

void onmessage(int fd, const unsigned char *websocket_msg, uint64_t size, int type)
{
    int index = get_client_index_with_fd(fd);

    #ifdef DEBUG_PROGRAM
    printf("%s %lu %s","onmessage() received websocket data size is : ", size ,"\n");
    #endif

    if(size > g_server_settings.websocket_message_max_length)
    {
        ws_close_client(fd);
        return;
    }

    if(size == 0)
    {
        ws_close_client(fd);
        return;
    }

    char* decrypted_websocket_data_buffer = (char*)calloc(size, sizeof(char));

    if(decrypted_websocket_data_buffer == 0)
    {
        return;
    }

    decrypt_websocket_msg(websocket_msg, decrypted_websocket_data_buffer, size);

    if(g_client_list[index].is_authenticated == false)
    {
        process_not_authenticated_client_message(fd, index, decrypted_websocket_data_buffer, size, type);
    }
    else
    {
        process_authenticated_client_message(fd, index, decrypted_websocket_data_buffer, size, type);
    }
}

void decrypt_websocket_msg(const unsigned char *msg, char* out_buffer, int out_buffer_length)
{
    #ifdef DEBUG_PROGRAM
    printf("%s %lu %s","[i] decrypt_websocket_msg ", strlen(msg), "\n");
    #endif

    zchg_base64_decode(msg, strlen(msg), out_buffer);

    struct AES_ctx ctx;

    //
    //first key used in decryption is last key used in encryption
    //

    for(int i = (g_server_settings.keys_count - 1); i >= 0; i--)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","decrypting data with key" , i, "\n");
        #endif
        AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key, g_server_settings.keys[i].iv);
        AES_CTR_xcrypt_buffer(&ctx, out_buffer, out_buffer_length);
    }
}

char* encrypt_websocket_msg(unsigned char* msg, int* out_allocated_buffer_size)
{

    int encryption_buffer_size = 0;
    int base64_out_string_size = 0;

    if(strlen(msg) < 1026)
    {
        encryption_buffer_size = 1026;
        base64_out_string_size = ((4 * encryption_buffer_size / 3) + 3) & ~3;
    }
    else
    {
        encryption_buffer_size = strlen(msg);
        base64_out_string_size = ((4 * encryption_buffer_size / 3) + 3) & ~3;
    }

    base64_out_string_size += 4;

    *out_allocated_buffer_size = base64_out_string_size;

    #ifdef DEBUG_PROGRAM
    printf("%s%d%s","encrypt_websocket_msg() encryption_buffer_size" , encryption_buffer_size, "\n");
    #endif

    #ifdef DEBUG_PROGRAM
    printf("%s%d%s","base64_out_string_size() base64_out_string_size" , base64_out_string_size, "\n");
    #endif


    char* encryption_buffer = (char*)calloc(encryption_buffer_size, sizeof(char));

    char* base64_out_string = (char*)calloc(base64_out_string_size, sizeof(char));

    clib__copy_memory(msg, encryption_buffer, strlen(msg));

    struct AES_ctx ctx;

    for(int i = 0; i < g_server_settings.keys_count; i++)
    {
        AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key, g_server_settings.keys[i].iv);
        AES_CTR_xcrypt_buffer(&ctx, encryption_buffer, encryption_buffer_size);
    }

    zchg_base64_encode((const unsigned char *)encryption_buffer, encryption_buffer_size , base64_out_string);

    //
    //encryption_buffer is freed here, base64_out_string is freed outside of function
    //

    if(encryption_buffer != 0)
    {
        clib__null_memory(encryption_buffer, encryption_buffer_size);
        free(encryption_buffer);
        encryption_buffer = 0;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s%lu%s","base64_out_string_size() strlen(base64_out_string)" , strlen(base64_out_string), "\n");
    #endif

    return base64_out_string;
}

void websocket_thread()
{
    #ifdef DEBUG_PROGRAM
    printf("%s%d%s","starting websocket server on port : ",   g_server_settings.port_number_WebSocket, "\n");
    #endif

    struct ws_events evs;
    evs.onopen    = &onopen;
    evs.onclose   = &onclose;
    evs.onmessage = &onmessage;
    ws_socket(&evs,   g_server_settings.port_number_WebSocket); /* Never returns. */
}

void websocket_check_thread()
{
    static unsigned long long timestamp_now;
    while(true)
    {
        timestamp_now = get_timestamp_ms();

        for(int i = 0; i < MAX_CLIENTS; i++)
        {
            if(g_client_list[i].timestamp_connected == 0)
            {
                continue;
            }

            if(g_client_list[i].is_authenticated == true)
            {
                unsigned long long timestamp_now = get_timestamp_ms();

                //
                //disconnect client who has not sent maintain_connection_message in given time limit
                //

                if(g_client_list[i].timestamp_last_maintain_connection_message_received + 180000 < timestamp_now)
                {
                    #ifdef DEBUG_PROGRAM
                    printf("%s%d%s","trying to disconnect client. did not receive maintain connection message : ", g_client_list[i].fd  ,"\n");
                    #endif

                    broadcast_client_disconnect(&g_client_list[i]);

                    ws_close_client(g_client_list[i].fd);
                    clib__null_memory(&g_client_list[i], sizeof(struct client_t));

                    continue;
                }

                int size_of_allocated_message_buffer = 0;
                char* msg = encrypt_websocket_msg("test", &size_of_allocated_message_buffer);
                ws_sendframe(g_client_list[i].fd, msg, strlen(msg), false, 1);
                if(msg != 0)
                {
                    clib__null_memory(msg, size_of_allocated_message_buffer);

                    free(msg);
                    msg = 0;
                }
                continue;
            }

            //
            //remove client who does not authenticate within given time limit
            //

            if(g_client_list[i].timestamp_connected + 60000 < timestamp_now)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s","trying to disconnect client : ", g_client_list[i].fd  ,"\n");
                #endif
                ws_close_client(g_client_list[i].fd);
                clib__null_memory(&g_client_list[i], sizeof(struct client_t));
            }
        }

        //
        //run every 60 seconds
        //

        sleep(60);
    }
}

void init_setup()
{
    //local input, wont check length
    char input2[10] = {0};
    printf("WebSocket port: ");
    fgets(input2, sizeof(input2), stdin);

    g_server_settings.port_number_WebSocket = strtol(input2,0,10);

    char input_number_of_keys[10] = {0};

    int number_of_keys = 0;

    printf("%s", "enter number of keys to be used. (1-100) : ");
    fgets(input_number_of_keys, sizeof(input_number_of_keys), stdin);

    number_of_keys = atoi(input_number_of_keys);

    g_server_settings.keys_count = number_of_keys;
    for(int i = 0; i < g_server_settings.keys_count; i++)
    {
        char current_key_input_string[256] = {0};
        printf("%s%d%s", "specify key " , i, " : ");
        fgets(current_key_input_string, sizeof(current_key_input_string), stdin);

    	//remove unwanted chars from string...that would prevent client from getting correct hash of string as in javascript side
        if(current_key_input_string[strlen(current_key_input_string) - 1] == 10)
        {
            current_key_input_string[strlen(current_key_input_string) - 1] = 0;
        }

        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, current_key_input_string, strlen(current_key_input_string));
        sha256_final(&ctx,  g_server_settings.keys[i].key);

        //
        //for now, IV for custom keys is staticaly defined
        //

        unsigned char custom_iv[16] =  {90,11,8,33,4,50,50,88,    8,89,200,15,24,4,15,10};

        //destination,                        source,             length
        memcpy(&g_server_settings.keys[i].iv,custom_iv,16);
    }
}

int main()
{
    clib__null_memory(&g_server_settings, sizeof(struct server_settings_t));
    init_setup();

    setup_server(&g_server_settings);

    static unsigned long thread_id = 0;
    pthread_create(&thread_id, 0, (void*)&websocket_thread, 0);

    static unsigned long thread_id1 = 0;
    pthread_create(&thread_id1, 0, (void*)&websocket_check_thread, 0);

    char input1[10] = {0};
    fgets(input1,sizeof(input1),stdin);
    printf("%s",input1);
    return 0;
}
