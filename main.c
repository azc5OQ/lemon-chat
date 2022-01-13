#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/timeb.h>

#include <pthread.h>

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "base.h"

#include "zhicheng/base64.h"
#include "theldus-websocket/ws.h"
#include "dave-g-json/cJSON.h"
#include "ITH-sha/sha256.h"
#include "kokke-tiny-aes-c/aes.h"

server_settings_t g_server_settings;
client_t clients_array[MAX_CLIENTS];
channel_t channel_array[MAX_CHANNELS];
int id_last_sent_picture = 0;


int get_client_index_of_audio_ws_fd(int fd)
{
    int i;
    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].websocket_audio_fd == fd)
        {
            return i;
        }
    }
    return -1;
}


int get_client_index_with_fd(int fd)
{
    int i;
    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].fd == fd)
        {
            return i;
        }
    }
    return -1;
}

int get_channel_index_by_channel_id(int channel_id)
{
    int result = -1;
    int i;
    for(i = 0; i < MAX_CHANNELS; i++)
    {
        if(channel_array[i].channel_id != channel_id)
        {
            continue;
        }
        if(channel_array[i].is_channel == TRUE)
        {
            result = i;
            break;
        }
    }
    return result;
}

int get_new_index_for_client()
{
    int result = 0;
    int i;
    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].timestamp_connected == 0)
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
        timestamp_msec = ((unsigned long long) timer_msec.time) * 1000 + (unsigned long long) timer_msec.millitm;
    }
    else
    {
        timestamp_msec = -1;
    }
    return timestamp_msec;
}

void broadcast_channel_create(channel_t* channel)
{
    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;

    cJSON_AddStringToObject(json_message_object1, "type", "channel_create");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "parent_channel_id", channel->parent_channel_id);
    cJSON_AddStringToObject(json_message_object1, "name", channel->name);
    cJSON_AddStringToObject(json_message_object1, "description", channel->description);
    cJSON_AddNumberToObject(json_message_object1, "maintainer_id", -1);
    cJSON_AddBoolToObject(json_message_object1, "is_using_password", channel->is_using_password);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","broadcast_channel_create() : msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","broadcast_channel_create() clients_array[i].fd ", clients_array[i].fd , "\n");
        #endif

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", clients_array[i].fd , "\n");
        #endif
    }

    //check null pointers
    clib__null_memory(msg_text, size_of_allocated_message_buffer); //clear bytes

    free(msg_text);
    msg_text = 0;

    free(json_root_object1_string);
    json_root_object1_string = 0;

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("broadcast_channel_create() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }
}

void broadcast_channel_delete(int channel_id)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_delete");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);
    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","broadcast_channel_delete() : msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","broadcast_channel_delete() clients_array[i].fd ", clients_array[i].fd , "\n");
        #endif

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", clients_array[i].fd , "\n");
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
        printf("broadcast_channel_delete() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }
}

void broadcast_client_rename(client_t* client)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_rename");
    cJSON_AddStringToObject(json_message_object1, "new_username", client->username);
    cJSON_AddNumberToObject(json_message_object1, "user_id", client->user_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        //wont send message to ourselves
        //alternatively, could compare it to "i" which is equal to user_id
        if(clients_array[i].user_id == client->user_id)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","broadcast_client_rename() : msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","broadcast_client_rename() clients_array[i].fd ", clients_array[i].fd , "\n");
        #endif

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", clients_array[i].fd , "\n");
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

void send_client_list_to_client(client_t* receiver)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    cJSON *json_client_array = 0;
    int i;
    int x;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;

    #ifdef DEBUG_PROGRAM
    printf("%s %d %s", "sending client list to -> ", receiver->user_id, "\n");
    #endif


    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        //
        //client list information is only sent to single user.
        //single client only allows list to be retrieved once. Client discards other client_list messages, if server would send them
        //

        if(clients_array[i].user_id != receiver->user_id)
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
        for(x = 0; x < MAX_CLIENTS; x++)
        {
            if(clients_array[x].is_authenticated == TRUE)
            {
                cJSON *single_client = cJSON_CreateObject();
                cJSON_AddStringToObject(single_client, "username", clients_array[x].username);
                cJSON_AddStringToObject(single_client, "public_key", clients_array[x].public_key);
                cJSON_AddNumberToObject(single_client, "channel_id", clients_array[x].channel_id);


                //wut
                //char id_string[16];
                //clib__null_memory(id_string, sizeof(id_string)); since when is index same as ID?
                //sprintf(id_string,"%d",x);

                cJSON_AddNumberToObject(single_client, "user_id", clients_array[x].user_id);
                cJSON_AddItemToArray(json_client_array, single_client);
            }
        }

        cJSON_AddStringToObject(json_message_object1, "type", "client_list");
        cJSON_AddItemToObject(json_message_object1, "clients", json_client_array);
        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);
        cJSON_AddStringToObject(json_message_object1, "local_username", clients_array[i].username);

        json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

        size_of_allocated_message_buffer = 0;
        msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_client_list_to_client: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","send_client_list_to_client clients_array[i].fd ", clients_array[i].fd , "\n");
        #endif
        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", clients_array[i].fd , "\n");
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

void send_channel_list_to_client(client_t* receiver)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    cJSON *json_channel_array = 0;
    char* json_root_object1_string = 0;
    char* msg_text = 0;
    int size_of_allocated_message_buffer = 0;
    int i;
    cJSON *single_channel = 0;

    if(receiver == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s", "send_channel_list_to_client() client == 0 \n");
        #endif
        return;
    }
    #ifdef DEBUG_PROGRAM
    printf("%s %d %s", "send_channel_list_to_client() for -> ", receiver->user_id, "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_channel_array = cJSON_CreateArray();
    json_root_object1_string = 0;
    msg_text = 0;
    size_of_allocated_message_buffer = 0;

    for(i = 0; i < MAX_CHANNELS; i++)
    {
        if(channel_array[i].is_channel == FALSE)
        {
            continue;
        }

        single_channel = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_channel, "channel_id", channel_array[i].channel_id);
        cJSON_AddNumberToObject(single_channel, "parent_channel_id", channel_array[i].parent_channel_id);
        cJSON_AddStringToObject(single_channel, "name", channel_array[i].name);
        cJSON_AddStringToObject(single_channel, "description", channel_array[i].description);
        cJSON_AddBoolToObject(single_channel, "is_using_password", channel_array[i].is_using_password);
        cJSON_AddNumberToObject(single_channel, "maintainer_id", channel_array[i].maintainer_id);
        cJSON_AddItemToArray(json_channel_array, single_channel);
    }

    cJSON_AddStringToObject(json_message_object1, "type", "channel_list");
    cJSON_AddItemToObject(json_message_object1, "channels", json_channel_array);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);
    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    #ifdef DEBUG_PROGRAM
    printf("%s %s %s","send_channel_list_to_client(): msg_text ", msg_text , "\n");
    #endif

    if(msg_text != 0)
    {
        ws_sendframe(receiver->fd, msg_text, strlen(msg_text), FALSE, 1);
        clib__null_memory(msg_text, size_of_allocated_message_buffer);
        free(msg_text);
        msg_text = 0;
    }

    if(json_root_object1_string != 0)
    {
        free(json_root_object1_string);
        json_root_object1_string = 0;
    }

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_channel_list_to_client() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }
}

void broadcast_client_connect(client_t* client)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_connect");
    cJSON_AddStringToObject(json_message_object1, "username", client->username);
    cJSON_AddStringToObject(json_message_object1, "public_key", client->public_key);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", client->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "user_id", client->user_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if(clients_array[i].user_id == client->user_id)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_client_list_to_client: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","send_client_list_to_client clients_array[i].fd ", clients_array[i].fd , "\n");
        #endif

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", clients_array[i].fd , "\n");
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

void send_channel_maintainer_id(channel_t* channel)
{
    int i;
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;


    #ifdef DEBUG_PROGRAM
        printf("%s %s","send_channel_maintainer_id", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "maintainer_id", channel->maintainer_id);
    cJSON_AddStringToObject(json_message_object1, "type", "channel_maintainer_id");
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    //#ifdef DEBUG_PROGRAM
    // printf("%s %s", json_root_object1_string , "\n");
    //#endif

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_channel_maintainer_id() cJSON_Delete(json_root_object1); \n");
        cJSON_Delete(json_root_object1);
        #endif

        json_root_object1 = 0;
        json_message_object1 = 0;
    }

    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    if(json_root_object1_string != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_channel_maintainer_id free(json_root_object1_string); \n");
        #endif
        clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));
        free(json_root_object1_string);
        json_root_object1_string = 0;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s %s",msg_text , "\n");
    #endif

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }
        if(clients_array[i].channel_id != channel->channel_id)
        {
            continue;
        }

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
    }

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void send_channel_microphone_usage(client_t* client)
{
    #ifdef DEBUG_PROGRAM
        printf("%s %s","send_channel_microphone_usage", "\n");
    #endif

    cJSON *json_root_object1 = cJSON_CreateObject();
    cJSON *json_message_object1 = cJSON_CreateObject();

    cJSON_AddNumberToObject(json_message_object1, "client_id", client->user_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", client->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "value", (int)client->microphone_active);
    cJSON_AddStringToObject(json_message_object1, "type", "channel_microphone_usage");
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    //#ifdef DEBUG_PROGRAM
    // printf("%s %s", json_root_object1_string , "\n");
    //#endif

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_channel_maintainer_id() cJSON_Delete(json_root_object1); \n");
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
        printf("send_channel_maintainer_id free(json_root_object1_string); \n");
        #endif
        clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));
        free(json_root_object1_string);
        json_root_object1_string = 0;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s %s",msg_text , "\n");
    #endif

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }
        else if (clients_array[i].channel_id != client->channel_id)
        {
            continue;
        }
    	else if (clients_array[i].user_id == client->user_id)
    	{
    	    continue;
    	}

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
    }

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void broadcast_client_disconnect(client_t* client)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;
    int y;
    BOOL is_new_maintainer_found = FALSE;


    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_disconnect");
    cJSON_AddNumberToObject(json_message_object1, "user_id", client->user_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if(clients_array[i].user_id == client->user_id)
        {
            continue;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_client_list_to_client: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","send_client_list_to_client clients_array[i].fd ", clients_array[i].fd , "\n");
        #endif

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", clients_array[i].fd , "\n");
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

    //
    //check if user that disconnected, was maintainer of one of the channels
    //

    for(i = 0; i < MAX_CHANNELS; i++)
    {
        if(channel_array[i].is_channel == FALSE)
        {
            continue;
        }
        if(channel_array[i].maintainer_id == client->user_id)
        {
            channel_array[i].maintainer_id = -1;
            #ifdef DEBUG_PROGRAM
            printf("%s%s%s%s%s","[i] broadcast_client_disconnect() client ", client->username , " was the maintainer of channel: ", channel_array[i].name ,"\n");
            #endif

            is_new_maintainer_found = FALSE;

            for(y = 0; y < MAX_CLIENTS; y++)
            {
                if(clients_array[y].is_authenticated == FALSE)
                {
                    continue;
                }
                if(channel_array[i].channel_id == clients_array[y].channel_id)
                {
                    channel_array[i].maintainer_id = clients_array[y].user_id;
                    is_new_maintainer_found = TRUE;

                    #ifdef DEBUG_PROGRAM
                    printf("%s%d%s%s%s","broadcast_client_disconnect() new maintainer of channel(id) ", channel_array[i].channel_id , " is user ", clients_array[y].username , "\n");
                    #endif

                    send_channel_maintainer_id(&channel_array[i]);

                    break;
                }
            }
            return;
        }
    }
}

void mark_channels_for_deletion(int channel_id, int* current_index, int* channel_indices)
{
    int i;

    if(current_index == 0 || channel_indices == 0)
    {
        return;
    }

    for(i = 0; i < MAX_CHANNELS; i++)
    {
        if(channel_id == channel_array[i].parent_channel_id)
        {
            channel_indices[*current_index] = i;
            *current_index += 1;
            #ifdef DEBUG_PROGRAM
            printf("%s %s %s", "need to remove channel ", channel_array[i].name, " \n");
            #endif
            mark_channels_for_deletion(channel_array[i].channel_id, current_index, channel_indices);
        }
    }
}

void broadcast_channel_join(client_t* client)
{
    int i = 0;
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_join");
    cJSON_AddNumberToObject(json_message_object1, "user_id", client->user_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", client->channel_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        //if(clients_array[i].user_id == client->user_id)
        //{
            //continue;
        //}

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","broadcast_channel_join: msg_text ", msg_text , "\n");
        #endif

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","broadcast_channel_join clients_array[i].fd ", clients_array[i].fd , "\n");
        #endif

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","test ", clients_array[i].fd , "\n");
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
        printf("broadcast_channel_join() cJSON_Delete(json_root_object1); \n");
        #endif
        cJSON_Delete(json_root_object1);
    }
}

void send_direct_chat_picture_metadata(char* username, unsigned long long picture_size, int picture_id, int sender_id, int receiver_id)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i = 0;

    #ifdef DEBUG_PROGRAM
    printf("%s %s","[i] send_direct_chat_picture_metadata() ", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_picture_metadata");
    cJSON_AddStringToObject(json_message_object1, "sender_username", username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", sender_id);
    cJSON_AddNumberToObject(json_message_object1, "picture_id", picture_id); //name of sender
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated && i == receiver_id)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s %d %s", "[i] sending direct chat picture meta information to user :", i, "\n");
            #endif
            ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
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
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i = 0;

    #ifdef DEBUG_PROGRAM
    printf("%s %s","[i] send_direct_chat_message()", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_picture");
    cJSON_AddStringToObject(json_message_object1, "value", chat_picture_base64);
    cJSON_AddStringToObject(json_message_object1, "sender_username", username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", sender_id);
    cJSON_AddNumberToObject(json_message_object1, "picture_id", picture_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == TRUE && i == receiver_id)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s %d %s", "[i] sending direct chat picture to user :", i, "\n");
            #endif
            ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
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
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;

    #ifdef DEBUG_PROGRAM
    printf("%s %s","send_direct_chat_message", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_message");
    cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
    cJSON_AddStringToObject(json_message_object1, "sender_username", username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", sender_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == TRUE && i == receiver_id)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s %d %s", "[i] sending DM to user :", i, "\n");
            #endif
            ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
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

void send_channel_chat_message(int channel_id, client_t* sender, char* chat_message_value)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;

    #ifdef DEBUG_PROGRAM
    printf("%s %s","send_channel_chat_message() ", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel_id);
    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_message");
    cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
    cJSON_AddStringToObject(json_message_object1, "sender_username", sender->username);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }
        if(clients_array[i].channel_id != channel_id)
        {
            continue;
        }

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);

        #ifdef DEBUG_PROGRAM
        printf("%s %s %s","send_channel_chat_message() sending message to -> ", clients_array[i].username , "\n");
        #endif
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

void send_maintainer_id_to_client(client_t* receiver, channel_t* channel)
{
    int i = 0;
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;

    #ifdef DEBUG_PROGRAM
        printf("%s %s","send_channel_maintainer_id", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "maintainer_id", channel->maintainer_id);
    cJSON_AddStringToObject(json_message_object1, "type", "channel_maintainer_id");
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    //#ifdef DEBUG_PROGRAM
    // printf("%s %s", json_root_object1_string , "\n");
    //#endif

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_channel_maintainer_id() cJSON_Delete(json_root_object1); \n");
        cJSON_Delete(json_root_object1);
        #endif

        json_root_object1 = 0;
        json_message_object1 = 0;
    }

    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    if(json_root_object1_string != 0)
    {
        #ifdef DEBUG_HEAP
        printf("send_channel_maintainer_id free(json_root_object1_string); \n");
        #endif
        clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));
        free(json_root_object1_string);
        json_root_object1_string = 0;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s %s",msg_text , "\n");
    #endif

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }
        if(clients_array[i].user_id != receiver->user_id)
        {
            continue;
        }

        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
    }

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void send_channel_chat_picture(int channel_id, client_t* sender, char* chat_picture_base64, int picture_id)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;
    cJSON *json_root_object2 = 0;
    cJSON *json_message_object2 = 0;
    char* json_root_object2_string = 0;
    char* msg_text2 = 0;


    #ifdef DEBUG_PROGRAM
        printf("%s %s","send_channel_chat_picture", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel_id); //useless?
    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_picture");
    cJSON_AddStringToObject(json_message_object1, "value", chat_picture_base64);
    cJSON_AddStringToObject(json_message_object1, "username", sender->username);
    cJSON_AddNumberToObject(json_message_object1, "picture_id", picture_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

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

    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

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

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }
        if(clients_array[i].channel_id != channel_id)
        {
            continue;
        }

        if(clients_array[i].user_id != sender->user_id)
        {
            ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
        }
        else
        {
            json_root_object2 = cJSON_CreateObject();
            json_message_object2 = cJSON_CreateObject();

            cJSON_AddStringToObject(json_message_object2, "type", "chat_picture_accepted");
            cJSON_AddItemToObject(json_root_object2, "message", json_message_object2);

            json_root_object2_string = cJSON_PrintUnformatted(json_root_object2);
            size_of_allocated_message_buffer = 0;
            msg_text2 = encrypt_websocket_msg(json_root_object2_string, &size_of_allocated_message_buffer);
            ws_sendframe(clients_array[i].fd, msg_text2, strlen(msg_text2), FALSE, 1);

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

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void send_channel_chat_picture_metadata(int channel_id, client_t* sender, unsigned long long picture_size, int picture_id)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = 0; //should not this be freed?
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;


    #ifdef DEBUG_PROGRAM
    printf("%s %s","send_channel_chat_picture_metadata", "\n");
    #endif

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();


    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel_id);
    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_picture_metadata");
    cJSON_AddNumberToObject(json_message_object1, "size", picture_size);
    cJSON_AddNumberToObject(json_message_object1, "picture_id", picture_id);
    cJSON_AddStringToObject(json_message_object1, "username", sender->username);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1); //should not this be freed?

    //#ifdef DEBUG_PROGRAM
    // printf("%s %s", json_root_object1_string , "\n");
    //#endif

    if(json_root_object1 != 0)
    {
        cJSON_Delete(json_root_object1);
        json_root_object1 = 0;
        json_message_object1 = 0;
    }

    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    if(json_root_object1_string != 0)
    {
        #ifdef DEBUG_HEAP
      printf("135 send_channel_chat_picture_metadata free(json_root_object1_string); \n");
    #endif
        clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));
        free(json_root_object1_string);
        json_root_object1_string = 0;
    }

  #ifdef DEBUG_PROGRAM
    printf("%s %s",msg_text , "\n");
  #endif

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }
        if(clients_array[i].channel_id != channel_id)
        {
            continue;
        }

        if(clients_array[i].user_id == sender->user_id)
        {
            continue;
        }
        ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
    }

    clib__null_memory(msg_text, size_of_allocated_message_buffer);

    free(msg_text);
    msg_text = 0;
}

void broadcast_server_info(char* info)
{
    cJSON *json_root_object1 = 0;
    cJSON *json_message_object1 = 0;
    char* json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    int i;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "server_info_broadcast");
    cJSON_AddStringToObject(json_message_object1, "value", info);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    if(json_root_object1 != 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("broadcast_server_info() cJSON_Delete(json_root_object1); \n");
        #endif

        cJSON_Delete(json_root_object1);
        json_root_object1 = 0;
        json_message_object1 = 0;
    }

    size_of_allocated_message_buffer = 0;
    msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

    for(i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients_array[i].is_authenticated == TRUE)
        {
            ws_sendframe(clients_array[i].fd, msg_text, strlen(msg_text), FALSE, 1);
        }
    }

    free(json_root_object1_string);
    json_root_object1_string = 0;

    clib__null_memory(msg_text, size_of_allocated_message_buffer);
    free(msg_text);
    msg_text = 0;
}

char* encrypt_websocket_msg(unsigned char* msg, int* out_allocated_buffer_size)
{
    int encryption_buffer_size = 0;
    int base64_out_string_size = 0;
    char* encryption_buffer = 0;
    char* base64_out_string = 0;

    struct AES_ctx ctx;
    int i = 0;


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


    encryption_buffer = (char*)calloc(encryption_buffer_size, sizeof(char));

    base64_out_string = (char*)calloc(base64_out_string_size, sizeof(char));

    clib__copy_memory(msg, encryption_buffer, strlen(msg));


    for(i = 0; i < g_server_settings.keys_count; i++)
    {
        AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, g_server_settings.keys[i].key_iv);
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

void decrypt_websocket_msg(const unsigned char *msg, char* out_buffer, int out_buffer_length)
{
    struct AES_ctx ctx;
    int i = 0;

    #ifdef DEBUG_PROGRAM
    printf("%s %lu %s","[i] decrypt_websocket_msg ", strlen(msg), "\n");
    #endif

    zchg_base64_decode(msg, strlen(msg), out_buffer);


    //
    //first key used in decryption is last key used in encryption
    //

    for(i = (g_server_settings.keys_count - 1); i >= 0; i--)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","decrypting data with key" , i, "\n");
        #endif
        AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, g_server_settings.keys[i].key_iv);
        AES_CTR_xcrypt_buffer(&ctx, out_buffer, out_buffer_length);
    }
}

void onopen(int fd)
{
    int index = get_new_index_for_client();
    char *cli;

    if(index == -1)
    {
        printf("get_new_index_for_client returned -1, closing socket");
        ws_close_client(fd);
        return;
    }

    #ifdef DEBUG_PROGRAM
    printf("%s%d%s","[i] onopen : new client id: ", index, "\n");
    #endif

    clients_array[index].is_authenticated = FALSE;
    clients_array[index].timestamp_connected = get_timestamp_ms();
    clients_array[index].fd = fd;
    clients_array[index].user_id = index;
    cli = ws_getaddress(fd);
    #ifdef DEBUG_PROGRAM
    printf("Connection opened, client: %d | addr: %s\n", fd, cli);
    #endif
	free(cli);
    cli = 0;

    //broadcast_client_connect(&clients_array[index]);
}

void onclose(int fd)
{
    int index = get_client_index_with_fd(fd);

    char *cli;
    char server_info[400];

    cli = ws_getaddress(fd);
    #ifdef DEBUG_PROGRAM
    printf("%s%d%s%d%s", "[i] closing connection with client. client.user_id : ", index, " fd : ", fd , "\n");
    #endif
	free(cli);
    cli = 0;

    if(index != -1)
    {

        clib__null_memory(server_info, sizeof(server_info));
        sprintf(server_info,"%s%s","[i] disconnected : ", clients_array[index].username);
        broadcast_server_info(server_info);

        #ifdef DEBUG_PROGRAM
        printf("clib__null_memory(&clients_array[fd], sizeof(client_t),0);");
        #endif

        broadcast_client_disconnect(&clients_array[index]);

        //
        //set all bytes of client_t structure to null, nulling out every data of his
        //
        clib__null_memory(&clients_array[index], sizeof(client_t));
    }
}

void onmessage(int fd, const unsigned char *websocket_msg, unsigned long size, int type)
{
    int index = get_client_index_with_fd(fd);
    char* decrypted_websocket_data_buffer = 0;

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

    decrypted_websocket_data_buffer = (char*)calloc(size, sizeof(char));

    if(decrypted_websocket_data_buffer == 0)
    {
        return;
    }

    decrypt_websocket_msg(websocket_msg, decrypted_websocket_data_buffer, size);

    if(clients_array[index].is_authenticated == FALSE)
    {
        process_not_authenticated_client_message(fd, index, decrypted_websocket_data_buffer, size, type);
    }
    else
    {
        process_authenticated_client_message(fd, index, decrypted_websocket_data_buffer, size, type);
    }
}

void onopen_audio(int fd)
{
    //printf("onopen_audio \n");
}

void onclose_audio(int fd)
{
    //printf("audio socket closed \n");
}

//can fd be 0?
void onmessage_audio(int fd, const unsigned char *websocket_msg, unsigned long size, int type)
{
    if(size == 0)
    {
        #ifdef DEBUG_PROGRAM
        printf("onmessage_audio closing connection");
        #endif
        ws_close_client(fd);

        return;
    }

    if(size > g_server_settings.websocket_message_max_length)
    {
        #ifdef DEBUG_PROGRAM
        printf("onmessage_audio closing connection");
        #endif
        ws_close_client(fd);
        return;
    }

    int index = get_client_index_of_audio_ws_fd(fd);

    if(index == -1)
    {
        #ifdef DEBUG_PROGRAM
        printf("index is -1");
        #endif

        char* decrypted_websocket_data_buffer = decrypted_websocket_data_buffer = (char*)calloc(size, sizeof(char));

        if(decrypted_websocket_data_buffer == 0)
        {
            return;
        }

        decrypt_websocket_msg(websocket_msg, decrypted_websocket_data_buffer, size);

        #ifdef DEBUG_PROGRAM
        printf("%s %s", decrypted_websocket_data_buffer, "\n");
        #endif

        int found_index = -1;

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (strcmp(decrypted_websocket_data_buffer, clients_array[i].websocket_audio_auth_string_base64) == 0)
            {
                found_index = i;
                break;
            }
        }

        if(found_index != -1)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%s%s", " found client with name" , clients_array[i].username, "\n");
            #endif

            clients_array[found_index].websocket_audio_fd = fd;
            clients_array[found_index].is_audio_websocket_authenticated = TRUE;


            //
            //websocket_audio_auth_string_base64 is not needed at this point
            //

            clib__null_memory(clients_array[found_index].websocket_audio_auth_string_base64, strlen(clients_array[found_index].websocket_audio_auth_string_base64));
        }
        else
        {
            #ifdef DEBUG_PROGRAM
            printf("onmessage_audio closing connection");
            ws_close_client(fd);

            #endif
        }
    }

    else
    {
        //
        // sent audio data to other clients located in same channel as found client, if they too have authenticated audio websocket
        //

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if(clients_array[i].is_audio_websocket_authenticated == FALSE)
            {
                continue;
            }

            if(i == index) //dont want to sent message to ourselve
            {
                continue;
            }

            else if (clients_array[i].channel_id != clients_array[index].channel_id)
            {
                continue;
            }

            ws_sendframe(clients_array[i].websocket_audio_fd, websocket_msg, size, FALSE, type);
        }
    }

    //printf("onmessage_audio(int fd, const unsigned char *msg, unsigned long size, int type)");
}

void process_authenticated_client_message(int fd, int index, unsigned char *decrypted_websocket_data_buffer, unsigned long long size, int type)
{
    //
    //C89 requires all local variables to be declared at the start of the function/subroutine
    //

    char* encrypted_data_to_send_buffer = 0;
    int size_of_allocated_message_buffer = 0;
    cJSON* json_root = 0;
    cJSON* json_message_object = 0;
    cJSON* json_message_type = 0;
    unsigned long long timestamp_now = 0;

    cJSON* json_chat_message_value = 0;
    cJSON* receiver_type = 0;
    cJSON* receiver_id = 0;

    BOOL is_string_number = FALSE;

    int receiver_id_int = 0;
    int channel_id_int = 0;

    cJSON* target_channel_id = 0;
    int target_channel_id_int = 0;
    int target_channel_index = 0;
    int current_channel_index = 0;
    int current_channel_maintainer_id = 0;

    BOOL is_new_maintainer_found = FALSE;

    int i = 0;
    int x = 0;
    cJSON* new_channel_name = 0;
    cJSON* parent_channel_id = 0;

    cJSON* channel_description = 0;

    cJSON* channel_password = 0;


    int parent_channel_id_int = 0;
    BOOL is_channel_found = FALSE;


    int channel_index_in_array = 0;

    int num_channels = 0;
    int* marked_channel_indices = 0;

    int index_of_channel_to_delete = 0;
    int channel_id_to_delete = 0;


    cJSON *json_new_username = 0;

    char server_info[400] = {0};


    char *cli;


    //#ifdef DEBUG_PROGRAM
    //printf("%s%s%s", "[i] decrypted received data: " , decrypted_websocket_data_buffer, "\n");
    //#endif

    json_root = cJSON_Parse(decrypted_websocket_data_buffer);

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

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

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

    json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
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

    if (strcmp(json_message_type->valuestring, "client_chat_picture") == 0)
    {
        timestamp_now = get_timestamp_ms();

        if(clients_array[index].timestamp_last_sent_chat_message + g_server_settings.chat_cooldown_milliseconds > timestamp_now)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " can not sent chat message. cooldown active \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
        receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");
        receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id"); //can be string or id

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
            is_string_number = clib__is_str_number(receiver_id->valuestring);

            if(is_string_number == FALSE)
            {
                #ifdef DEBUG_PROGRAM
                    printf("%s%d%s%s%s","[!] client : ", index, " receiver_id ", receiver_id->valuestring , "is wrong \n");
                #endif
                ws_close_client(fd);
                cJSON_Delete(json_root);
                return;
            }

            clients_array[index].timestamp_last_sent_chat_message = get_timestamp_ms();
            id_last_sent_picture++;

            receiver_id_int = atoi(receiver_id->valuestring);

            #ifdef DEBUG_PROGRAM
                printf("%s%d%s","[i] sending direct chat picture to user ", receiver_id_int, "\n");
            #endif

            send_direct_chat_picture_metadata(clients_array[index].username, strlen(json_chat_message_value->valuestring),id_last_sent_picture,index, receiver_id_int);
            send_direct_chat_picture(clients_array[index].username,json_chat_message_value->valuestring, id_last_sent_picture, index, receiver_id_int);
        }

        if(strcmp(receiver_type->valuestring, "channel") == 0)
        {
            is_string_number = clib__is_str_number(receiver_id->valuestring);
            if(is_string_number == FALSE)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s%s%s","[!] client : ", index, " channel id ", receiver_id->valuestring , "is wrong \n");
                #endif
                ws_close_client(fd);
                cJSON_Delete(json_root);
                return;
            }

            channel_id_int = atoi(receiver_id->valuestring);

            clients_array[index].timestamp_last_sent_chat_message = get_timestamp_ms();
            id_last_sent_picture++;
            send_channel_chat_picture_metadata(channel_id_int, &clients_array[index], strlen(json_chat_message_value->valuestring), id_last_sent_picture);
            send_channel_chat_picture(channel_id_int, &clients_array[index], json_chat_message_value->valuestring, id_last_sent_picture);
        }

        cJSON_Delete(json_root);

        #ifdef DEBUG_HEAP
        printf("%s","cJSON_Delete(json_root); \n");
        #endif
    }

    else if (strcmp(json_message_type->valuestring, "microphone_usage") == 0)
    {
        cJSON* message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value"); //can be string or id
        //check receiver type
        if (!cJSON_IsString(message_value) || message_value->valuestring == NULL || strlen(message_value->valuestring) == 0)
        {
            //#ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] microphone_usage message value is wrong : ", index, "\n");
            //#endif
            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }
        if (strcmp(message_value->valuestring, "used") == 0)
        {
            clients_array[index].microphone_active = TRUE;
            send_channel_microphone_usage(&clients_array[index]);
        }
        else if (strcmp(message_value->valuestring, "not_used") == 0)
        {
            clients_array[index].microphone_active = FALSE;
            send_channel_microphone_usage(&clients_array[index]);
        }
        else
        {
            printf("%s%d%s","[!] microphone_usage message value is wrong : ", index, "\n");
            //#endif
            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }
    }
    else if(strcmp(json_message_type->valuestring, "join_channel_request") == 0)
    {
        timestamp_now = get_timestamp_ms();

        if(clients_array[index].timestamp_last_sent_join_channel_request + g_server_settings.join_channel_request_cooldown_milliseconds > timestamp_now)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " can not sent chat message. cooldown active \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        clients_array[index].timestamp_last_sent_join_channel_request = timestamp_now;

        //
        //check if channel exists
        //

        target_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

        //check receiver type
        if (!cJSON_IsString(target_channel_id) || target_channel_id->valuestring == NULL || strlen(target_channel_id->valuestring) == 0 || !clib__is_str_number(target_channel_id->valuestring))
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " target_channel_id is wrong \n");
            #endif
            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        //
        //check if target channel exists
        //

        target_channel_id_int = atoi(target_channel_id->valuestring);
        target_channel_index = get_channel_index_by_channel_id(target_channel_id_int);

        if(target_channel_index == -1)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, "join_channel_request target_channel_index not found \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        //check channel password
        channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");

        //check receiver type
        if (!cJSON_IsString(channel_password) || channel_password->valuestring == NULL || strlen(channel_password->valuestring) >= 128)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " channel_password is wrong \n");
            #endif
            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }


        //
        //check if user is a maintainer of the current channel before letting him join new channel.
        //If he is, pick new maintainer from other users currently present in current channel.
        //If there are no users to pick from, set maintaner to -1
        //

        current_channel_index = get_channel_index_by_channel_id(clients_array[index].channel_id);
        current_channel_maintainer_id = channel_array[current_channel_index].maintainer_id;

        if(clients_array[index].channel_id == target_channel_id_int)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s%d%s","[!] client : ", index, "join_channel_request client is already at the channel",target_channel_id_int,"not found \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        //
        //check if target channel has password
        //

        if(channel_array[target_channel_index].is_using_password == TRUE)
        {
            if(strcmp(channel_password->valuestring, channel_array[target_channel_index].password) != 0)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s%d%s","[!] client : ", index, " wrong password for channel ",target_channel_id_int," \n");
                #endif
                cJSON_Delete(json_root);
                return;
            }
        }

        //
        //at this point it is safe to join channel
        //

        clients_array[index].channel_id = target_channel_id_int;
        broadcast_channel_join(&clients_array[index]);

        //
        //check if the user that left channel was the maintainer of channel
        //

        if(current_channel_maintainer_id == clients_array[index].user_id)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%s%s%s%s","[i] client ", clients_array[index].username , " is maintainer of channel: ", channel_array[current_channel_index].name ,"\n");
            #endif

            is_new_maintainer_found = FALSE;

            for(i = 0; i < MAX_CLIENTS; i++)
            {
                if(clients_array[i].is_authenticated == FALSE)
                {
                    continue;
                }
                if(clients_array[i].channel_id == channel_array[current_channel_index].channel_id)
                {
                    channel_array[current_channel_index].maintainer_id = clients_array[i].user_id;
                    is_new_maintainer_found = TRUE;
                    #ifdef DEBUG_PROGRAM
                    printf("%s%d%s%s%s","new maintainer of channel(id) ", channel_array[current_channel_index].channel_id , " is user ", clients_array[i].username , "\n");
                    #endif
                    send_channel_maintainer_id(&channel_array[current_channel_index]);
                    break;
                }
            }

            if(is_new_maintainer_found == FALSE)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s","new maintainer of channel(id) ", channel_array[current_channel_index].channel_id , " is no one (-1) \n");
                #endif
                channel_array[current_channel_index].maintainer_id = -1;
            }
            else
            {
                send_channel_maintainer_id(&channel_array[target_channel_index]);
            }
        }

        if(channel_array[target_channel_index].maintainer_id == -1)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","client : ", index, " if(channel_array[target_channel_index].maintainer_id == -1) \n");
            #endif
            channel_array[target_channel_index].maintainer_id = clients_array[index].user_id;
            send_channel_maintainer_id(&channel_array[target_channel_index]);
        }
        else
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","client : ", index, " send_maintainer_id_to_client() \n");
            #endif
            send_maintainer_id_to_client(&clients_array[index], &channel_array[target_channel_index]);
        }


        cJSON_Delete(json_root);
    }
    else if(strcmp(json_message_type->valuestring, "create_channel_request") == 0)
    {
        timestamp_now = get_timestamp_ms();

        if(clients_array[index].timestamp_last_sent_create_channel_request + g_server_settings.create_channel_request_cooldown_milliseconds > timestamp_now)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " can not create channel. cooldown active \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        clients_array[index].timestamp_last_sent_create_channel_request = timestamp_now;

        //
        //check if channel exists
        //

        new_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");

        //check parent_channel_id
        if (!cJSON_IsString(new_channel_name) || new_channel_name->valuestring == NULL || strlen(new_channel_name->valuestring) == 0 || strlen(new_channel_name->valuestring) >= 128)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " new_channel_name is wrong \n");
            #endif
            //ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");

        //check parent_channel_id

        if (!cJSON_IsString(parent_channel_id) || parent_channel_id->valuestring == NULL || strlen(parent_channel_id->valuestring) == 0 || !clib__is_str_number(parent_channel_id->valuestring))
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " parent_channel_id is wrong \n");
            #endif
            //ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        //check description
        channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");

        if (!cJSON_IsString(channel_description) || channel_description->valuestring == NULL || strlen(channel_description->valuestring) >= 1000)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " channel_description is wrong \n");
            #endif
            //ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        //check password
        channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");

        if (!cJSON_IsString(channel_password) || channel_password->valuestring == NULL || strlen(channel_password->valuestring) >= 128)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " channel_password is wrong \n");
            #endif
            //ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        parent_channel_id_int = atoi(parent_channel_id->valuestring);
        is_channel_found = FALSE;

        for(i = 0; i < MAX_CHANNELS; i++)
        {
            if(channel_array[i].is_channel == FALSE)
            {
                int new_channel_id = i + 1;
                channel_array[i].channel_id = new_channel_id;
                channel_array[i].parent_channel_id = parent_channel_id_int;
                channel_array[i].max_clients = MAX_CLIENTS;

                clib__copy_memory((void*)new_channel_name->valuestring, (void*)&channel_array[i].name, strlen(new_channel_name->valuestring));
                clib__copy_memory((void*)channel_description->valuestring, (void*)&channel_array[i].description, strlen(channel_description->valuestring));

                channel_array[i].type = 1;
                channel_array[i].is_channel = TRUE;
                channel_array[i].maintainer_id = -1;

                if(strlen(channel_password->valuestring) > 0)
                {
                    channel_array[i].is_using_password = TRUE;
                    clib__copy_memory((void*)channel_password->valuestring, (void*)&channel_array[i].password, strlen(channel_password->valuestring));
                }

                broadcast_channel_create(&channel_array[i]);

                break;
            }
        }

        cJSON_Delete(json_root);
    }
    else if(strcmp(json_message_type->valuestring, "delete_channel_request") == 0)
    {
        if(clients_array[index].is_admin == FALSE)
        {
            cJSON_Delete(json_root);
            return;
        }

        timestamp_now = get_timestamp_ms();

        if(clients_array[index].timestamp_last_sent_delete_channel_request + g_server_settings.delete_channel_request_cooldown_milliseconds > timestamp_now)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " can not sent chat message. cooldown active \n");
            #endif
            cJSON_Delete(json_root);
            return;
        }

        clients_array[index].timestamp_last_sent_delete_channel_request = timestamp_now;

        //
        //check if channel exists
        //

        target_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

        //check receiver type
        if (!cJSON_IsString(target_channel_id) || target_channel_id->valuestring == NULL || strlen(target_channel_id->valuestring) == 0 || !clib__is_str_number(target_channel_id->valuestring))
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " target_channel_id is wrong \n");
            #endif
            //ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        target_channel_id_int = atoi(target_channel_id->valuestring);
        is_channel_found = FALSE;
        channel_index_in_array = -1;


        if(target_channel_id_int == 1)
        {
            cJSON_Delete(json_root);
            return;
        }

        for(i = 0; i < MAX_CHANNELS; i++)
        {
            if(channel_array[i].channel_id != target_channel_id_int)
            {
                continue;
            }
            if(channel_array[i].is_channel == TRUE)
            {
                channel_index_in_array = i;
                is_channel_found = TRUE;
                break;
            }
        }

        if(is_channel_found == FALSE)
        {
            cJSON_Delete(json_root);
            return;
        }

        num_channels = 0;
        marked_channel_indices = (int*)calloc(MAX_CHANNELS, sizeof(int));
        marked_channel_indices[0] = channel_index_in_array;
        num_channels += 1;

        mark_channels_for_deletion(channel_array[channel_index_in_array].channel_id, &num_channels, marked_channel_indices);

        for(i = 0; i < num_channels; i++)
        {
            #ifdef DEBUG_PROGRAM
                printf("%s %d %s" , "deleting channel with index", marked_channel_indices[i], "\n");
            #endif

            index_of_channel_to_delete = marked_channel_indices[i];
            channel_id_to_delete = channel_array[index_of_channel_to_delete].channel_id;

            clib__null_memory(&channel_array[index_of_channel_to_delete],sizeof(channel_t));

            //
            //move clients from channel, if there are any, to root channel
            //

            for(x = 0; x < MAX_CLIENTS; x++)
            {
                if(clients_array[x].is_authenticated == FALSE)
                {
                    continue;
                }
                if(clients_array[x].channel_id == channel_id_to_delete)
                {
                    clients_array[x].channel_id = 1;
                    broadcast_channel_join(&clients_array[x]);
                }
            }

            broadcast_channel_delete(channel_id_to_delete);
        }

        clib__null_memory(marked_channel_indices, MAX_CHANNELS * sizeof(int));
        free(marked_channel_indices);
        marked_channel_indices = 0;

        cJSON_Delete(json_root);
    }
    else if(strcmp(json_message_type->valuestring, "client_chat_message") == 0)
    {
        //unsigned long long timestamp_now = get_timestamp_ms();

        //if(clients_array[index].timestamp_last_sent_chat_message + g_server_settings.chat_cooldown_milliseconds > timestamp_now)
        //{
            //#ifdef DEBUG_PROGRAM
            //printf("%s%d%s","[!] client : ", index, " can not sent chat message. cooldown active \n");
            //#endif
            //cJSON_Delete(json_root);
            //return;
        //}

        json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
        receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");
        receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id"); //must be string of certain length

        if (!cJSON_IsString(json_chat_message_value) || json_chat_message_value->valuestring == NULL ||
        strlen(json_chat_message_value->valuestring) > g_server_settings.websocket_chat_message_string_max_length || strlen(json_chat_message_value->valuestring) == 0)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s",    "[!] client : ", index, " json_chat_message_value is wrong \n");
            #endif

	    printf("%s%s", json_chat_message_value->valuestring, "\n");

            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }

        //check receiver_id

        if (!cJSON_IsString(receiver_id) || receiver_id->valuestring == NULL || strlen(receiver_id->valuestring) == 0 || strlen(receiver_id->valuestring) > 10)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[!] client : ", index, " receiver_id is wrong \n");
            #endif

            ws_close_client(fd);
            cJSON_Delete(json_root);
            return;
        }
        //
        //check receiver type
        //

        if (!cJSON_IsString(receiver_type) || receiver_type->valuestring == NULL || strlen(receiver_type->valuestring) == 0 || strlen(receiver_type->valuestring) > 10)
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
            is_string_number = clib__is_str_number(receiver_id->valuestring);
            if(is_string_number == FALSE)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s%s%s","[!] client : ", index, " receiver_id ", receiver_id->valuestring , "is wrong \n");
                #endif

                ws_close_client(fd);
                cJSON_Delete(json_root);
                return;
            }

            clients_array[index].timestamp_last_sent_chat_message = get_timestamp_ms();

            receiver_id_int = atoi(receiver_id->valuestring);

            #ifdef DEBUG_PROGRAM
            printf("%s%d%s","[i] sending direct message to user ", receiver_id_int, "\n");
            #endif

            send_direct_chat_message(clients_array[index].username, json_chat_message_value->valuestring, index, receiver_id_int);
        }

        if(strcmp(receiver_type->valuestring, "channel") == 0)
        {
            is_string_number = clib__is_str_number(receiver_id->valuestring);
            if(is_string_number == FALSE)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s%s%s","[!] client : ", index, " channel id ", receiver_id->valuestring , "is wrong \n");
                #endif
                ws_close_client(fd);
                cJSON_Delete(json_root);
                return;
            }

            channel_id_int = atoi(receiver_id->valuestring);

            clients_array[index].timestamp_last_sent_chat_message = get_timestamp_ms();
            send_channel_chat_message(channel_id_int, &clients_array[index], json_chat_message_value->valuestring);
        }

        cJSON_Delete(json_root);

    }
    else if(strcmp(json_message_type->valuestring, "change_client_username") == 0)
    {
        timestamp_now = get_timestamp_ms();

        //
        //avoid spam
        //

        if(clients_array[index].timestamp_username_changed + 3000 > timestamp_now)
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

        json_new_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");
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

        for(i = 0; i < MAX_CLIENTS; i++)
        {
            if(clients_array[i].is_authenticated == FALSE)
            {
                continue;
            }
            if(strcmp(json_new_username->valuestring, clients_array[i].username) == 0)
            {
                #ifdef DEBUG_PROGRAM
                printf("[!] username already taken \n");
                #endif

                cJSON_Delete(json_root);
                return;
            }
        }

        clib__null_memory(server_info, sizeof(server_info));
        sprintf(server_info,"%s%s%s%s","[i] ", clients_array[index].username, " renamed to ", json_new_username->valuestring);
        broadcast_server_info(server_info);

        clib__null_memory(clients_array[index].username,sizeof(clients_array[index].username));
        clib__copy_memory(json_new_username->valuestring, clients_array[index].username, strlen(json_new_username->valuestring));
        clients_array[index].timestamp_username_changed = get_timestamp_ms();

        broadcast_client_rename(&clients_array[index]);

        cJSON_Delete(json_root);
    }
    else if(strcmp(json_message_type->valuestring, "client_connection_test") == 0)
    {
        timestamp_now = get_timestamp_ms();
        clients_array[index].timestamp_last_maintain_connection_message_received = timestamp_now;
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

    cli = ws_getaddress(fd);

    if(cli != 0)
    {
        printf("864 free cli \n");
        free(cli);
    }

    ws_sendframe(fd, (char *)encrypted_data_to_send_buffer, strlen(encrypted_data_to_send_buffer), FALSE, type);

    if(encrypted_data_to_send_buffer != 0)
    {
        clib__null_memory(encrypted_data_to_send_buffer, size_of_allocated_message_buffer);
        free(encrypted_data_to_send_buffer);
        encrypted_data_to_send_buffer = 0;
    }
}

void process_not_authenticated_client_message(int fd, int index, unsigned char *decrypted_websocket_data_buffer, unsigned long long size, int type)
{
    cJSON* json_root = 0;
    cJSON* json_message_object = 0;
    cJSON* json_message_type = 0;
    cJSON* json_message_value = 0;
    cJSON* verification_string = 0;
    char index_string[5];
    char *ip_address = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    char server_info[200];
    int channel_index = 0;
    int client_count_in_channel = 0;
    int i = 0;

    #ifdef DEBUG_PROGRAM
    printf("%s %d %s","[i] authenticating client ", fd, "\n");
    #endif

    #ifdef DEBUG_PROGRAM
    printf("%s %s %s","decrypted client verification message : ", decrypted_websocket_data_buffer, "\n");
    #endif

    json_root = cJSON_Parse(decrypted_websocket_data_buffer);

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

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
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

    json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
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

    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    //
    //public RSA key should be 344 characters in length
    //

    if (!cJSON_IsString(json_message_value) || json_message_value->valuestring == NULL || strlen(json_message_value->valuestring) != 344)
    {
        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","client : ", index, " json_message_type != string \n");
        #endif

        ws_close_client(fd);
        cJSON_Delete(json_root);
        return;
    }

    verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
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
        //give him some username, add him to root channel, save his public key,
        //send him list of channels, send him list of clients and send him ID of maintaner of the current channel (root)
        //

        clients_array[index].channel_id = 1; //root channel, id is 1
        clients_array[index].is_admin = 1;

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","[i] client ", index ," authenticated \n");
        #endif

        clib__copy_memory(g_server_settings.default_client_name ,  clients_array[index].username, strlen(g_server_settings.default_client_name));
        sprintf(index_string,"%d",index);
        clib__copy_memory(index_string,&clients_array[index].username[strlen(clients_array[index].username)], strlen(index_string));
        ip_address = 0;

        #ifdef DEBUG_PROGRAM
        ip_address = ws_getaddress(clients_array[index].fd);
        if(fd != 0)
        {
            printf("%s %s %s %s","new user connected: " , ip_address, clients_array[index].username, "\n");
        }
        #endif

        clib__copy_memory(json_message_value->valuestring , clients_array[index].public_key, strlen(json_message_value->valuestring));
        clients_array[index].is_authenticated = TRUE; //DO NOT set without setting up username first
        size_of_allocated_message_buffer = 0;

        cJSON *json_root_object1 = cJSON_CreateObject();
        cJSON *json_message_object1 = cJSON_CreateObject();
        char* json_root_object1_string = 0;
        int size_of_allocated_message_buffer = 0;
        char* msg_text = 0;
        int i;

        cJSON_AddStringToObject(json_message_object1, "type", "authentication_status");
        cJSON_AddStringToObject(json_message_object1, "value", "success");
        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

        if(g_server_settings.is_voice_chat_active == TRUE)
        {
            cJSON_AddNumberToObject(json_message_object1, "websocket_audio_port_number", g_server_settings.websocket_audio_port_number);
            cJSON_AddBoolToObject(json_message_object1, "is_voice_chat_active", TRUE);

            //
            // create random auth string for audio websocket
            //

            unsigned char random_bytes[32] = {0};

            for(int x = 0; x < 32; x++)
            {
                random_bytes[x] = (unsigned char)((rand() % (255 - 100 + 1)) + 100);
            }

            char* websocket_audio_auth_string = (char*)calloc(45, sizeof(char));
            zchg_base64_encode((const unsigned char *)random_bytes, sizeof(random_bytes) , websocket_audio_auth_string);

            clib__copy_memory(websocket_audio_auth_string, clients_array[index].websocket_audio_auth_string_base64, strlen(websocket_audio_auth_string));

            clib__null_memory(websocket_audio_auth_string, strlen(websocket_audio_auth_string));
            free(websocket_audio_auth_string);
            websocket_audio_auth_string = 0;

            cJSON_AddStringToObject(json_message_object1, "websocket_audio_auth_string_base64", clients_array[index].websocket_audio_auth_string_base64);
        }
        else
        {
            cJSON_AddBoolToObject(json_message_object1, "is_voice_chat_active", FALSE);
        }

        json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);
        msg_text = encrypt_websocket_msg(json_root_object1_string, &size_of_allocated_message_buffer);

        ws_sendframe(fd, msg_text, strlen(msg_text), FALSE, type);

        clib__null_memory(msg_text, size_of_allocated_message_buffer);
        free(msg_text);
        msg_text = 0;

        free(json_root_object1_string);
        json_root_object1_string = 0;

        #ifdef DEBUG_PROGRAM
        printf("%s%d%s","type :" , type ,"\n");
        #endif

        broadcast_client_connect(&clients_array[index]);
        send_channel_list_to_client(&clients_array[index]);
        send_client_list_to_client(&clients_array[index]);
        clib__null_memory(server_info, sizeof(server_info));
        sprintf(server_info,"%s%s","[i] connected : ", clients_array[index].username);
        broadcast_server_info(server_info);

        channel_index = get_channel_index_by_channel_id(1);
        if(channel_index == -1)
        {
            #ifdef DEBUG_PROGRAM
            printf("%s","[i] root channel not found \n");
            #endif

            cJSON_Delete(json_root);
            ws_close_client(fd);
            return;
        }

        client_count_in_channel = 0;

        for(i = 0; i < MAX_CLIENTS; i++)
        {
            if(clients_array[i].is_authenticated == FALSE)
            {
                continue;
            }
            if(clients_array[i].channel_id == 1)
            {
                client_count_in_channel++;
            }
        }

        if(client_count_in_channel == 1)
        {
            channel_array[channel_index].maintainer_id = clients_array[index].user_id;
        }

        #ifdef DEBUG_PROGRAM
        printf("%s %d %s","[i] send_maintainer_id_to_client client ", index ,"  \n");
        #endif
        send_maintainer_id_to_client(&clients_array[index], &channel_array[channel_index]);
    }
    else
    {
        cJSON_Delete(json_root);
        ws_close_client(fd);
    }
}


void websocket_thread(void)
{
    struct ws_events evs;

    //#ifdef DEBUG_PROGRAM
    printf("%s%d%s","starting websocket server on port : ",   g_server_settings.websocket_port, "\n");
    //#endif

    evs.onopen    = &onopen;
    evs.onclose   = &onclose;
    evs.onmessage = &onmessage;
    ws_socket(&evs,   g_server_settings.websocket_port); /* Never returns. */
}



void websocket_audio_thread(void)
{
    struct ws_events evs;

    #ifdef DEBUG_PROGRAM
    printf("%s%d%s","starting websocket server on port : ",   g_server_settings.websocket_port, "\n");
    #endif

    evs.onopen    = &onopen_audio;
    evs.onclose   = &onclose_audio;
    evs.onmessage = &onmessage_audio;
    ws_socket(&evs,   g_server_settings.websocket_audio_port_number);
}


void websocket_connection_check_thread(void)
{
    static unsigned long long timestamp_now = 0;
    int i = 0;
    int size_of_allocated_message_buffer = 0;
    char* msg = 0;

    while(1)
    {
        timestamp_now = get_timestamp_ms();

        for(i = 0; i < MAX_CLIENTS; i++)
        {
            if(clients_array[i].timestamp_connected == 0)
            {
                continue;
            }

            if(clients_array[i].is_authenticated == TRUE)
            {
                timestamp_now = get_timestamp_ms();

                //
                //disconnect client who has not sent maintain_connection_message in given time limit
                //

                if(clients_array[i].timestamp_last_maintain_connection_message_received + 180000 < timestamp_now)
                {
                    #ifdef DEBUG_PROGRAM
                    printf("%s%d%s","trying to disconnect client. did not receive maintain connection message : ", clients_array[i].fd  ,"\n");
                    #endif

                    broadcast_client_disconnect(&clients_array[i]);

                    ws_close_client(clients_array[i].fd);
                    clib__null_memory(&clients_array[i], sizeof(client_t));

                    continue;
                }

                size_of_allocated_message_buffer = 0;
                msg = encrypt_websocket_msg((unsigned char*)"connection_test", &size_of_allocated_message_buffer);
                ws_sendframe(clients_array[i].fd, msg, strlen(msg), FALSE, 1);
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

            if(clients_array[i].timestamp_connected + 60000 < timestamp_now)
            {
                #ifdef DEBUG_PROGRAM
                printf("%s%d%s","trying to disconnect client : ", clients_array[i].fd  ,"\n");
                #endif
                ws_close_client(clients_array[i].fd);
                clib__null_memory(&clients_array[i], sizeof(client_t));
            }
        }

        sleep(60);
    }
}


void init_channel_list(void)
{
    char channel_name[] = "root";
    char description[] = "this is default entry channel";
    clib__null_memory(&channel_array, sizeof(channel_t) * MAX_CHANNELS);
    channel_array[0].channel_id = 1;
    channel_array[0].parent_channel_id = -1;
    channel_array[0].is_channel = TRUE;
    channel_array[0].max_clients = MAX_CLIENTS;
    clib__copy_memory((void*)&channel_name, (void*)&channel_array[0].name, strlen(channel_name));
    clib__copy_memory((void*)&description, (void*)&channel_array[0].description, strlen(description));
    channel_array[0].type = 1;
    channel_array[0].maintainer_id = -1;
    return;
}

void set_server_settings(void)
{
    char input[256];
    int i = 0;
    char verification_message[] = "welcome";
    char default_client_name[30] = "anon";

    //
    //initialization vector must match iv defined in client.html
    //

    SHA256_CTX ctx;
    unsigned char custom_iv[16] =  {90,11,8,33,4,50,50,88,    8,89,200,15,24,4,15,10};

    clib__null_memory(&g_server_settings, sizeof(server_settings_t));
    clib__copy_memory(verification_message, g_server_settings.client_verificaton_message_cleartext, strlen(verification_message));
    g_server_settings.websocket_message_max_length = 5000000;
    g_server_settings.websocket_chat_message_string_max_length = 8000;
    g_server_settings.chat_cooldown_milliseconds = 100;
    g_server_settings.join_channel_request_cooldown_milliseconds = 100;
    g_server_settings.create_channel_request_cooldown_milliseconds = 1000;
    clib__copy_memory(default_client_name, g_server_settings.default_client_name, strlen(default_client_name));

    printf("WebSocket port: ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    g_server_settings.websocket_port = strtol(input, 0, 10);
    clib__null_memory(input, sizeof(input));

    printf("%s", "enter number of keys to be used. (1-100) : ");
    fgets(input, sizeof(input), stdin);

    g_server_settings.keys_count = atoi(input);

    for(i = 0; i < g_server_settings.keys_count; i++)
    {
        clib__null_memory(input, sizeof(input));
        printf("%s%d%s", "specify key " , i, " : ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);

        sha256_init(&ctx);
        sha256_update(&ctx, (unsigned char*)input, strlen(input));
        sha256_final(&ctx, g_server_settings.keys[i].key_value);

        //
        //destination,                 source,             length
        //

        clib__copy_memory(custom_iv, &g_server_settings.keys[i].key_iv, 16);
    }


    clib__null_memory(input, sizeof(input));

    printf("%s", "allow voice chat? y/n ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if (strcmp(input, "y") == 0)
    {
        g_server_settings.is_voice_chat_active = TRUE;
    }

    if(g_server_settings.is_voice_chat_active == TRUE)
    {
        printf("WebSocket port for voice chat: ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        g_server_settings.websocket_audio_port_number = strtol(input, 0, 10);
        clib__null_memory(input, sizeof(input));
    }
}

void process_local_command(char* command)
{
    int i = 0;
    BOOL result;
    char input1[7] = {0};
    int user_id;

    if(strcmp(command, "client_list") == 0)
    {
        printf("%s","user id, username , is admin \n");

        for(i = 0; i < MAX_CLIENTS; i++)
        {
            if(clients_array[i].is_authenticated == FALSE)
            {
                continue;
            }
            printf("%d %s %s %s %s %s %d", clients_array[i].user_id, ":" , clients_array[i].username, ", ", clients_array[i].websocket_audio_auth_string_base64, ",", (int)clients_array[i].is_admin);
            printf("%s", "\n");
        }
    }
    else if(strcmp(command, "channel_list") == 0)
    {
        printf("%s","channel id, name , maintainer_id \n");

        for(i = 0; i < MAX_CHANNELS; i++)
        {
            if(channel_array[i].is_channel == FALSE)
            {
                continue;
            }
            printf("%d %s %s %s %d", channel_array[i].channel_id, ":" , channel_array[i].name,",",(int)channel_array[i].maintainer_id);
            printf("%s", "\n");
        }
    }
    else if(strcmp(command, "set_admin") == 0)
    {
        printf("%s", "enter client id to set (-1 to cancel): ");

        clib__null_memory(input1,sizeof(input1));
        fgets(input1,sizeof(input1),stdin);

        if(strlen(input1) > 7)
        {
            return;
        }

        result = clib__is_str_number(input1);
        if(result == FALSE)
        {
            return;
        }

        user_id = atoi(input1);

        if(user_id == -1)
        {
            return;
        }

        for(i = 0; i < MAX_CLIENTS; i++)
        {
            if(clients_array[i].is_authenticated == FALSE)
            {
                continue;
            }

            if(clients_array[i].user_id == user_id)
            {
                clients_array[i].is_admin = !clients_array[i].is_admin;
                break;
            }
        }
    }
    else
    {
        printf("%s", "unknown command \n");
    }
}


int main()
{
    char input[50];

    init_channel_list();
    set_server_settings();

    unsigned long thread_id0 = 0;
    unsigned long thread_id1 = 0;
    unsigned long thread_id2 = 0;

    pthread_create(&thread_id0, 0, (void*)&websocket_thread, 0);
    pthread_create(&thread_id1, 0, (void*)&websocket_connection_check_thread, 0);

    if(g_server_settings.is_voice_chat_active)
    {
        pthread_create(&thread_id2, 0, (void*)&websocket_audio_thread, 0);
    }

    while(1)
    {
        clib__null_memory(input, sizeof(input));
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        process_local_command(input);
    }

    return 0;
}
