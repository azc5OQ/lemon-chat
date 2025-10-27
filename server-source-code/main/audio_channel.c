#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "libdatachannel/include/rtc/rtc.h"
#include "dave-g-json/cJSON.h"

#include "base.h"

#include "audio_channel.h"
#include "../theldus-websocket/include/ws.h"

#include "clib/clib_memory.h"
#include "clib/clib_string.h"
#include "log/log.h"
#include "memory_manager.h"

#include "server_message.h"

//externy sa musia inicializovat, na 0
//opus_data_buffer_entry_t *opus_data_buffer_entries_array = 0;
webrtc_peer_t *webrtc_muggles_array = 0;

void RTC_API peerconnection_on_setlocaldescription_callback(int pc, const char *sdp, const char *type, void *ptr);
void RTC_API peerconnection_on_icecandidate_callback(int pc, const char *cand, const char *mid, void *ptr);
void RTC_API peerconnection_on_statechanged_callback(int pc, rtcState state, void *ptr);
void RTC_API peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void *ptr);
void RTC_API peerconnection_on_datachannel_callback(int pc, int dc, void *ptr);
void RTC_API datachannel_on_open_callback(int id, void *ptr);
void RTC_API datachannel_on_closed_callback(int id, void *ptr);
void RTC_API datachannel_on_message_callback(int id, const char *message, int size, void *ptr);
char *state_print(rtcState state);
char *rtcGatheringState_print(rtcGatheringState state);

//bol som nuteny odstranit z audio_channel.c rwlocky, pretoze, sposobovali zamrzanie.

/**
 * @brief error callback
 *
 * @param id id
 * @param const char* error
 * @param void* client
 *
 * @return void
 */
void RTC_API datachannel_onerror_callback(int id, const char *error, void *ptr)
{
	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s %s", "DataChannel from %s error: %s\n", error, "\n");
}

/**
 * @brief peerconnection_on_setlocaldescription_callback
 *
 * @param id peer_connection_handle
 * @param const char* sdp
 * @param void* type
 * @param void* ptr
 *
 * @return void
 */
void RTC_API peerconnection_on_setlocaldescription_callback(int pc, const char *sdp, const char *type, void *ptr)
{
	char *text_to_send = 0;
	cJSON *json_root_object = NULL;
	cJSON *json_message_object = NULL;
	cJSON *json_message_value = NULL;
	char *json_root_object_string = 0;
	int size_of_allocated_message_buffer = 0;
	cJSON *jid = NULL;
	cJSON *jtype = NULL;
	cJSON *jdescription = NULL;
	webrtc_peer_t *peer = 0;
	char *json_value_object_string;

	DBG_AUDIOCHANNEL_WEBRTC log_info("peerconnection_on_setlocaldescription_callback \n");

	//toremove--pthread_rwlock_rdlock(&clients_global_rwlock_guard);
	//toremove--pthread_rwlock_rdlock(&webrtc_muggles_rwlock_guard);

	if (ptr == NULL_POINTER)
	{
		goto label_descriptionCallback_end;
	}

	peer = (webrtc_peer_t *)ptr;

	if (peer->is_existing == FALSE)
	{
		goto label_descriptionCallback_end;
	}

	if (clients_array[peer->client_id].is_authenticated == FALSE)
	{
		goto label_descriptionCallback_end;
	}

	if (clib__is_string_equal(type, "offer") == TRUE)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("we are creating offer \n");
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %p %s", "peerconnection_on_setlocaldescription_callback", peer, "\n");

		json_root_object = cJSON_CreateObject();
		json_message_object = cJSON_CreateObject();

		json_message_value = cJSON_CreateObject(); // this object will be parsed to string down below, and converted to json again when received on clients side

		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "test 2");

		jtype = cJSON_CreateString(type);
		cJSON_AddItemToObject(json_message_value, "type", jtype);
		jdescription = cJSON_CreateString(sdp);
		cJSON_AddItemToObject(json_message_value, "sdp", jdescription);
		json_root_object_string = cJSON_PrintUnformatted(json_message_value);

		cJSON_AddStringToObject(json_message_object, "value", json_root_object_string);

		cJSON_AddStringToObject(json_message_object, "type", "sdp_offer");
		cJSON_AddItemToObject(json_root_object, "message", json_message_object);

		json_root_object_string = cJSON_PrintUnformatted(json_root_object);

		text_to_send = base__encrypt_cstring_and_convert_to_base64(json_root_object_string, &size_of_allocated_message_buffer, peer->dh_shared_secret);
		ws_sendframe_txt(peer->p_ws_connection, text_to_send);

		base__free_json_message(json_root_object, json_root_object_string);

		memorymanager__free((nuint)text_to_send);
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "test 3");
	}

label_descriptionCallback_end:
	return;

	//toremove--pthread_rwlock_unlock(&clients_global_rwlock_guard);
	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
}

/**
 * @brief peerconnection_on_datachannel_callback
 *
 * @param id peer_connection_handle
 * @param int data_channel_handle
 * @param void *ptr
 *
 * @return void
 */
void RTC_API peerconnection_on_datachannel_callback(int pc, int dc, void *ptr)
{
	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s", "void RTC_API peerconnection_on_datachannel_callback(int peer_connection_handle, int data_channel_handle, void *ptr)", "\n");

	webrtc_peer_t *peer = (webrtc_peer_t *)ptr;

	//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

	if (peer != NULL_POINTER)
	{
		peer->data_channel_handle = dc;
		peer->connected = true;
		rtcSetClosedCallback(dc, datachannel_on_closed_callback);
		rtcSetMessageCallback(dc, datachannel_on_message_callback);
		char buffer[256];

		if (rtcGetDataChannelLabel(dc, buffer, 256) >= 0)
		{
			DBG_AUDIOCHANNEL_WEBRTC log_info("DataChannel %s: Received with label \"%s\"\n", "answerer", buffer);
		}
	}

	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
}

/**
 * @brief peerconnection_on_icecandidate_callback
 *
 * @param id peer_connection_handle
 * @param const char *cand
 * @param const char *mid
 * @param void *ptr
 *
 * @attention internal thread of libdatachannel calls previously set candidatecallback function. using //toremove--pthread_rwlock_rdlock should be safe because thread is different from websockets thread where read and write locks are used
 *
 * @return void
 */
void RTC_API peerconnection_on_icecandidate_callback(int pc, const char *cand, const char *mid, void *ptr)
{
	cJSON *json_root_object = NULL;
	cJSON *json_message_object = NULL;
	cJSON *json_message_value = NULL;
	cJSON *jmid = NULL;
	cJSON *jsoncandidate = NULL;
	cJSON *jsonlineindex = NULL;
	webrtc_peer_t *peer = 0;
	int size_of_allocated_message_buffer = 0;
	char *json_root_object_string = 0;
	char *text_to_send = 0;

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "peerconnection_on_icecandidate_callback \n");

	peer = (webrtc_peer_t *)ptr;

	if (peer == NULL_POINTER)
	{
		return;
	}

	//toremove--pthread_rwlock_rdlock(&clients_global_rwlock_guard);

	if (peer->is_existing)
	{
		//toremove--pthread_rwlock_rdlock(&clients_global_rwlock_guard);

		if (clients_array[peer->client_id].is_authenticated)
		{
			DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "sending sdp offer to client \n");

			server_msg__send_webrtc_sdp_offer_to_single_client(cand, mid, &clients_array[peer->client_id]);
		}

		//toremove--pthread_rwlock_unlock(&clients_global_rwlock_guard);
	}

	//toremove--pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param int client_id
 * @param boole is_active
 *
 * @return void
 */
void audio_channel__set_is_client_sending_audio(int client_id, boole is_active)
{
	webrtc_muggles_array[client_id].is_sending_audio_right_now = is_active;
}

/**
 * @brief peerconnection_on_statechanged_callback
 *
 * @param id peer_connection_handle
 * @param rtcState state
 * @param void *ptr
 *
 * @return void
 */
void RTC_API peerconnection_on_statechanged_callback(int pc, rtcState state, void *ptr)
{
	webrtc_peer_t *peer = 0;
	client_t *client = 0;

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "entered this function");
	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s %s %s", "peerconnection_on_statechanged_callback ", ((webrtc_peer_t *)ptr)->client_id, " to client ", state_print(state), "\n");

	peer = (webrtc_peer_t *)ptr;

	if (peer != NULL_POINTER)
	{
		//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

		if (peer->is_existing)
		{
			//toremove--pthread_rwlock_wrlock(&clients_global_rwlock_guard);

			DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s %s", "peerconnection_on_statechanged_callback ", state_print(state), "\n");
			client = &clients_array[peer->client_id];

			if (client->is_authenticated)
			{
				int state_to_send;
				if (state == RTC_CONNECTED)
				{
					state_to_send = 3;
					client->audio_state = AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;
				}
				else
				{
					client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
				}

				if (peer->last_sent_audio_state != client->audio_state)
				{
					peer->last_sent_audio_state = client->audio_state;
					server_msg__send_audio_state_of_client_to_all_clients(peer->client_id, client->audio_state);
				}

				//server_msg__send_peer_connection_state_to_all_clients(client->client_id, state_to_send);
			}

			//toremove--pthread_rwlock_unlock(&clients_global_rwlock_guard);
		}
		//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
	}
}

/**
 * @brief peerconnection_on_gatheringstatechanged_callback
 *
 * @param id peer_connection_handle
 * @param rtcState state
 * @param void *ptr
 *
 * @return void
 */
void RTC_API peerconnection_on_gatheringstatechanged_callback(int pc, rtcGatheringState state, void *ptr)
{
	//log_info("%s %s %s", "peerconnection_on_gatheringstatechanged_callback", rtcGatheringState_print(state), "\n");

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "entered this function");

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s %s %s", "peerconnection_on_gatheringstatechanged_callback ", ((webrtc_peer_t *)ptr)->client_id, " to client ", rtcGatheringState_print(state), "\n");

	if (state == RTC_GATHERING_COMPLETE)
	{
	}
}

/**
 * @brief datachannel_on_open_callback
 *
 * @param id peer_connection_handle
 * @param void *ptr
 *
 * @return void
 */
void RTC_API datachannel_on_open_callback(int id, void *ptr)
{
	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s", "datachannel_on_open_callback", "\n");
	webrtc_peer_t *peer = (webrtc_peer_t *)ptr;

	//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

	if (peer != NULL_POINTER)
	{
		peer->connected = true;
		char buffer[256];
		if (rtcGetDataChannelLabel(peer->data_channel_handle, buffer, 256) >= 0)
		{
			DBG_AUDIOCHANNEL_WEBRTC log_info("DataChannel %s: Received with label \"%s\"\n", "offerer", buffer);
		}
	}

	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
}

/**
 * @brief datachannel_on_closed_callback
 *
 * @param id peer_connection_handle
 * @param void *ptr
 *
 * @return void
 */
void RTC_API datachannel_on_closed_callback(int id, void *ptr)
{
	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s", "datachannel_on_closed_callback", "\n");

	//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

	webrtc_peer_t *peer = (webrtc_peer_t *)ptr;

	if (peer != NULL_POINTER)
	{
		peer->is_existing = false;
		peer->connected = false;
		clib__null_memory(peer, sizeof(webrtc_peer_t));
	}
	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
}

/**
 * @brief sends UDP voice data to other clients in same channel.
 *
 * @param int id
 * @param const char *message
 * @param int size
 * @param void *ptr
 *
 * @warning this function lags the chat. Solution, create small list that will mirror client ids and their channel ids that only this messagecallback will use
 *
 * @return void
 */
void RTC_API datachannel_on_message_callback(int id, const char *message, int size, void *ptr)
{
	//printf("%s %s", "datachannel_on_message_callback" , "\n");
	webrtc_peer_t *peer_receiver;
	webrtc_peer_t *peer_sender;
	int dc = 0;

	if (size < 0)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "datachannel_on_message_callback size < 0\n");
	}
	else
	{
		//toremove--pthread_rwlock_rdlock(&webrtc_muggles_rwlock_guard);

		//printf("Message %s: [binary of size %d]\n", "offerer", size);

		peer_sender = (webrtc_peer_t *)ptr;

		if (peer_sender->is_sending_audio_right_now == TRUE)
		{
			//
			// sent audio data to other clients located in same channel as found client, if they too have authenticated audio websocket
			//

			for (int i = 0; i < MAX_CLIENTS; i++)
			{
				peer_receiver = &webrtc_muggles_array[i];

				if (peer_receiver->is_existing == FALSE)
				{
					continue;
				}
				else if (peer_receiver->channel_id != peer_sender->channel_id)
				{
					continue;
				}
				else if (channel_array[peer_receiver->channel_id].is_audio_enabled == FALSE)
				{
					continue;
				}
				else if (i == peer_sender->client_id) //dont want to sent message to ourselve
				{
					continue;
				}

				DBG_AUDIOCHANNEL_WEBRTC log_info("%s %s %s", "sending data to ", clients_array[i].username, "\n");

				void *message1 = (void *)memorymanager__allocate(size + 5, MEMALLOC_AUDIOCHANNEL_ONMESSAGE);
				((int *)message1)[0] = peer_sender->client_id;
				clib__copy_memory((void *)message, ((unsigned char *)message1 + 4), size, size);

				rtcSendMessage(peer_receiver->data_channel_handle, message1, size + 4);

				memorymanager__free((nuint)message1);
			}
		}

		//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
	}
}

/**
 * @brief state_print
 *
 * @param rtcState state
 *
 * @return char *
 */
char *state_print(rtcState state)
{
	char *str = NULL;
	switch (state)
	{
	case RTC_NEW:
		str = "RTC_NEW";
		break;
	case RTC_CONNECTING:
		str = "RTC_CONNECTING";
		break;
	case RTC_CONNECTED:
		str = "RTC_CONNECTED";
		break;
	case RTC_DISCONNECTED:
		str = "RTC_DISCONNECTED";
		break;
	case RTC_FAILED:
		str = "RTC_FAILED";
		break;
	case RTC_CLOSED:
		str = "RTC_CLOSED";
		break;
	default:
		break;
	}

	return str;
}

/**
 * @brief rtcGatheringState_print
 *
 * @param rtcGatheringState state
 *
 * @return char*
 */
char *rtcGatheringState_print(rtcGatheringState state)
{
	char *str = NULL;
	switch (state)
	{
	case RTC_GATHERING_NEW:
		str = "RTC_GATHERING_NEW";
		break;
	case RTC_GATHERING_INPROGRESS:
		str = "RTC_GATHERING_INPROGRESS";
		break;
	case RTC_GATHERING_COMPLETE:
		str = "RTC_GATHERING_COMPLETE";
		break;
	default:
		break;
	}

	return str;
}

/**
 * @brief audio_channel__process_client_channel_join
 *
 * @param client_t* client
 *
 * @return void
 */
void audio_channel__process_client_channel_join(client_t *client)
{
	webrtc_peer_t *peer = NULL;

	//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

	peer = &webrtc_muggles_array[client->client_id];

	if (peer->is_existing == FALSE)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_client_channel_join peer not is_existing \n");
		goto label_audio_channel__process_client_channel_switch_end;
	}

	peer->channel_id = client->channel_id;

label_audio_channel__process_client_channel_switch_end:

	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);

	return;
}

/**
 * @brief audio_channel__process_client_disconnect
 *
 * @param client_t* client
 *
 * @return void
 */
void audio_channel__process_client_disconnect(client_t *client)
{
	webrtc_peer_t *peer = NULL;

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_client_disconnect \n");

	//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_client_disconnect got here \n");

	peer = &webrtc_muggles_array[client->client_id];

	if (peer->is_existing == FALSE)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_client_disconnect peer not is_existing \n");
		goto label_audio_channel__process_client_disconnect;
	}

	//these functions never returned and server froze
	//but i found out that, clib__null_memory is enough

	//if (peer->data_channel_handle)
	//{
	//    rtcDeleteDataChannel(peer->data_channel_handle);
	//}
	//if (peer->peer_connection_handle)
	//{
	//    rtcDeletePeerConnection(peer->peer_connection_handle);
	//}

	clib__null_memory(peer, sizeof(webrtc_peer_t));

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "audio_channel__process_client_disconnect peer ", client->client_id, "disconnected \n");

label_audio_channel__process_client_disconnect:
	return;

	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
}

/**
 * @brief rtcGatheringState_print
 *
 * @param client_t* client
 * @param char* candidate
 *
 * @return char*
 */
void audio_channel__process_ice_candidate_from_remote_peer(client_t *client, cJSON *json_root)
{
	sleep(1);

	cJSON *cjson_candidate = NULL;
	webrtc_peer_t *peer = NULL;

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_ice_candidate_from_remote_peer \n");

	if (json_root == NULL_POINTER)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_ice_candidate_from_remote_peer json root is null \n");
		return;
	}

	//tu sa deje nejaky deadlock, pokial si nevytvorim bezpecnu rwlock strukturu, bude to zakomentovane.
	//chcem vyriesit funkcnost audia ako takeho

	// //toremove--pthread_rwlock_rdlock(&webrtc_muggles_rwlock_guard);

	peer = &webrtc_muggles_array[client->client_id];

	if (peer->is_existing == FALSE)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_ice_candidate_from_remote_peerk peer not is_existing \n");
		goto label_audio_channel__process_ice_candidate_from_remote_peer_end;
	}

	cjson_candidate = cJSON_GetObjectItemCaseSensitive(json_root, "candidate");

	if (cjson_candidate == NULL_POINTER)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "process_ice_candidate_from_remote_peer ", client->client_id, "candicate is null \n");
		goto label_audio_channel__process_ice_candidate_from_remote_peer_end;
	}

	if (!cJSON_IsString(cjson_candidate) || cjson_candidate->valuestring == NULL)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "process_ice_candidate_from_remote_peer ", client->client_id, "!cJSON_IsString(cjson_candidate) || cjson_candidate->valuestring == NULL \n");
		goto label_audio_channel__process_ice_candidate_from_remote_peer_end;
	}

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "process_ice_candidate_from_remote_peer rtcAddRemoteCandidate \n");

	//improvement, better check the string length, max length must be known
	//char* candidate_value = (char*)calloc(clib__utf8_string_length(cjson_candidate->valuestring), sizeof(char));

	//clib__copy_memory(cjson_candidate->valuestring, candidate_value, strlen(cjson_candidate->valuestring), strlen(cjson_candidate->valuestring));

	rtcAddRemoteCandidate(peer->peer_connection_handle, cjson_candidate->valuestring, 0); //it is possible that value cjson_candidate->valuestring will have to be stored on heap, test for crashes

label_audio_channel__process_ice_candidate_from_remote_peer_end:
	return;
	////toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
}

/**
 * @brief rtcGatheringState_print
 *
 * @param client_t* client
 * @param char* candidate
 *
 * @return char*
 */
void audio_channel__process_sdp_answer_from_remote_peer(client_t *client, cJSON *RTCSessionDescription)
{
	webrtc_peer_t *peer = NULL;
	cJSON *cjson_type = NULL;
	cJSON *cjson_sdp = NULL;

	sleep(1);

	DBG_AUDIOCHANNEL_WEBRTC log_info("audio_channel__process_sdp_answer_from_remote_peer \n");

	if (RTCSessionDescription == NULL_POINTER || client == NULL_POINTER)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_sdp_answer_from_remote_peer RTCSessionDescription == NULL_POINTER || client == NULL_POINTER  \n");
		return;
	}

	//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

	peer = &webrtc_muggles_array[client->client_id];

	if (peer->is_existing == FALSE)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__process_sdp_answer_from_remote_peer peer not is_existing \n");
		goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
	}

	cjson_type = cJSON_GetObjectItemCaseSensitive(RTCSessionDescription, "type");
	cjson_sdp = cJSON_GetObjectItemCaseSensitive(RTCSessionDescription, "sdp");

	if (cjson_type == 0 || cjson_sdp == 0)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "[!] audio_channel__process_sdp_answer_from_remote_peer : ", client->client_id, "\n");
		goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
	}

	if (!cJSON_IsString(cjson_type) || cjson_type->valuestring == NULL || (cjson_type->valuestring) == 0)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s%d%s", "[!] audio_channel__process_sdp_answer_from_remote_peer : ", client->client_id, "\n");
		goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
	}

	if (!cJSON_IsString(cjson_sdp) || cjson_sdp->valuestring == NULL || clib__utf8_string_length(cjson_sdp->valuestring) == 0)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s%d%s", "[!] audio_channel__process_sdp_answer_from_remote_peer : ", client->client_id, "\n");
		goto label_audio_channel__process_sdp_answer_from_remote_peer_end;
	}

	DBG_AUDIOCHANNEL_WEBRTC log_info("audio_channel__process_sdp_answer_from_remote_peer rtcSetRemoteDescription \n");
	rtcSetRemoteDescription(peer->peer_connection_handle, cjson_sdp->valuestring, cjson_type->valuestring);

label_audio_channel__process_sdp_answer_from_remote_peer_end:
	return;

	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
}

/**
 * @brief this function tries to initialize structure that represents webrtc datachannel connection that server creates with client.
 *
 * @param client_t* client
 *
 * @return char* encrypted string
 *
 * @attention this function assumes that client pointer is used within readlock or write lock, and its safe to read it.
 *
 * @note like websocket, webrtc also works between client and server in this chat application, its not peer to peer. technically it is, but the peer is server.
 */
boole audio_channel__initialize_webrtc_datachannel_connection(client_t *client)
{
	webrtc_peer_t *peer = 0;
	boole result = FALSE;

	DBG_AUDIOCHANNEL_WEBRTC log_info("audio_channel__initialize_webrtc_datachannel_connection \n");

	if (client == NULL_POINTER)
	{
		result = FALSE;
		goto label_audio_channel__initialize_webrtc_datachannel_connection_end;
	}

	//rtcInitLogger(RTC_LOG_VERBOSE, NULL);

	rtcConfiguration config;

	memset(&config, 0, sizeof(config));

	//const char* iceServers[1] = { "stun:stun.l.google.com:19302" };
	const char *iceServers[1] = { "127.0.0.1:3478" }; //using our own stun server! (violet)
	config.iceServers = iceServers;
	config.iceServersCount = 1;

	//toremove--pthread_rwlock_wrlock(&webrtc_muggles_rwlock_guard);

	peer = &webrtc_muggles_array[client->client_id];

	peer->is_existing = TRUE;
	peer->last_sent_audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
	peer->peer_connection_handle = rtcCreatePeerConnection(&config);
	peer->p_ws_connection = client->p_ws_connection;
	peer->client_id = client->client_id;
	clib__copy_memory(client->dh_shared_secret, peer->dh_shared_secret, clib__utf8_string_length(client->dh_shared_secret), 1000);

	if (peer->peer_connection_handle == NULL_POINTER)
	{
		peer->is_existing = FALSE;
		//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);

		goto label_audio_channel__initialize_webrtc_datachannel_connection_end;
		result = FALSE;
	}

	rtcSetUserPointer(peer->peer_connection_handle, peer); //binds created peer connection to my own custom struct. Nice that this library has this
	rtcSetLocalDescriptionCallback(peer->peer_connection_handle, peerconnection_on_setlocaldescription_callback);
	rtcSetLocalCandidateCallback(peer->peer_connection_handle, peerconnection_on_icecandidate_callback); //gets called when ICE candidate is returned from STUN server
	rtcSetStateChangeCallback(peer->peer_connection_handle, peerconnection_on_statechanged_callback); //gets called when peer connection state is changed
	rtcSetGatheringStateChangeCallback(peer->peer_connection_handle, peerconnection_on_gatheringstatechanged_callback);
	rtcSetDataChannelCallback(peer->peer_connection_handle, peerconnection_on_datachannel_callback);

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "audio_channel__initialize_webrtc_datachannel_connection got here \n");

	rtcDataChannelInit init;

	//
	//clear out structure or initialization fails
	//only unreliable needs to be set
	//

	clib__null_memory(&init, sizeof(rtcDataChannelInit));
	init.reliability.unreliable = true;
	//init.reliability.unordered = true;

	//
	// name of datachannel is testQQQ
	//

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "peer->data_channel_handle", peer->data_channel_handle, "\n");
	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "audio_channel__initialize_webrtc_datachannel_connection peer->data_channel_handle", peer->data_channel_handle, "\n");

	peer->data_channel_handle = rtcCreateDataChannelEx(peer->peer_connection_handle, "testQQQ", &init);

	result = TRUE;

	rtcSetOpenCallback(peer->data_channel_handle, datachannel_on_open_callback);
	rtcSetClosedCallback(peer->data_channel_handle, datachannel_on_closed_callback);
	rtcSetMessageCallback(peer->data_channel_handle, datachannel_on_message_callback);
	rtcSetErrorCallback(peer->data_channel_handle, datachannel_onerror_callback);

label_audio_channel__initialize_webrtc_datachannel_connection_end:
	//toremove--pthread_rwlock_unlock(&webrtc_muggles_rwlock_guard);
	DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "audio_channel__initialize_webrtc_datachannel_connection asas", peer->data_channel_handle, "\n");

	return result;
}
