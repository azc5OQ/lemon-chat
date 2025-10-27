#include "mytypedef.h"
#include "dave-g-json/cJSON.h"
#include "client_message.h"
#include "log/log.h"
#include "clib/clib_string.h"
#include "base.h"
#include "memory_manager.h"
#include "clib/clib_memory.h"
#include "libtom/tommath.h"
#include "server_message.h"
#include "audio_channel.h"
#include "ip_tools.h"
#include "eteran-cvector/cvector.h"

//static functions are defined first

//declarations
static boole _client_msg_internal__is_add_tag_to_client_valid(cJSON *json_root);
static boole _client_msg_internal__is_remove_tag_from_client_valid(cJSON *json_root);
static boole _client_msg_internal__is_process_server_settings_icon_upload_message_valid(cJSON *json_root);
static boole _client_msg_internal__is_process_server_settings_add_new_tag_message_valid(cJSON *json_root);

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * *
 * @return boole
 */
static boole _client_msg_internal__is_add_tag_to_client_valid(cJSON *json_root)
{
	cJSON *json_client_id = 0;
	cJSON *json_tag_id = 0;
	cJSON *json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
	if (!cJSON_IsNumber(json_client_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_add_tag_to_client_valid cJSON_IsNumber(client_id) \n");
		return FALSE;
	}

	if (json_client_id->valueint < 0 || json_client_id->valueint >= MAX_CLIENTS)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_add_tag_to_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
		return FALSE;
	}

	json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
	if (!cJSON_IsNumber(json_tag_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_add_tag_to_client_valid cJSON_IsNumber(client_id) \n");
		return FALSE;
	}

	if (json_tag_id->valueint < 0 || json_client_id->valueint >= MAX_TAGS)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_add_tag_to_client_valid json_tag_id->valueint is not valid ", json_tag_id->valueint, "\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * *
 * @return boole
 */
static boole _client_msg_internal__is_remove_tag_from_client_valid(cJSON *json_root)
{
	cJSON *json_client_id;
	cJSON *json_tag_id;
	cJSON *json_message_object;

	json_client_id = 0;
	json_tag_id = 0;
	json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
	if (!cJSON_IsNumber(json_client_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_add_tag_to_client_valid cJSON_IsNumber(client_id) \n");
		return FALSE;
	}

	if (json_client_id->valueint < 0 || json_client_id->valueint >= MAX_CLIENTS)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_add_tag_to_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
		return FALSE;
	}

	json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
	if (!cJSON_IsNumber(json_tag_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_add_tag_to_client_valid cJSON_IsNumber(client_id) \n");
		return FALSE;
	}

	if (json_tag_id->valueint < 0 || json_client_id->valueint >= MAX_TAGS)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_add_tag_to_client_valid json_tag_id->valueint is not valid ", json_tag_id->valueint, "\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * *
 * @return boole
 */
static boole _client_msg_internal__is_process_server_settings_icon_upload_message_valid(cJSON *json_root)
{
	cJSON *json_message_object;
	cJSON *base64_icon_value;
	nuint base64_icon_length;

	json_message_object = 0;
	base64_icon_value = 0;
	base64_icon_length = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	base64_icon_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_icon_value");
	if (!cJSON_IsString(base64_icon_value))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_icon_upload_message_valid cJSON_IsString(base64_icon_value)");
		return FALSE;
	}

	base64_icon_length = clib__utf8_string_length(base64_icon_value->valuestring);

	//dont accept icons too small or too large
	if (base64_icon_length >= ICON_MAX_LENGTH || base64_icon_length < 128)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length(base64_icon_value->valuestring) >= ICON_MAX_LENGTH");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @note this is just first glance check, it doesnt access any of array structures array_icons or array_tags
 * *
 * @return boole
 */
static boole _client_msg_internal__is_process_server_settings_add_new_tag_message_valid(cJSON *json_root)
{
	cJSON *json_message_object;
	cJSON *json_tag_name;
	cJSON *json_linked_icon_id;
	nuint new_tag_name_length;

	json_message_object = 0;
	json_tag_name = 0;
	json_linked_icon_id = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_tag_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_name");

	if (!cJSON_IsString(json_tag_name))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_add_new_tag_message_valid cJSON_IsString(json_tag_name)");
		return FALSE;
	}

	new_tag_name_length = clib__utf8_string_length(json_tag_name->valuestring);

	if (new_tag_name_length >= TAG_MAX_NAME_LENGTH || new_tag_name_length == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_add_new_tag_message_valid (clib__utf8_string_length(json_tag_name->valuestring) >= TAG_MAX_NAME_LENGTH)");
		return FALSE;
	}

	json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
	if (!cJSON_IsNumber(json_linked_icon_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_add_new_tag_message_valid cJSON_IsNumber(json_linked_icon_id)");
		return FALSE;
	}

	if (json_linked_icon_id->valueint < 0 || json_linked_icon_id->valueint >= MAX_ICONS)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "icon id is invalid");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * *
 * @return boole
 */
static boole client_msg__is_json_start_song_stream_message_valid(cJSON *json_root)
{
	cJSON *json_song_name = 0;
	cJSON *json_message_object = 0;
	int64 song_name_length = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_song_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_name");
	if (!cJSON_IsString(json_song_name))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__is_json_start_song_stream_message_valid cJSON_IsString(json_song_name) \n");
		return FALSE;
	}

	if (json_song_name->valuestring == NULL_POINTER)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__is_json_start_song_stream_message_valid json_song_name->valuestring == NULL_POINTER \n");
		return FALSE;
	}

	song_name_length = clib__utf8_string_length_check_max_length(json_song_name->valuestring, SONG_NAME_MAX_LENGTH);
	if (song_name_length == -1 || song_name_length == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__is_json_start_song_stream_message_valid song_name_length == -1 || song_name_length == 0 \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * *
 * @return boole
 */
static boole _client_msg_internal__is_admin_password_message_valid(cJSON *json_root)
{
	cJSON *json_admin_password = 0;
	cJSON *json_message_object = 0;
	int64 song_name_length = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_admin_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	if (!cJSON_IsString(json_admin_password))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_admin_password_message_valid cJSON_IsString(json_admin_password) \n");
		return FALSE;
	}

	if (json_admin_password->valuestring == NULL_POINTER)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_admin_password_message_valid json_admin_password->valuestring == NULL_POINTER \n");
		return FALSE;
	}

	song_name_length = clib__utf8_string_length_check_max_length(json_admin_password->valuestring, 50);
	if (song_name_length == -1 || song_name_length == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_admin_password_message_valid json_admin_password length is wrong \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * *
 * @return boole
 */
static boole client_msg__is_json_process_microphone_usage_valid(cJSON *json_root)
{
	cJSON *json_value = 0;
	cJSON *json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	if (!cJSON_IsNumber(json_value))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__is_json_process_microphone_usage_valid cJSON_IsNumber(json_value_object) \n");
		return FALSE;
	}

	if (json_value->valueint < 1 || json_value->valueint > 3)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__is_json_process_microphone_usage_valid json_value->valueint < 1 || json_value->valueint > 3 \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 * *
 * @return boole
 */
static boole client_msg__is_ice_candidate_format_valid(cJSON *json_root, int client_index)
{
	cJSON *json_candidate = 0;
	cJSON *json_value_object = 0;
	cJSON *json_sdpMid = 0;
	cJSON *json_sdpMLineIndex = 0;
	cJSON *json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	if (!cJSON_IsObject(json_value_object))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " cJSON_IsObject(json_value_object) \n");
		return FALSE;
	}

	json_candidate = cJSON_GetObjectItemCaseSensitive(json_value_object, "candidate");
	if (!cJSON_IsString(json_candidate))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_candidate) \n");
		return FALSE;
	}

	if (json_candidate->valuestring == NULL)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " json_candidate->valuestring == NULL \n");
		return FALSE;
	}

	//if (clib__utf8_string_length(json_sdp_message_type->valuestring) == 0)
	//{
	//    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s","client : ", client_index, " clib__utf8_string_length(json_sdp_message_type->valuestring) \n");
	//    return FALSE;
	//}

	json_sdpMid = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdpMid");
	if (!cJSON_IsString(json_sdpMid))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_sdpMid) \n");
		return FALSE;
	}

	if (json_sdpMid->valuestring == NULL)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " json_sdpMid->valuestring == NULL \n");
		return FALSE;
	}

	//if (clib__utf8_string_length(json_sdp_message_value->valuestring) == 0)
	//{
	//    DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s","client : ", client_index, " clib__utf8_string_length(json_sdp_message_value->valuestring) \n");
	//    return FALSE;
	//}

	json_sdpMLineIndex = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdpMLineIndex");
	if (!cJSON_IsNumber(json_sdpMLineIndex))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(json_sdpMLineIndex) \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 * *
 * @return boole
 */
static boole client_msg__is_json_delete_request_format_valid(cJSON *json_root, int client_index)
{
	cJSON *json_channel_id = 0;
	cJSON *json_message_object = 0;

	//existence of "message" has already been checked at this point
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	//
	// json_message_object exists, continue validating received json
	//

	json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
	if (!cJSON_IsNumber(json_channel_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(json_channel_id) \n");
		return FALSE;
	}

	if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (MAX_CHANNELS - 1)))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (MAX_CHANNELS - 1)) \n");
		return FALSE;
	}

	if (json_channel_id->valueint == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " root channel cannot be deleted \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 * *
 * @return boole
 */
static boole client_msg__is_json_sdp_answer_format_valid(cJSON *json_root, int client_index)
{
	cJSON *json_sdp_message_value = 0;
	cJSON *json_value_object = 0;
	cJSON *json_sdp_message_type = 0;
	cJSON *json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	if (!cJSON_IsObject(json_value_object))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " cJSON_IsObject(json_value_object) \n");
		return FALSE;
	}

	json_sdp_message_type = cJSON_GetObjectItemCaseSensitive(json_value_object, "type");
	if (!cJSON_IsString(json_sdp_message_type))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_sdp_message_type) \n");
		return FALSE;
	}

	if (json_sdp_message_type->valuestring == NULL)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " json_sdp_message_type->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_sdp_message_type->valuestring) == 0)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(json_sdp_message_type->valuestring) \n");
		return FALSE;
	}

	json_sdp_message_value = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdp");
	if (!cJSON_IsString(json_sdp_message_value))
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_sdp_message_value) \n");
		return FALSE;
	}

	if (json_sdp_message_value->valuestring == NULL)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " json_sdp_message_value->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_sdp_message_value->valuestring) == 0)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(json_sdp_message_value->valuestring) \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 * *
 * @return boole
 */
static boole client_msg__is_json_join_channel_request_format_valid(cJSON *json_root, int client_index)
{
	cJSON *json_channel_id = 0;
	cJSON *json_channel_password = 0;
	cJSON *json_message_object = 0;

	//existence of "message" has already been checked at this point
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	//
	// json_message_object exists, continue validating received json
	//

	json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
	if (!cJSON_IsString(json_channel_password))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_channel_password) \n");
		return FALSE;
	}

	if (json_channel_password->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_channel_password->valuestring == NULL \n");
		return FALSE;
	}

	json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
	if (!cJSON_IsNumber(json_channel_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(json_channel_id) \n");
		return FALSE;
	}

	if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (MAX_CHANNELS - 1)))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (MAX_CHANNELS - 1)) \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 * @param message_type self explanatory
 *
 * @note this function is used for processing four different kinds of client messages, direct chat message, channel chat message, direct chat picture, channel chat picture
 *
 * @return boole
 */
boole client_msg__is_message_correct_at_first_sight_and_get_message_type(cJSON *json_root, int client_index, char **message_type)
{
	cJSON *json_message_type = 0;
	cJSON *json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	if (json_message_object == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_object is null\n");
		return FALSE;
	}

	if (!cJSON_IsObject(json_message_object))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsObject(json_message_object) == false\n");
		return FALSE;
	}

	//
	// json_message_object exists, continue validating received json
	//

	json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
	if (!cJSON_IsString(json_message_type))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_message_type) \n");
		return FALSE;
	}

	if (json_message_type->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_type->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_message_type->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(json_message_type->valuestring) \n");
		return FALSE;
	}

	*message_type = json_message_type->valuestring;
	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 *
 * @note this function is used for processing four different kinds of client messages, direct chat message, channel chat message, direct chat picture, channel chat picture
 *
 * @return boole
 */
static boole client_msg__is_json_poke_client_request_format_valid(cJSON *json_root, int client_index)
{
	cJSON *json_poke_message = 0;
	cJSON *json_receiver_id = 0;
	cJSON *json_message_object = 0;

	//existence of "message" has already been checked at this point
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	//
	// json_message_object exists, continue validating received json
	//

	json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
	if (!cJSON_IsNumber(json_receiver_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(json_receiver_id) \n");
		return FALSE;
	}

	if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= MAX_CLIENTS)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= MAX_CLIENTS \n");
		return FALSE;
	}

	json_poke_message = cJSON_GetObjectItemCaseSensitive(json_message_object, "poke_message");
	if (!cJSON_IsString(json_poke_message))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_poke_message) \n");
		return FALSE;
	}

	if (json_poke_message->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_poke_message->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_poke_message->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(json_poke_message->valuestring) \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 *
 * @note this function is used for processing four different kinds of client messages, direct chat message, channel chat message, direct chat picture, channel chat picture
 *
 * @return boole
 */
static boole client_msg__is_json_chat_message_format_valid(cJSON *json_root, int client_index)
{
	cJSON *json_chat_message_value = 0;
	cJSON *json_receiver_id = 0;
	cJSON *json_local_message_id = 0;
	cJSON *json_message_object = 0;

	//existence of "message" has already been checked at this point
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	//
	// json_message_object exists, continue validating received json
	//

	json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	if (!cJSON_IsString(json_chat_message_value))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_chat_message_value) \n");
		return FALSE;
	}

	if (json_chat_message_value->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_chat_message_value->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_chat_message_value->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(json_chat_message_value->valuestring) \n");
		return FALSE;
	}

	json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
	if (!cJSON_IsNumber(json_receiver_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(json_receiver_id) \n");
		return FALSE;
	}

	json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");
	if (!cJSON_IsNumber(json_local_message_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(json_local_message_id) \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 *
 * @attention
 *
 * @return boole
 */
static boole client_msg__is_json_edit_channel_request_valid(cJSON *json_root, int client_index)
{
	cJSON *json_channel_id = 0;
	cJSON *json_channel_name = 0;
	cJSON *json_channel_description = 0;
	cJSON *json_channel_password = 0;
	cJSON *json_message_object = 0;
	cJSON *json_is_audio_enabled = 0;

	int status = 0;

	//existence of "message" already checked before
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	//
	// json_message_object exists, continue validating received json
	//

	json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
	if (!cJSON_IsNumber(json_channel_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(channel_id) \n");
		return FALSE;
	}

	//cannot edit root channel
	if (json_channel_id->valueint == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(channel_id) \n");
		return FALSE;
	}

	if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (MAX_CHANNELS - 1)))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (MAX_CHANNELS - 1)) \n");
		return FALSE;
	}

	json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
	if (!cJSON_IsString(json_channel_name))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(channel_name) \n");
		return FALSE;
	}

	if (json_channel_name->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " channel_name->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_channel_name->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(channel_name->valuestring) \n");
		return FALSE;
	}

	json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
	if (!cJSON_IsString(json_channel_description))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(channel_description) \n");
		return FALSE;
	}

	if (json_channel_description->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " channel_description->valuestring == NULL \n");
		return FALSE;
	}

	json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
	if (!cJSON_IsString(json_channel_password))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_channel_password) \n");
		return FALSE;
	}

	if (json_channel_password->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_channel_password->valuestring == NULL \n");
		return FALSE;
	}

	json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
	if (!cJSON_IsBool(json_is_audio_enabled))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsBool(json_is_audio_enabled) \n");
		return FALSE;
	}

	status = (int)clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50);

	if (status == -1)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50); \n");
		return FALSE;
	}

	status = (int)clib__utf8_string_length_check_max_length(json_channel_description->valuestring, 1000);

	if (status == -1)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_description->valuestring, 1000); \n");
		return FALSE;
	}

	status = (int)clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30);

	if (status == -1)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30); \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 *
 * @attention
 *
 * @return boole
 */
static boole client_msg__is_json_create_channel_request_valid(cJSON *json_root, int client_index)
{
	cJSON *json_parent_channel_id = 0;
	cJSON *json_channel_name = 0;
	cJSON *json_channel_description = 0;
	cJSON *json_channel_password = 0;
	cJSON *json_is_audio_enabled = 0;
	cJSON *json_message_object = 0;

	int status = 0;

	//existence of "message" already checked before
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	//
	// json_message_object exists, continue validating received json
	//

	json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
	if (!cJSON_IsNumber(json_parent_channel_id))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsNumber(json_parent_channel_id) \n");
		return FALSE;
	}

	json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
	if (!cJSON_IsString(json_channel_name))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(channel_name) \n");
		return FALSE;
	}

	if (json_channel_name->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " channel_name->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_channel_name->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(channel_name->valuestring) \n");
		return FALSE;
	}

	json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
	if (!cJSON_IsString(json_channel_description))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(channel_description) \n");
		return FALSE;
	}

	if (json_channel_description->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " channel_description->valuestring == NULL \n");
		return FALSE;
	}

	json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
	if (!cJSON_IsString(json_channel_password))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_channel_password) \n");
		return FALSE;
	}

	if (json_channel_password->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_channel_password->valuestring == NULL \n");
		return FALSE;
	}

	json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
	if (!cJSON_IsBool(json_is_audio_enabled))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsBool(json_is_audio_enabled) \n");
		return FALSE;
	}

	status = (int)clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50);

	if (status == -1)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50); \n");
		return FALSE;
	}

	status = (int)clib__utf8_string_length_check_max_length(json_channel_description->valuestring, 1000);

	if (status == -1)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_description->valuestring, 1000); \n");
		return FALSE;
	}

	status = (int)clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30);

	if (status == -1)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30); \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root
 * @param int client_index
 *
 * @attention this function is static, only used within client_message.c
 *
 * @return boole
 */
static boole client_msg__is_public_key_info_message_valid(cJSON *json_root, int client_index)
{
	cJSON *json_message_type = 0;
	cJSON *json_message_value = 0;
	cJSON *verification_string = 0;
	cJSON *json_message_object = 0;

	boole status = FALSE;

	//
	//"message" object is fetched from json_root, again
	//

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	if (json_message_object == 0)
	{
		//no object is not present under key "message", not valid
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_object is null\n");
		return FALSE;
	}

	if (!cJSON_IsObject(json_message_object))
	{
		//message key exists but it does not store object in it, not valid
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsObject(json_message_object) == false\n");
		return FALSE;
	}

	//
	// json_message_object exists, continue validating received json
	//

	json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
	if (!cJSON_IsString(json_message_type))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " cJSON_IsString(json_message_type) \n");
		return FALSE;
	}

	if (json_message_type->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_type->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_message_type->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(json_message_type->valuestring) \n");
		return FALSE;
	}

	status = clib__is_string_equal(json_message_type->valuestring, "public_key_info");

	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, "clib__is_string_equal(json_message_type->valuestring, \"public_key_info\") \n");
		return FALSE;
	}

	//
	// type is verified, continue validating received json
	//

	json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

	if (!cJSON_IsString(json_message_value))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_type != string \n");
		return FALSE;
	}

	if (json_message_value->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_value->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(json_message_value->valuestring) != 344)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(json_message_value->valuestring) != 344 \n");
		return FALSE;
	}

	//
	// its verified that json contains clients public key, continue json validation
	//

	verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
	if (!cJSON_IsString(verification_string))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_type != string \n");
		return FALSE;
	}

	if (verification_string->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " verification_string->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(verification_string->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(verification_string->valuestring) \n");
		return FALSE;
	}

	//
	//its verified that json contains verification_string, continue json validation
	//

	verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "dh_public_mix");
	if (!cJSON_IsString(verification_string))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " dh_public_mix != string \n");
		return FALSE;
	}

	if (verification_string->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " dh_public_mix->valuestring == NULL \n");
		return FALSE;
	}

	if (clib__utf8_string_length(verification_string->valuestring) == 0)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " clib__utf8_string_length(dh_public_mix->valuestring) \n");
		return FALSE;
	}

	//
	//its verified that json contains dh_public_mix, json appears to be valid
	//

	return TRUE;
}

/**
 * @brief Helper function. gets challenge string from json.
 *
 * @param cJSON* json_root
 *
 * @return char* returns  the challenge string
 *
 * @attention this function assumes json_root is already verified , has correct data, if not this function will cause crash of entire server
 */
static char *client_msg__get_challenge_string(cJSON *json_root)
{
	cJSON *json_message_object;
	cJSON *challenge_string;

	char *result;

	//
	//"message" object is fetched from json_root, again
	//

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	challenge_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

	result = challenge_string->valuestring;

	return result;
}

/**
 * @brief This function processes connection check message from client
 *
 * @param cJSON* json_root
 * @param int client_index
 *
 * @attention this function is static, only used within client_message.c
 *
 * @return boole
 */
static boole is_public_key_challenge_response_valid(cJSON *json_root, int client_index)
{
	cJSON *json_message_value = 0;
	cJSON *json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	if (json_message_object == 0)
	{
		//no object is not present under key "message", not valid
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_object is null\n");
		return FALSE;
	}

	json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

	if (!cJSON_IsString(json_message_value))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_type != string \n");
		return FALSE;
	}

	if (json_message_value->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_value->valuestring == NULL \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief This function checks if clients username change request json message has all nessecary fields
 *
 * @param cJSON* json_root
 *
 * @attention this function is static, only used within client_message.c
 *
 * @return boole
 */
static boole client_msg__is_change_client_username_message_valid(cJSON *json_root, int client_index)
{
	cJSON *json_message_value = 0;
	cJSON *json_message_object = 0;

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	if (json_message_object == 0)
	{
		//no object is not present under key "message", not valid
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " json_message_object is null\n");
		return FALSE;
	}

	json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");

	if (!cJSON_IsString(json_message_value))
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " new_username != string \n");
		return FALSE;
	}

	if (json_message_value->valuestring == NULL)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", client_index, " new_username->valuestring == NULL \n");
		return FALSE;
	}

	return TRUE;
}

/**
 * @brief Helper function. gets few things from json
 *
 * @param cJSON* json_root
 * @param char** public_key
 * @param char** verification_string
 * @param char** dh_mix *
 *
 * @return void
 *
 * @attention this function assumes json_root is already verified , has correct data, if not this function will cause crash of entire server
 */
void client_msg__get_public_key_and_verification_string_and_dh_public_mix(cJSON *json_root, char **public_key, char **verification_string, char **dh_mix)
{
	cJSON *public_key_json;
	cJSON *verification_string_json;
	cJSON *json_message_object;
	cJSON *dh_public_mix_json;

	//
	//"message" object is fetched from json_root, again
	//

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	public_key_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	verification_string_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
	dh_public_mix_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "dh_public_mix");

	*verification_string = verification_string_json->valuestring;
	*public_key = public_key_json->valuestring;
	*dh_mix = dh_public_mix_json->valuestring;
}

/**
 * @brief this is first message server processes from client
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @attention this function will be improved. the "diffie-hellman key exchange" here is not secure enough it just tries to imitate the diffie hellman key exchange
 *
 * @return boole
 */
void client_msg__process_public_key_info(cJSON *json_root, int sender_client_index)
{
	boole status;
	mp_int bignum_dh_secret_exponent_of_this_server;
	char *public_key;
	char *verification_string;
	char *dh_received_public_mix_from_client;
	unsigned int dh_server_secret_exponent;
	mp_int bignum_dh_received_public_mix_from_client;
	mp_int bignum_dh_shared_secret;
	mp_err mpstatus1;
	size_t written;
	mp_int bignum_dh_public_mix_from_server_for_client;
	mp_int bignum_dh_public_modulus;
	mp_int bignum_dh_public_base_generator;
	size_t written1;
	char *challenge_string;
	char *dh_public_mix_from_server_string_for_client;
	char *challenge_value_for_client;
	boole is_everything_ok_still;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_public_key_info base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_public_key_info_message_valid(json_root, sender_client_index);
	if (!status)
	{
		base__close_websocket_connection(sender_client_index, TRUE);
		DBG_AUTHENTICATION log_info("%s %d %s", "client : ", sender_client_index, " json_message_type != string \n");
		return;
	}

	DBG_AUTHENTICATION log_info("%s %d %s", "client : ", sender_client_index, " everything looks valid, proceeding with authentication\n");

	//
	//verification string should be "welcome"
	//there also should not be another connected client using same public_key, loop through clients, find out if anyone is using same public key
	//

	public_key = 0;
	verification_string = 0;
	dh_received_public_mix_from_client = 0; //diffie hellman received (product of public base to private exponent received from client)

	client_msg__get_public_key_and_verification_string_and_dh_public_mix(json_root, &public_key, &verification_string, &dh_received_public_mix_from_client);

	status = clib__is_string_equal(verification_string, "welcome");

	if (!status)
	{
		DBG_AUTHENTICATION log_info("%s %d %s", "client : ", sender_client_index, " verification string not welcome  \n");
		base__close_websocket_connection(sender_client_index, TRUE);
		return;
	}

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);
	status = base__is_there_a_client_with_same_public_key(public_key);
	pthread_rwlock_unlock(&clients_global_rwlock_guard);

	if (status)
	{
		DBG_AUTHENTICATION log_info("%s %d %s", "client : ", sender_client_index, " duplicate client \n");
		base__close_websocket_connection(sender_client_index, TRUE);
		return;
	}

	/* computation expensive part of code, but */

	DBG_AUTHENTICATION log_info("%s %s %s", "client public_key : ", public_key, " \n");

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);
	clib__copy_memory(public_key, &clients_array[sender_client_index].public_key[0], clib__utf8_string_length(public_key), 1000);
	pthread_rwlock_unlock(&clients_global_rwlock_guard);

	//
	//create modulus. Client and server have same modulus
	//

	cstring dh_known_modulus_str = "13232376895198612407547930718267435757728527029623408872245156039757713029036368719146452186041204237350521785240337048752071462798273003935646236777459223";
	mpstatus1 = mp_init(&bignum_dh_public_modulus);
	status = mp_read_radix(&bignum_dh_public_modulus, (char *)dh_known_modulus_str, 10);
	if (status != MP_OKAY)
	{
		printf("%s", "mp_read_radix error \n");
	}

	//
	//create secret exponent used by server
	//

	dh_server_secret_exponent |= 1 << 10;
	int a = rand();
	dh_server_secret_exponent |= (a & 127);
	dh_server_secret_exponent = 1092;
	DBG_AUTHENTICATION log_info("%s %d", "dh_secret_exponent ", dh_server_secret_exponent);
	mpstatus1 = mp_init_i64(&bignum_dh_secret_exponent_of_this_server, dh_server_secret_exponent);

	//
	//convert string public mix received from client to mp_int
	//

	mpstatus1 = mp_init(&bignum_dh_received_public_mix_from_client);
	status = mp_read_radix(&bignum_dh_received_public_mix_from_client, (char *)dh_received_public_mix_from_client, 10);
	if (status != MP_OKAY)
	{
		printf("%s", "mp_read_radix error \n");
	}

	DBG_AUTHENTICATION log_info("%s", "step 2 \n");

	mpstatus1 = mp_init(&bignum_dh_shared_secret);

	//
	//take public mix received from client, multiple by itself n number of times (pow function) where n is bignum_dh_secret_exponent_of_this_server
	//then use modulo on the result of the pow, and store result of modulo operation in bignum_dh_shared_secret
	//result of modulo operation is bignum_dh_shared_secret
	//

	status = mp_exptmod(&bignum_dh_received_public_mix_from_client, &bignum_dh_secret_exponent_of_this_server, &bignum_dh_public_modulus, &bignum_dh_shared_secret);
	if (status != MP_OKAY)
	{
		printf("%s", "mp_exptmod error \n");
	}

	//
	//convert shared secret to readable string
	//

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	is_everything_ok_still = FALSE;

	if (clients_array[sender_client_index].is_existing)
	{
		status = mp_to_radix(&bignum_dh_shared_secret, clients_array[sender_client_index].dh_shared_secret, 1000, &written, 10);
		if (status != MP_OKAY)
		{
			DBG_AUTHENTICATION log_info("%s", "mp_to_radix bignum_dh_shared_secret error \n");
		}

		DBG_AUTHENTICATION log_info("%s %s %s", "bignum_dh_shared_secret: ", clients_array[sender_client_index].dh_shared_secret, " \n");

		//
		//very important, save the shared secret
		//

		clients_array[sender_client_index].is_dh_shared_secret_agreed_upon = TRUE;
		is_everything_ok_still = TRUE;
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);

	if (is_everything_ok_still == FALSE)
	{
		DBG_AUTHENTICATION log_info("%s %d %s", "turns out client: ", sender_client_index, "doenst exist anymore \n");
		return;
	}

	DBG_AUTHENTICATION log_info("%s %s %s", "clients_array[client_id].dh_shared_secret: ", &clients_array[sender_client_index].dh_shared_secret[0], " \n");

	//
	//create public mix that will be sent TO client
	//

	mpstatus1 = mp_init_i64(&bignum_dh_public_base_generator, 2);

	mpstatus1 = mp_init(&bignum_dh_public_mix_from_server_for_client);
	status = mp_exptmod(&bignum_dh_public_base_generator, &bignum_dh_secret_exponent_of_this_server, &bignum_dh_public_modulus, &bignum_dh_public_mix_from_server_for_client);
	if (status != MP_OKAY)
	{
		DBG_AUTHENTICATION log_info("%s", "mp_exptmod error \n");
	}

	//
	//public mix is sent as decimal number in string format in json, thats why the usage of mp_to_radix
	//

	dh_public_mix_from_server_string_for_client = (char *)memorymanager__allocate(1000, MEMALLOC_DHPROCESS);
	status = mp_to_radix(&bignum_dh_public_mix_from_server_for_client, dh_public_mix_from_server_string_for_client, 1000, &written1, 10);
	if (status != MP_OKAY)
	{
		DBG_AUTHENTICATION log_info("%s", "mp_to_radix bignum_dh_shared_secret error \n");
	}

	DBG_AUTHENTICATION log_info("%s %s", "WTF public_key -> ", public_key);

	//
	//challenge string to verify public key
	//

	challenge_string = (char *)memorymanager__allocate(128, MEMALLOC_TYPE_CHALLENGE);

#define CHALLENGE_STRING_SIZE 100
	base__fill_block_of_data_with_ascii_characters(challenge_string, CHALLENGE_STRING_SIZE);

	DBG_AUTHENTICATION log_info("%s %s %s", "challenge_string: ", challenge_string, " \n");

	challenge_value_for_client = base__encrypt_string_with_public_key(public_key, (unsigned char *)challenge_string, (uint64)clib__utf8_string_length(challenge_string));
	DBG_AUTHENTICATION log_info("%s %s %s", "dh_public_mix_for_client: ", dh_public_mix_from_server_string_for_client, " \n");
	server_msg__send_public_key_challenge_to_single_client(clients_array[sender_client_index].p_ws_connection, challenge_value_for_client, dh_public_mix_from_server_string_for_client);
	memorymanager__free((nuint)challenge_value_for_client);

	is_everything_ok_still = FALSE;

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	if (clients_array[sender_client_index].is_existing)
	{
		clib__copy_memory(challenge_string, &clients_array[sender_client_index].challenge_string[0], CHALLENGE_STRING_SIZE, 128);
		clients_array[sender_client_index].is_public_key_challenge_sent = TRUE;
		is_everything_ok_still = TRUE;
		DBG_AUTHENTICATION log_info("%s", " public key challenge is sent \n");
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);

	memorymanager__free((nuint)challenge_string);
	memorymanager__free((nuint)dh_public_mix_from_server_string_for_client);

	if (is_everything_ok_still == FALSE)
	{
		DBG_AUTHENTICATION log_info("%s", " public key challenge failed something didnt go right \n");
		base__close_websocket_connection(sender_client_index, TRUE);
	}
}

/**
 * @brief processes public key challenge response from client
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @attention this function is static, only used within client_message.c
 *
 * @return boole
 */
void client_msg__process_public_key_challenge_response(cJSON *json_root, int sender_client_index)
{
	boole status;
	char *received_challenge_string;
	int client_count_in_root_channel;
	int channel_index;
	channel_t *root_channel;

	DBG_AUTHENTICATION log_info("%s", "co sa tu sakra deje 1 \n");

	//status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	//if (!status)
	//{
	//    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_public_key_challenge_response base__is_request_allowed_based_on_spam_protection == FALSE \n");
	//    return;
	//}

	status = is_public_key_challenge_response_valid(json_root, sender_client_index);
	if (!status)
	{
		base__close_websocket_connection(sender_client_index, TRUE);
		DBG_AUTHENTICATION log_info("%s %d %s", "client_msg__process_public_key_challenge_response deleting client because challenge response not valid: client index ", sender_client_index, " \n");
		return;
	}

	DBG_AUTHENTICATION log_info("%s", "co sa tu sakra deje 2 \n");

	//
	//client sends public key to server at the time of authentication
	//server generates random string, encrypts that string with the clients public key
	//server then verifies if the client really is the owner of public key by sending client a little challenge
	//"if the public key is really yours, client, please, decrypt and then send back this randomly generated string that I will send you. you will have no problem telling me what I sent you, if its really your key"
	//something like that
	//

	client_t *current_client = &clients_array[sender_client_index];
	if (!current_client->is_public_key_challenge_sent)
	{
		base__close_websocket_connection(sender_client_index, TRUE);
		DBG_AUTHENTICATION log_info("%s %d %s", "client_msg__process_public_key_challenge_response deleting client because !current_client->is_public_key_challenge_sent : client index ", sender_client_index, " \n");
		return;
	}

	//
	//compare the key
	//

	received_challenge_string = client_msg__get_challenge_string(json_root);

	status = clib__is_string_equal(clients_array[sender_client_index].challenge_string, received_challenge_string);

	if (status)
	{
		DBG_AUTHENTICATION log_info("%s %d %s", "client_msg__process_public_key_challenge_response challenge response string match : client index ", sender_client_index, " \n");

		pthread_rwlock_wrlock(&clients_global_rwlock_guard);

		current_client->channel_id = 0;
		current_client->is_admin = FALSE;
		current_client->is_authenticated = TRUE;
		current_client->timestamp_last_maintain_connection_message_received = base__get_timestamp_ms();

		if (g_server_settings.is_display_country_flags_active)
		{
			ip_tools_load_iso_country_code(current_client->ip_address, current_client->country_iso_code);
		}

		status = base__assign_username_for_newly_joined_client(sender_client_index, g_server_settings.default_client_name);
		pthread_rwlock_unlock(&clients_global_rwlock_guard);

		if (!status)
		{
			base__close_websocket_connection(sender_client_index, FALSE);
			DBG_AUTHENTICATION log_info("%s %d %s", "client_msg__process_public_key_challenge_response deleting client base__assign_username_for_newly_joined_client returned false : client index ", sender_client_index, " \n");
			return;
		}

		server_msg__send_authentication_status_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);

		//its better when readlock is placed here instead of it being placed directly in server_msg__send_channel_list_to_single_client function

		pthread_rwlock_wrlock(&clients_global_rwlock_guard);
		pthread_rwlock_wrlock(&channels_global_rwlock_guard);
		pthread_rwlock_rdlock(&tags_global_rwlock_guard);
		pthread_rwlock_rdlock(&icons_global_rwlock_guard);

		server_msg__send_channel_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);
		server_msg__send_client_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret, current_client->username, current_client->client_id);
		server_msg__send_icon_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);
		server_msg__send_tag_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);
		server_msg__send_active_microphone_usage_for_current_channel_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret, current_client->channel_id);
		server_msg__send_client_connect_message_to_all_clients(current_client->client_id);

		client_count_in_root_channel = base__get_client_count_for_channel(ROOT_CHANNEL_ID);

		root_channel = &channel_array[ROOT_CHANNEL_ID];

		if (client_count_in_root_channel == 1)
		{
			root_channel->maintainer_id = current_client->client_id;
			root_channel->is_channel_maintainer_present = TRUE;
			server_msg__send_maintainer_id_to_single_client(current_client, ROOT_CHANNEL_ID, current_client->client_id);
		}
		else
		{
			server_msg__send_maintainer_id_to_single_client(current_client, ROOT_CHANNEL_ID, root_channel->maintainer_id);
		}

		audio_channel__initialize_webrtc_datachannel_connection(current_client);

		pthread_rwlock_unlock(&clients_global_rwlock_guard);
		pthread_rwlock_unlock(&channels_global_rwlock_guard);
		pthread_rwlock_unlock(&tags_global_rwlock_guard);
		pthread_rwlock_unlock(&icons_global_rwlock_guard);

		//send_cross_thread_message_create_new_client_at_rtc_thread(sender, client_id);
	}
	else
	{
		DBG_AUTHENTICATION log_info("%s", " challenge string not equal \n");
		base__close_websocket_connection(sender_client_index, TRUE);
	}
}

/**
 * @brief This function processes connection check message from client
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @attention this function uses write lock on clients_global_rwlock_guard
 *
 * @return void
 */
void client_msg__process_client_connection_check(cJSON *json_root, int sender_client_index)
{
	int i = 0;
	client_t *client;

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	if (clients_array[sender_client_index].is_authenticated)
	{
		clients_array[sender_client_index].timestamp_last_maintain_connection_message_received = base__get_timestamp_ms();
		server_msg__send_connection_check_response_to_single_client(&clients_array[sender_client_index]);
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief This function processes connection check message from client
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @attention this function uses write lock on clients_global_rwlock_guard
 *
 * @return void
 */
void client_msg__process_change_client_username(cJSON *json_root, int sender_client_index)
{
	boole status;
	cJSON *json_message_value = 0;
	cJSON *json_message_object = 0;
	boole is_username_taken = FALSE;
	boole is_change_success = FALSE;
	int new_username_length = 0;
	int i;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_client_username base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_change_client_username_message_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_client_username client_msg__is_change_client_username_message_valid == FALSE \n");
		return;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		//is_existing needs to be guarded with mutex, while opening client, rwlock is not usable

		//log_info("%s %s %d %s", "trying username -> ", loop_username_buffer , i, "\n");

		if (!clients_array[i].is_existing)
		{ //client not is_existing, skip, this need global lock
			continue;
		}

		if (!clients_array[i].is_authenticated)
		{
			continue;
		}

		is_username_taken = clib__is_string_equal(clients_array[i].username, json_message_value->valuestring);

		if (is_username_taken)
		{
			//username used by some of the clients, start another loop, with incremented numeric part of clients username
			DBG_CLIENT_MESSAGE log_info("%s %d %s", "username ", is_username_taken, " already taken \n");
			break;
		}
	}

	if (is_username_taken == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %s %s", "username ", json_message_value->valuestring, " not taken \n");

		if (clients_array[sender_client_index].is_authenticated)
		{
			new_username_length = (int)clib__utf8_string_length_check_max_length(json_message_value->valuestring, 50);
			if (new_username_length == -1)
			{
				DBG_CLIENT_MESSAGE log_info("%s %s %s", "username ", json_message_value->valuestring, " max length exceeded 50 characters \n");
			}
			else if (new_username_length > 0)
			{
				clib__null_memory(clients_array[sender_client_index].username, USERNAME_MAX_LENGTH);
				clib__copy_memory(json_message_value->valuestring, &clients_array[sender_client_index].username[0], new_username_length, USERNAME_MAX_LENGTH);
				is_change_success = TRUE;
			}
		}
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);

	if (is_change_success)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__send_client_rename_message_to_all_clients \n");

		pthread_rwlock_rdlock(&clients_global_rwlock_guard);
		server_msg__send_client_rename_message_to_all_clients(sender_client_index, clients_array[sender_client_index].username);
		pthread_rwlock_unlock(&clients_global_rwlock_guard);
	}
}

/**
 * @brief This function processes create channel request message
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @attention this function aquires read and write locks for channel_array and clients_array
 *
 * @return void
 */
void client_msg__process_create_channel_request(cJSON *json_root, int sender_client_index)
{
	boole is_channel_create_allowed = FALSE;
	boole is_parent_channel_id_existing = FALSE;
	cJSON *json_parent_channel_id = 0;
	cJSON *json_channel_description = 0;
	cJSON *json_channel_password = 0;
	cJSON *json_channel_name = 0;
	cJSON *json_is_audio_enabled = 0;
	boole is_password_used = FALSE;
	boole is_channel_created_successfully = FALSE;
	cJSON *json_message_object = 0;
	int i = 0;
	int created_channel_index;
	int channel_creator_client_index;
	channel_t *channel;
	boole status;

	//
	//check timestamp first
	//check if json is valid
	//check access rights
	//check if parent channel id is valid, if parent channel is is_existing
	//finally, create channel
	//

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_create_channel_request_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request client_msg__is_json_create_channel_request_valid == FALSE \n");
		return;
	}

	//doesnt really need read lock,

	is_channel_create_allowed = TRUE;

	if (g_server_settings.is_restrict_channel_deletion_creation_editing_to_admin_active)
	{
		is_channel_create_allowed = FALSE;
		pthread_rwlock_rdlock(&clients_global_rwlock_guard);
		if (clients_array[sender_client_index].is_authenticated && clients_array[sender_client_index].is_existing)
		{
			if (clients_array[sender_client_index].is_admin == TRUE)
			{
				is_channel_create_allowed = TRUE;
			}
		}
		pthread_rwlock_unlock(&clients_global_rwlock_guard);
	}

	//is_channel_create_allowed = TRUE;

	if (is_channel_create_allowed)
	{
		//what is done here:
		// checking if parent channel exists,
		// creating the child channel
		//both need to be wrapper within same write lock because they are tied to one another,

		pthread_rwlock_wrlock(&channels_global_rwlock_guard);
		json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
		json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
		json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
		json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
		json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
		json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");

		for (i = 0; i < MAX_CHANNELS; i++)
		{
			if (channel_array[i].is_existing == FALSE)
			{
				continue;
			}

			if (channel_array[i].channel_id == (int)json_parent_channel_id->valuedouble)
			{
				is_parent_channel_id_existing = TRUE;
				break;
			}
		}

		if (is_parent_channel_id_existing)
		{
			DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request is_parent_channel_id_existing == TRUE \n");

			for (i = 0; i < MAX_CHANNELS; i++)
			{
				channel = &channel_array[i];

				if (channel->is_existing == FALSE)
				{
					//
					//everything is in order, now create the channel
					//set is_channel_created_successfully to TRUE so message about channel creation will be sent to all clients
					//will be initiated later in code
					//

					channel->is_existing = TRUE;
					channel->channel_id = i; //????
					channel->parent_channel_id = (int)json_parent_channel_id->valuedouble;
					clib__copy_memory(json_channel_name->valuestring, channel->name, clib__utf8_string_length(json_channel_name->valuestring), CHANNEL_NAME_MAX_LENGTH);
					clib__copy_memory(json_channel_password->valuestring, channel->password, clib__utf8_string_length(json_channel_password->valuestring), CHANNEL_PASSWORD_MAX_LENGTH);
					clib__copy_memory(json_channel_description->valuestring, channel->description, clib__utf8_string_length(json_channel_description->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH);
					channel->maintainer_id = -1;
					channel->is_channel_maintainer_present = FALSE;
					is_password_used = (boole)(clib__utf8_string_length(json_channel_password->valuestring) > 0);
					channel->is_using_password = is_password_used;
					channel->is_audio_enabled = (boole)cJSON_IsTrue(json_is_audio_enabled);

					DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_create_channel_request is_parent_channel_id_existing ", i, "\n");
					is_channel_created_successfully = TRUE;
					created_channel_index = i;
					channel_creator_client_index = sender_client_index;

					break;
				}
			}
		}
		else
		{
			DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request is_parent_channel_id_existing == FALSE \n");
		}
		pthread_rwlock_unlock(&channels_global_rwlock_guard);

		//okay, channel created successfully, aquire read lock for channels and clients
		if (is_channel_created_successfully)
		{
			pthread_rwlock_rdlock(&clients_global_rwlock_guard);
			pthread_rwlock_rdlock(&channels_global_rwlock_guard);

			server_msg__send_channel_create_message_to_all_clients(created_channel_index, channel_creator_client_index);

			pthread_rwlock_unlock(&clients_global_rwlock_guard);
			pthread_rwlock_unlock(&channels_global_rwlock_guard);
		}
	}
	else
	{
		pthread_rwlock_rdlock(&clients_global_rwlock_guard);
		if (clients_array[sender_client_index].is_authenticated && clients_array[sender_client_index].is_existing)
		{
			server_msg__send_access_denied_to_single_client(&clients_array[sender_client_index]);
		}
		pthread_rwlock_unlock(&clients_global_rwlock_guard);
	}
}

/**
 * @brief This function processes create edit request message
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @attention this function aquires read and write locks for channel_array and clients_array
 *
 * @return void
 */
void client_msg__process_edit_channel_request(cJSON *json_root, int sender_client_index)
{
	boole is_channel_edit_allowed = FALSE;
	cJSON *json_channel_id = 0;
	cJSON *json_channel_description = 0;
	cJSON *json_channel_password = 0;
	cJSON *json_channel_name = 0;
	boole is_password_used = FALSE;
	boole is_channel_edited_successfully = FALSE;
	cJSON *json_message_object = 0;
	cJSON *json_is_audio_enabled = 0;
	int channel_index_to_edit;
	channel_t *channel;
	boole status;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_edit_channel_request_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request client_msg__is_json_edit_channel_request_valid == FALSE \n");
		return;
	}

	//doesnt really need read lock,

	is_channel_edit_allowed = TRUE;

	if (g_server_settings.is_restrict_channel_deletion_creation_editing_to_admin_active)
	{
		is_channel_edit_allowed = FALSE;
		pthread_rwlock_rdlock(&clients_global_rwlock_guard);
		if (clients_array[sender_client_index].is_authenticated && clients_array[sender_client_index].is_existing)
		{
			if (clients_array[sender_client_index].is_admin == TRUE)
			{
				is_channel_edit_allowed = TRUE;
			}
		}
		pthread_rwlock_unlock(&clients_global_rwlock_guard);
	}

	if (is_channel_edit_allowed)
	{
		//what is done here:
		// checking if parent channel exists,
		// creating the child channel
		//both need to be wrapper within same write lock because they are tied to one another,

		pthread_rwlock_wrlock(&channels_global_rwlock_guard);
		json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
		json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
		json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
		json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
		json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
		json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");

		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request is_parent_channel_id_existing == TRUE \n");

		channel_index_to_edit = json_channel_id->valueint;

		channel = &channel_array[channel_index_to_edit];

		if (channel->is_existing == TRUE)
		{
			clib__null_memory(channel->name, CHANNEL_NAME_MAX_LENGTH);
			clib__null_memory(channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
			clib__null_memory(channel->description, CHANNEL_DESCRIPTION_MAX_LENGTH);

			clib__copy_memory(json_channel_name->valuestring, channel->name, clib__utf8_string_length(json_channel_name->valuestring), CHANNEL_NAME_MAX_LENGTH);
			clib__copy_memory(json_channel_password->valuestring, channel->password, clib__utf8_string_length(json_channel_password->valuestring), CHANNEL_PASSWORD_MAX_LENGTH);
			clib__copy_memory(json_channel_description->valuestring, channel->description, clib__utf8_string_length(json_channel_description->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH);

			is_password_used = (boole)(clib__utf8_string_length(json_channel_password->valuestring) > 0);
			channel->is_using_password = is_password_used;
			channel->is_audio_enabled = (boole)cJSON_IsTrue(json_is_audio_enabled);

			DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request is_parent_channel_id_existing TRUE \n");
			is_channel_edited_successfully = TRUE;
		}

		pthread_rwlock_unlock(&channels_global_rwlock_guard);

		//okay, channel created successfully, aquire read lock for channels and clients
		if (is_channel_edited_successfully)
		{
			pthread_rwlock_rdlock(&clients_global_rwlock_guard);
			pthread_rwlock_rdlock(&channels_global_rwlock_guard);

			server_msg__send_channel_edit_message_to_all_clients(channel_index_to_edit, sender_client_index);

			pthread_rwlock_unlock(&clients_global_rwlock_guard);
			pthread_rwlock_unlock(&channels_global_rwlock_guard);
		}
	}
	else
	{
		pthread_rwlock_rdlock(&clients_global_rwlock_guard);
		if (clients_array[sender_client_index].is_authenticated && clients_array[sender_client_index].is_existing)
		{
			server_msg__send_access_denied_to_single_client(&clients_array[sender_client_index]);
		}
		pthread_rwlock_unlock(&clients_global_rwlock_guard);
	}
}

/**
 * @brief This function processes direct chat message
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_direct_chat_message(cJSON *json_root, int sender_client_index)
{
	boole is_message_valid = FALSE;
	boole is_receiver_existing = FALSE;
	cJSON *json_receiver_id = 0;
	cJSON *json_local_message_id = 0;
	cJSON *json_chat_message_value = 0;
	cJSON *json_message_object = 0;

	int server_chat_message_id;

	//
	//because maintainer of channel sends out channel keys to each client individually,
	//and a maintainer needs to do that as quickly as possible, repeatedly, possibly for hundred clients in everyone is in his channel
	//spam prevention cannot be placed here like in other places, needs to be improved
	//

	is_message_valid = client_msg__is_json_chat_message_format_valid(json_root, sender_client_index);

	if (!is_message_valid)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_message client_msg__is_json_chat_message_format_valid == FALSE \n");
		return;
	}

	DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_message got here \n");
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
	json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

	if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= MAX_CLIENTS)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", sender_client_index, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= MAX_CLIENTS \n");
		return;
	}

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);

	is_receiver_existing = clients_array[json_receiver_id->valueint].is_authenticated;

	if (is_receiver_existing)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "receiver is_existing: ", json_receiver_id->valueint, "\n");

		server_chat_message_id = (int)base___get_chat_message_id();
		base___increment_chat_message_id();
		server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(sender_client_index, server_chat_message_id, json_local_message_id->valueint);
		server_msg__send_chat_message_to_single_client(sender_client_index, json_receiver_id->valueint, server_chat_message_id, json_chat_message_value->valuestring);
	}
	else
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "receiver is_existing not is_existing", json_receiver_id->valueint, "\n");
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief This function processes direct chat message
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_channel_chat_message(cJSON *json_root, int sender_client_index)
{
	boole is_channel_existing = FALSE;
	//cJSON* json_receiver_id = 0; json contains this ID, but it is not needed. Id of channel is taken from senders client struct
	cJSON *json_local_message_id = 0;
	cJSON *json_chat_message_value = 0;
	cJSON *json_message_object = 0;
	int channel_id;
	int server_chat_message_id;

	boole status;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_message base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_chat_message_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_message client_msg__is_json_chat_message_format_valid == FALSE \n");
		return;
	}

	DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_message got here \n");
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);
	pthread_rwlock_rdlock(&channels_global_rwlock_guard);

	channel_id = clients_array[sender_client_index].channel_id;
	is_channel_existing = channel_array[channel_id].is_existing;

	if (is_channel_existing)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "receiving channel exists: ", channel_id, "\n");

		server_chat_message_id = (int)base___get_chat_message_id();
		base___increment_chat_message_id();
		server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(sender_client_index, server_chat_message_id, json_local_message_id->valueint);
		server_msg__send_chat_message_to_clients_in_same_channel(sender_client_index, channel_id, server_chat_message_id, json_chat_message_value->valuestring);
	}
	else
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "receiving channel does not exist: ", channel_id, "\n");
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
	pthread_rwlock_unlock(&channels_global_rwlock_guard);
}

/**
 * @brief This function processes channel chat picture
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_channel_chat_picture(cJSON *json_root, int sender_client_index)
{
	boole is_channel_existing = FALSE;
	cJSON *json_local_message_id = 0;
	cJSON *json_chat_message_value = 0;
	cJSON *json_message_object = 0;
	int channel_id;
	int server_chat_message_id;
	boole status;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_picture base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_chat_message_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_picture client_msg__is_json_chat_message_format_valid == FALSE \n");
		return;
	}

	DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_picture got here \n");
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);
	pthread_rwlock_rdlock(&channels_global_rwlock_guard);

	channel_id = clients_array[sender_client_index].channel_id;
	is_channel_existing = channel_array[channel_id].is_existing;

	if (is_channel_existing)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_channel_chat_picture receiving channel exists: ", channel_id, "\n");

		server_chat_message_id = (int)base___get_chat_message_id();
		base___increment_chat_message_id();
		server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel(sender_client_index, channel_id, server_chat_message_id);
		server_msg__send_channel_chat_picture_to_clients_in_same_channel(sender_client_index, channel_id, server_chat_message_id, json_chat_message_value->valuestring);
		server_msg__send_image_status_to_single_client(&clients_array[sender_client_index], "success");
		server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(sender_client_index, server_chat_message_id, json_local_message_id->valueint);
	}
	else
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_channel_chat_picture receiving channel does not exist: ", channel_id, "\n");
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
	pthread_rwlock_unlock(&channels_global_rwlock_guard);
}

/**
 * @brief This function processes direct chat message
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_direct_chat_picture(cJSON *json_root, int sender_client_index)
{
	boole is_receiver_existing = FALSE;
	cJSON *json_receiver_id = 0;
	cJSON *json_local_message_id = 0;
	cJSON *json_chat_message_value = 0;
	cJSON *json_message_object = 0;
	boole status = FALSE;
	int server_chat_message_id;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_picture base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_chat_message_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_picture client_msg__is_json_chat_message_format_valid == FALSE \n");
		return;
	}

	DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_message got here \n");
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
	json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
	json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

	if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= MAX_CLIENTS)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", sender_client_index, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= MAX_CLIENTS \n");
		return;
	}

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);

	is_receiver_existing = clients_array[json_receiver_id->valueint].is_authenticated;

	if (is_receiver_existing)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "receiver is_existing: ", json_receiver_id->valueint, "\n");

		server_chat_message_id = (int)base___get_chat_message_id();
		base___increment_chat_message_id();
		server_msg__send_chat_picture_metadata_to_single_client(sender_client_index, json_receiver_id->valueint, server_chat_message_id);
		server_msg__send_chat_picture_to_single_client(sender_client_index, json_receiver_id->valueint, server_chat_message_id, json_chat_message_value->valuestring);
		server_msg__send_image_status_to_single_client(&clients_array[sender_client_index], "success");
		server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(sender_client_index, server_chat_message_id, json_local_message_id->valueint);
	}
	else
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "receiver is_existing not is_existing", json_receiver_id->valueint, "\n");
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief This function processes join channel request
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_join_channel_request(cJSON *json_root, int sender_client_index)
{
	boole status;
	cJSON *json_channel_password = 0;
	cJSON *json_channel_id = 0;
	cJSON *json_message_object = 0;
	boole is_client_that_is_leaving_channel_maintainer_of_that_channel = FALSE;
	channel_t *new_channel = 0;
	channel_t *old_channel = 0;
	client_t *client_that_is_joining_channel;
	int new_maintainer_index = 0;
	boole is_maintainer_found = FALSE;
	boole is_authenticated;
	boole is_existing;
	client_t *client_in_some_loop;
	int x;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_join_channel_request_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request client_msg__is_json_join_channel_request_format_valid == FALSE \n");
		return;
	}

	//
	//whole function is wrapped within write locks.
	//because i do not know how to handle multithreaded enviroment properly
	//so I wrap code in write locks to avoid any unpredictable behaviour of this server
	//

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);
	pthread_rwlock_wrlock(&channels_global_rwlock_guard);

	is_authenticated = clients_array[sender_client_index].is_authenticated;
	is_existing = clients_array[sender_client_index].is_existing;

	if (is_existing == FALSE || is_authenticated == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request is_existing == FALSE || is_authenticated == FALSE \n");
		goto client_msg__process_join_channel_request_end;
	}

	DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request got here \n");
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
	json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");

	new_channel = &channel_array[json_channel_id->valueint];

	if (!new_channel->is_existing)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request channel not is_existing \n");
		goto client_msg__process_join_channel_request_end;
	}

	client_that_is_joining_channel = &clients_array[sender_client_index];
	old_channel = &channel_array[client_that_is_joining_channel->channel_id];

	status = (boole)(client_that_is_joining_channel->channel_id == json_channel_id->valueint);

	if (status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request client is already in that channel \n");
		goto client_msg__process_join_channel_request_end;
	}

	//check if password is valid

	if (new_channel->is_using_password)
	{
		status = clib__is_string_equal(new_channel->password, json_channel_password->valuestring) || client_that_is_joining_channel->is_admin;
		if (status)
		{
			DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request correct password \n");
			goto client_msg__process_join_channel_request_continue;
		}
		else
		{
			DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request wrong password \n");
			server_msg__send_access_denied_to_single_client(client_that_is_joining_channel);
			goto client_msg__process_join_channel_request_end;
		}
	}

client_msg__process_join_channel_request_continue:
	DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue  \n");

	//change channel in client struct
	client_that_is_joining_channel->channel_id = json_channel_id->valueint;

	if (old_channel->is_channel_maintainer_present)
	{
		is_client_that_is_leaving_channel_maintainer_of_that_channel = (boole)(old_channel->maintainer_id == client_that_is_joining_channel->client_id);
	}

	if (is_client_that_is_leaving_channel_maintainer_of_that_channel)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue is_client_that_is_leaving_channel_maintainer_of_that_channel TRUE  \n");

		is_maintainer_found = base__find_new_maintainer_for_channel(&new_maintainer_index, old_channel->channel_id, sender_client_index, TRUE);
		if (is_maintainer_found)
		{
			//
			//client that left channel was maintainer of that channel, choose new maintainer
			//then broadcast channel join message
			//then send new maintainer id to clients in that channel so they know who new maintainer is
			//

			DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_join_channel_request_continue maintainer found ", new_maintainer_index, "\n");
			old_channel->is_channel_maintainer_present = TRUE;
			old_channel->maintainer_id = new_maintainer_index;

			//first send join message, then maintainer message for clients in that chnanel
			server_msg__send_channel_join_message_to_all_clients(client_that_is_joining_channel, new_channel);

			//
			//client is joining channel that was hidden and clients in it were not visible to him
			//send clients to him using client_join_channel message
			//
			if (g_server_settings.is_hide_clients_in_password_protected_channels_active && new_channel->is_using_password)
			{
				for (x = 0; x < MAX_CLIENTS; x++)
				{
					client_in_some_loop = &clients_array[x];

					if (client_in_some_loop->is_existing == FALSE)
					{
						continue;
					}

					if (client_in_some_loop->is_authenticated == FALSE)
					{
						continue;
					}

					if (client_in_some_loop->channel_id != new_channel->channel_id)
					{
						continue;
					}

					server_msg__send_channel_join_message_to_single_client(client_in_some_loop, new_channel, client_that_is_joining_channel);
				}
			}

			server_msg__send_maintainer_id_to_clients_in_same_channel(old_channel->channel_id, old_channel->maintainer_id);
		}
		else
		{
			DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue maintainer found  FALSE \n");
			old_channel->is_channel_maintainer_present = FALSE;
			old_channel->maintainer_id = 0;
			server_msg__send_channel_join_message_to_all_clients(client_that_is_joining_channel, new_channel);

			//
			//client is joining channel that was hidden and clients in it were not visible to him
			//send clients to him using client_join_channel message
			//
			if (g_server_settings.is_hide_clients_in_password_protected_channels_active && new_channel->is_using_password)
			{
				for (x = 0; x < MAX_CLIENTS; x++)
				{
					client_in_some_loop = &clients_array[x];

					if (client_in_some_loop->is_existing == FALSE)
					{
						continue;
					}

					if (client_in_some_loop->is_authenticated == FALSE)
					{
						continue;
					}

					if (client_in_some_loop->channel_id != new_channel->channel_id)
					{
						continue;
					}

					server_msg__send_channel_join_message_to_single_client(client_in_some_loop, new_channel, client_that_is_joining_channel);
				}
			}
		}
	}
	else
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue is_client_that_is_leaving_channel_maintainer_of_that_channel FALSE  \n");
		server_msg__send_channel_join_message_to_all_clients(client_that_is_joining_channel, new_channel);

		//
		//client is joining channel that was hidden and clients in it were not visible to him
		//send clients to him using client_join_channel message
		//
		if (g_server_settings.is_hide_clients_in_password_protected_channels_active && new_channel->is_using_password)
		{
			for (x = 0; x < MAX_CLIENTS; x++)
			{
				client_in_some_loop = &clients_array[x];

				if (client_in_some_loop->is_existing == FALSE)
				{
					continue;
				}

				if (client_in_some_loop->is_authenticated == FALSE)
				{
					continue;
				}

				if (client_in_some_loop->channel_id != new_channel->channel_id)
				{
					continue;
				}

				server_msg__send_channel_join_message_to_single_client(client_in_some_loop, new_channel, client_that_is_joining_channel);
			}
		}
	}

	audio_channel__process_client_channel_join(client_that_is_joining_channel);

	//
	//at this point channel is joined
	//but there is still some work to do, find out how many clients are there in newly joined channel,
	// if there is only one client, the newly joined client, he must be the maintainer of it
	//

	if (base__get_client_count_for_channel(new_channel->channel_id) == 1)
	{
		new_channel->maintainer_id = client_that_is_joining_channel->client_id;
		new_channel->is_channel_maintainer_present = TRUE;
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue client that joined channel is the only one in the channel, he is maintainer of it \n");
	}

	server_msg__send_maintainer_id_to_single_client(client_that_is_joining_channel, new_channel->channel_id, new_channel->maintainer_id);

	server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client_that_is_joining_channel->p_ws_connection, client_that_is_joining_channel->dh_shared_secret, client_that_is_joining_channel->channel_id);

client_msg__process_join_channel_request_end:
	pthread_rwlock_unlock(&channels_global_rwlock_guard);
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief This function processes delete channel request
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_delete_channel_request(cJSON *json_root, int sender_client_index)
{
	boole status;
	boole is_authenticated;
	boole is_existing;
	cJSON *json_channel_id = 0;
	cJSON *json_message_object = 0;
	int *channel_ids_to_delete = 0;
	int channels_to_delete_count = 0;
	client_t *client_to_move_maybe = 0;
	int channel_id_to_delete;
	int client_count_in_channel;
	int index_of_new_maintainer;
	boole is_channel_delete_allowed = TRUE;
	int i;
	int ii;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_delete_request_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request client_msg__is_json_join_channel_request_format_valid == FALSE \n");
		return;
	}

	//add admin rights check later

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);
	pthread_rwlock_wrlock(&channels_global_rwlock_guard);

	//again check if client exists and is authenticated
	is_authenticated = clients_array[sender_client_index].is_authenticated;
	is_existing = clients_array[sender_client_index].is_existing;

	if (is_existing == FALSE || is_authenticated == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request is_existing == FALSE || is_authenticated == FALSE \n");
		goto label_client_msg__process_delete_channel_request_end;
	}

	DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request got here \n");
	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

	status = channel_array[json_channel_id->valueint].is_existing;

	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request channel does not exist \n");
		goto label_client_msg__process_delete_channel_request_end;
	}

	if (g_server_settings.is_restrict_channel_deletion_creation_editing_to_admin_active)
	{
		is_channel_delete_allowed = FALSE;
		if (clients_array[sender_client_index].is_authenticated && clients_array[sender_client_index].is_existing)
		{
			if (clients_array[sender_client_index].is_admin == TRUE)
			{
				is_channel_delete_allowed = TRUE;
			}
		}
	}

	if (is_channel_delete_allowed)
	{
		channel_ids_to_delete = (int *)memorymanager__allocate(MAX_CHANNELS * sizeof(int), MEMALLOC_MARKED_CHANNEL_INDICES);
		channel_ids_to_delete[channels_to_delete_count] = json_channel_id->valueint;
		channels_to_delete_count += 1;

		//json_channel_id->valueint channel id to start marking other child channels from
		//channels_to_delete_count count of marked channels to delete
		//array that stores ids of channels that should be deleted
		base__mark_channels_for_deletion(json_channel_id->valueint, &channels_to_delete_count, channel_ids_to_delete);

		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_delete_channel_request channel ids to delete count ", channels_to_delete_count, "\n");

		//
		//at this point the channels are marked, loop throuh marked channel ids, but do not delete them yet, just find out if there are clients there
		//

		for (i = 0; i < channels_to_delete_count; i++)
		{
			channel_id_to_delete = channel_ids_to_delete[i];
			client_count_in_channel = base__get_client_count_for_channel(channel_id_to_delete);

			DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "client_msg__process_delete_channel_request client count in channel ", channel_id_to_delete, " is ", client_count_in_channel, "\n");

			for (ii = 0; ii < MAX_CLIENTS; ii++)
			{
				client_to_move_maybe = &clients_array[ii];

				if (!client_to_move_maybe->is_existing)
				{
					continue;
				}
				if (!client_to_move_maybe->is_authenticated)
				{
					continue;
				}
				if (client_to_move_maybe->channel_id != channel_id_to_delete)
				{
					continue;
				}

				//
				//client found
				//

				DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_delete_channel_request moving client ", client_to_move_maybe->client_id, "to root channel \n");

				client_to_move_maybe->channel_id = ROOT_CHANNEL_ID;

				server_msg__send_channel_join_message_to_all_clients(client_to_move_maybe, &channel_array[ROOT_CHANNEL_ID]);

				if (channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present)
				{
					DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_delete_channel_request also sending maintainer id of root channel to moving client ", channel_array[ROOT_CHANNEL_ID].maintainer_id, "\n");
					server_msg__send_maintainer_id_to_single_client(client_to_move_maybe, ROOT_CHANNEL_ID, channel_array[ROOT_CHANNEL_ID].maintainer_id);
				}

				server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move_maybe->p_ws_connection, client_to_move_maybe->dh_shared_secret, ROOT_CHANNEL_ID);
			}
		}

		//clients are moved, now delete channels

		for (i = 0; i < channels_to_delete_count; i++)
		{
			channel_id_to_delete = channel_ids_to_delete[i];

			clib__null_memory(&channel_array[channel_id_to_delete], sizeof(channel_t));
			server_msg__send_channel_delete_message_to_all_clients(channel_id_to_delete, sender_client_index);
		}

		//if there is no maintainer in root channel, find new maintainer now
		if (!channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present)
		{
			status = base__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE);
			if (status)
			{
				channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present = TRUE;
				channel_array[ROOT_CHANNEL_ID].maintainer_id = index_of_new_maintainer;
				server_msg__send_maintainer_id_to_clients_in_same_channel(ROOT_CHANNEL_ID, channel_array[ROOT_CHANNEL_ID].maintainer_id);
			}
		}
	}
	else
	{
		if (clients_array[sender_client_index].is_authenticated && clients_array[sender_client_index].is_existing)
		{
			server_msg__send_access_denied_to_single_client(&clients_array[sender_client_index]);
		}
	}

label_client_msg__process_delete_channel_request_end:

	memorymanager__free((nuint)channel_ids_to_delete);

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
	pthread_rwlock_unlock(&channels_global_rwlock_guard);
}

/**
 * @brief This function processes delete channel request
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_poke_client_request(cJSON *json_root, int sender_client_index)
{
	boole status;
	cJSON *json_message_object;
	cJSON *json_receiver_id;
	cJSON *json_poke_message;

	status = base__is_request_allowed_based_on_spam_protection(sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_poke_client_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
		return;
	}

	status = client_msg__is_json_poke_client_request_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_poke_client_request client_msg__is_json_poke_client_request_format_valid == FALSE \n");
		return;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_poke_message = cJSON_GetObjectItemCaseSensitive(json_message_object, "poke_message");
	json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);

	if (clients_array[json_receiver_id->valueint].is_authenticated)
	{
		server_msg__send_poke_to_single_client(&clients_array[json_receiver_id->valueint], sender_client_index, json_poke_message->valuestring);
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief This function processes sdp answer for offer that server sent to client as part of process of establishing webrtc datachannel connection
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_sdp_answer(cJSON *json_root, int sender_client_index)
{
	boole status;
	cJSON *json_message_object;
	cJSON *json_message_value;

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_sdp_answer");

	status = client_msg__is_json_sdp_answer_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_sdp_answer client_msg__is_json_sdp_answer_format_valid == FALSE \n");
		return;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_message_value = cJSON_GetObjectItem(json_message_object, "value");

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);

	if (clients_array[sender_client_index].is_authenticated)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_sdp_answer got here");

		audio_channel__process_sdp_answer_from_remote_peer(&clients_array[sender_client_index], json_message_value);
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief This function processes ice candidate. When I tried this on local network, this was not needed. Still, good to send and process
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_ice_candidate(cJSON *json_root, int sender_client_index)
{
	boole status;
	cJSON *json_message_object;
	cJSON *json_message_value;

	DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_ice_candidate");

	status = client_msg__is_ice_candidate_format_valid(json_root, sender_client_index);
	if (!status)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_ice_candidate client_msg__is_ice_candidate_format_valid == FALSE \n");
		return;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

	json_message_value = cJSON_GetObjectItem(json_message_object, "value");

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);

	if (clients_array[sender_client_index].is_authenticated)
	{
		DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_ice_candidate got here");

		audio_channel__process_ice_candidate_from_remote_peer(&clients_array[sender_client_index], json_message_value);
	}

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_microphone_usage(cJSON *json_root, int sender_client_index)
{
	cJSON *json_value = 0;
	cJSON *json_message_object = 0;
	boole status;
	int received_microphone_usage;
	client_t *client;

	status = client_msg__is_json_process_microphone_usage_valid(json_root);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_microphone_usage client_msg__process_microphone_usage == FALSE \n");
		return;
	}

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE)
	{
		goto label_client_msg__process_microphone_usage_end;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

	received_microphone_usage = json_value->valueint;

	if (received_microphone_usage == MICROPHONE_USAGE__KEEP_PUSH_TO_TALK_READY_BUT_DONT_SEND_AUDIO)
	{
		if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_ENABLED)
		{
			client->audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
			audio_channel__set_is_client_sending_audio(client->client_id, FALSE);
			server_msg__send_audio_state_of_client_to_all_clients(sender_client_index, client->audio_state);
		}
	}
	else if (received_microphone_usage == MICROPHONE_USAGE__DISABLE_PUSH_TO_TALk)
	{
		if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS)
		{
			client->audio_state = AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;

			if (client->is_streaming_song == TRUE)
			{
				client->is_streaming_song = FALSE;
				server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client);
			}

			audio_channel__set_is_client_sending_audio(client->client_id, FALSE);
			server_msg__send_audio_state_of_client_to_all_clients(sender_client_index, client->audio_state);
		}
	}
	else if (received_microphone_usage == MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
	{
		if (client->audio_state != MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
		{
			client->audio_state = MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO;
			audio_channel__set_is_client_sending_audio(client->client_id, TRUE);
			server_msg__send_audio_state_of_client_to_all_clients(sender_client_index, client->audio_state);
		}
	}

label_client_msg__process_microphone_usage_end:
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
	return;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_start_song_stream_message(cJSON *json_root, int sender_client_index)
{
	boole status;
	client_t *client;
	cJSON *json_song_name = 0;
	cJSON *json_message_object = 0;

	status = client_msg__is_json_start_song_stream_message_valid(json_root);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_start_song_stream_message client_msg__is_json_start_song_stream_message_valid == FALSE \n");
		return;
	}

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE)
	{
		goto label_client_msg__process_start_song_stream_end;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_song_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_name");

	client->is_streaming_song = true;
	clib__copy_memory(json_song_name->valuestring, client->song_name, clib__utf8_string_length(json_song_name->valuestring), SONG_NAME_MAX_LENGTH);

	server_msg__send_start_song_stream_message_to_clients_in_same_channel(client);

label_client_msg__process_start_song_stream_end:

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
	return;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_stop_song_stream_message(cJSON *json_root, int sender_client_index)
{
	boole status;
	client_t *client;
	cJSON *json_song_name = 0;
	cJSON *json_message_object = 0;

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE)
	{
		goto label_client_msg__process_start_song_stream_end;
	}

	server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client);

label_client_msg__process_start_song_stream_end:

	pthread_rwlock_unlock(&clients_global_rwlock_guard);
	return;
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_admin_password_message(cJSON *json_root, int sender_client_index)
{
	boole status;
	client_t *client;
	cJSON *admin_password = 0;
	cJSON *json_message_object = 0;
	uint64 cvector_loop_index;

	status = _client_msg_internal__is_admin_password_message_valid(json_root);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_start_song_stream_message client_msg__is_json_start_song_stream_message_valid == FALSE \n");
		return;
	}

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE || client->is_existing == FALSE)
	{
		goto label_client_msg__process_admin_password_message_end;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	admin_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

	status = clib__is_string_equal(admin_password->valuestring, g_server_settings.admin_password);

	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client : ", sender_client_index, "admin password is not correct \n");
		goto label_client_msg__process_admin_password_message_end;
	}

	if (client->is_admin == FALSE)
	{
		client->is_admin = TRUE;

		if (g_server_settings.is_display_admin_tag_active)
		{
			//
			//only add admin tag to client if he doesnt have it
			//

			boole is_tag_found = FALSE;
			if (client->tag_ids != NULL_POINTER)
			{
				for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->tag_ids); ++cvector_loop_index)
				{
					if (client->tag_ids[cvector_loop_index] == ADMIN_TAG_ID)
					{
						is_tag_found = TRUE;
						break;
					}
				}
			}

			if (is_tag_found == FALSE)
			{
				cvector_push_back(client->tag_ids, ADMIN_TAG_ID);
			}

			server_msg__send_add_tag_to_client_event_to_all_clients(client->client_id, ADMIN_TAG_ID);
		}
	}

	//
	//send admin status to other clients possibly, or not, do they have to know you are an admin
	//

label_client_msg__process_admin_password_message_end:
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_add_tag_to_client_message(cJSON *json_root, int sender_client_index)
{
	boole status;
	boole does_sender_have_permissions_to_add_tag;
	client_t *client;
	client_t *client_to_add_tag_to;
	cJSON *json_message_object;
	cJSON *json_client_id_to_add_tag_to;
	cJSON *json_tag_id;

	uint64 cvector_loop_index;

	does_sender_have_permissions_to_add_tag = FALSE;
	json_message_object = NULL_POINTER;

	status = _client_msg_internal__is_add_tag_to_client_valid(json_root);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_start_song_stream_message client_msg__is_json_start_song_stream_message_valid == FALSE \n");
		return;
	}

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	//
	//check if client that sent the message is valid. If he is connected and he exists.
	//this was checked before but not within write lock like here
	//

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE || client->is_existing == FALSE)
	{
		goto label_client_msg__process_add_tag_to_client_message_end;
	}

	//
	//check if client that is sending request has permission to add tags
	//

	does_sender_have_permissions_to_add_tag = client->is_admin;

	if (does_sender_have_permissions_to_add_tag == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "sender with sender_client_index", sender_client_index, "does not have permission to add tag \n");
		goto label_client_msg__process_add_tag_to_client_message_end;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_client_id_to_add_tag_to = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
	json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

	if (g_server_settings.is_display_admin_tag_active == FALSE && json_tag_id->valueint == ADMIN_TAG_ID)
	{
		goto label_client_msg__process_add_tag_to_client_message_end;
	}

	//
	//check if client whose tag is about to be added exists
	//

	client_to_add_tag_to = &clients_array[json_client_id_to_add_tag_to->valueint];

	if (client_to_add_tag_to->is_authenticated == FALSE || client_to_add_tag_to->is_existing == FALSE)
	{
		goto label_client_msg__process_add_tag_to_client_message_end;
	}

	//
	//check if the tag itself that is about to be added exists
	//

	status = base__is_tag_id_real(json_tag_id->valueint);

	if (status == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "tag id : ", json_tag_id->valueint, "does not exist \n");
		goto label_client_msg__process_add_tag_to_client_message_end;
	}

	//
	//now check if the client already has the tag id about to be added
	//

	status = base__is_client_already_assigned_this_tag_id(json_client_id_to_add_tag_to->valueint, json_tag_id->valueint);
	if (status == TRUE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "tag id : ", json_tag_id->valueint, " already is assigned to client ", json_tag_id->valueint, "\n");
		goto label_client_msg__process_add_tag_to_client_message_end;
	}

	//
	//at this point following is clear
	//- client has a permission to add this tag id
	//- receiving client exists and doesnt have that tag id yet
	//- tag id is valid (exists)
	//- all that is left is to add that tag id to client, and make him admin if tag id happens to be admin
	//

	cvector_push_back(client_to_add_tag_to->tag_ids, json_tag_id->valueint);

	if (json_tag_id->valueint == ADMIN_TAG_ID)
	{
		client_to_add_tag_to->is_admin = TRUE;
	}

	server_msg__send_add_tag_to_client_event_to_all_clients(client_to_add_tag_to->client_id, json_tag_id->valueint);

	//
	//send admin status to other clients possibly, or not, do they have to know you are an admin
	//

label_client_msg__process_add_tag_to_client_message_end:
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_remove_tag_from_client_message(cJSON *json_root, int sender_client_index)
{
	boole status;
	boole does_sender_have_permissions_to_remove_tag;
	client_t *client;
	client_t *client_to_remove_tag_id_from;
	cJSON *json_message_object;
	cJSON *json_client_id_to_remove_tag_id_from;
	cJSON *json_tag_id;
	int tag_id_index;

	uint64 cvector_loop_index;

	does_sender_have_permissions_to_remove_tag = FALSE;
	json_message_object = NULL_POINTER;

	status = _client_msg_internal__is_remove_tag_from_client_valid(json_root);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_start_song_stream_message client_msg__is_json_start_song_stream_message_valid == FALSE \n");
		return;
	}

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	//
	//check if client that sent the message is valid. If he is connected and he exists.
	//this was checked before but not within write lock like here
	//

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE || client->is_existing == FALSE)
	{
		goto label_client_msg__process_remove_tag_from_client_message_end;
	}

	//
	//check if client that is sending request has permission to add tags
	//

	does_sender_have_permissions_to_remove_tag = client->is_admin;

	if (does_sender_have_permissions_to_remove_tag == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "sender with sender_client_index", sender_client_index, "does not have permission to add tag \n");
		goto label_client_msg__process_remove_tag_from_client_message_end;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_client_id_to_remove_tag_id_from = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
	json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

	//
	//check if tag happens to be admin tag
	//

	if (g_server_settings.is_display_admin_tag_active == FALSE && json_tag_id->valueint == ADMIN_TAG_ID)
	{
		goto label_client_msg__process_remove_tag_from_client_message_end;
	}

	//
	//check if client whose tag is about to be added exists
	//

	client_to_remove_tag_id_from = &clients_array[json_client_id_to_remove_tag_id_from->valueint];

	if (client_to_remove_tag_id_from->is_authenticated == FALSE || client_to_remove_tag_id_from->is_existing == FALSE)
	{
		goto label_client_msg__process_remove_tag_from_client_message_end;
	}

	//
	//check if the tag itself that is about to be added exists
	//

	status = base__is_tag_id_real(json_tag_id->valueint);

	if (status == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "tag id : ", json_tag_id->valueint, "does not exist \n");
		goto label_client_msg__process_remove_tag_from_client_message_end;
	}

	//
	//now check if the client already has the tag id about to be added
	//

	status = base__is_client_already_assigned_this_tag_id(json_client_id_to_remove_tag_id_from->valueint, json_tag_id->valueint);
	if (status == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "tag id : ", json_tag_id->valueint, " already is not assigned to client ", json_tag_id->valueint, "\n");
		goto label_client_msg__process_remove_tag_from_client_message_end;
	}

	//
	//at this point following is clear
	//- client has a permission to remove this tag id
	//- receiving client exists and has that tag id
	//- tag id is valid (exists)
	//- all that is left is to add that tag id to client, and make him admin if tag id happens to be admin
	//

	tag_id_index = base__get_index_of_tag_id_of_client(json_client_id_to_remove_tag_id_from->valueint, json_tag_id->valueint);

	if (tag_id_index != -1)
	{
		cvector_erase(client_to_remove_tag_id_from->tag_ids, tag_id_index);

		if (json_tag_id->valueint == ADMIN_TAG_ID)
		{
			client_to_remove_tag_id_from->is_admin = FALSE;
		}

		server_msg__send_remove_tag_from_client_event_to_all_clients(client_to_remove_tag_id_from->client_id, json_tag_id->valueint);
	}

label_client_msg__process_remove_tag_from_client_message_end:
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_set_server_settings_icon_upload(cJSON *json_root, int sender_client_index)
{
	boole status;
	boole does_sender_have_permissions_to_upload_icon;
	client_t *client;
	uint64 icon_index;
	icon_t *found_icon;
	cJSON *base64_icon_value;
	cJSON *json_message_object;

	status = FALSE;
	found_icon = NULL_POINTER;

	status = _client_msg_internal__is_process_server_settings_icon_upload_message_valid(json_root);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_icon_upload_message_valid is not valid");
		return;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	base64_icon_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_icon_value");

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	//
	//check if client that sent the message is valid. If he is connected and he exists.
	//this was checked before but not within write lock like here
	//

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE || client->is_existing == FALSE)
	{
		goto label_client_msg__process_server_settings_icon_upload_end;
	}

	//
	//check if client that is sending request has permission to add tags
	//

	does_sender_have_permissions_to_upload_icon = client->is_admin;

	if (does_sender_have_permissions_to_upload_icon == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "sender with sender_client_index", sender_client_index, "does not have permission to add tag \n");
		goto label_client_msg__process_server_settings_icon_upload_end;
	}

	//
	//sender has nessecary permission to upload new icon.
	//find out if there is free slot for icon in the array
	//

	for (icon_index = 0; icon_index < MAX_ICONS; icon_index++)
	{
		icon_t *icon = &icons_array[icon_index];
		if (icon->is_existing == FALSE)
		{
			found_icon = icon;
			found_icon->is_existing = TRUE;
			clib__copy_memory((void *)base64_icon_value->valuestring, (void *)&found_icon->base64[0], clib__utf8_string_length(base64_icon_value->valuestring), ICON_MAX_LENGTH);
			found_icon->id = icon_index;

			break;
		}
	}

	if (found_icon == NULL_POINTER)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "found icon is null");
		goto label_client_msg__process_server_settings_icon_upload_end;
	}

	//
	//everything is checked
	//write new icon and notify users
	//

	server_msg__send_add_new_icon_event_to_all_clients(found_icon->id, &found_icon->base64[0]);

label_client_msg__process_server_settings_icon_upload_end:
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param cJSON* json_root
 * @param int sender_client_index
 * *
 * @return void
 */
void client_msg__process_set_server_settings_add_new_tag(cJSON *json_root, int sender_client_index)
{
	boole status;
	boole does_sender_have_permissions_to_add_new_tag_to_server;
	client_t *client;
	cJSON *json_linked_icon_id;
	cJSON *json_tag_name;
	cJSON *json_message_object;
	tag_t *newly_added_tag;
	tag_t *tag_in_loop;
	uint64 tag_index;
	boole is_same_tag_name_found;

	status = FALSE;
	does_sender_have_permissions_to_add_new_tag_to_server = FALSE;
	client = NULL_POINTER;
	json_linked_icon_id = NULL_POINTER;
	json_tag_name = NULL_POINTER;
	json_message_object = NULL_POINTER;
	newly_added_tag = NULL_POINTER;
	tag_index = 0;
	is_same_tag_name_found = FALSE;
	tag_in_loop = NULL_POINTER;

	status = _client_msg_internal__is_process_server_settings_add_new_tag_message_valid(json_root);
	if (!status)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_server_settings_add_new_tag is not valid");
		return;
	}

	json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
	json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
	json_tag_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_name");

	pthread_rwlock_wrlock(&clients_global_rwlock_guard);

	//
	//one more check for linked_icon_id , check if it exists
	//range check for json_linked_icon_id->valueint is done in _client_msg_internal__is_process_server_settings_add_new_tag_message_valid
	//icon id is same as icon index
	//

	status = icons_array[json_linked_icon_id->valueint].is_existing;

	if (status == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_set_server_settings_add_new_tag sender with sender_client_index", sender_client_index, "does not have permission to add newly_added_tag \n");
		goto label_client_msg__process_server_settings_add_new_tag_end;
	}

	//
	//check if client that sent the message is valid. If he is connected and he exists.
	//this was checked before but not within write lock like here
	//

	client = &clients_array[sender_client_index];

	if (client->is_authenticated == FALSE || client->is_existing == FALSE)
	{
		goto label_client_msg__process_server_settings_add_new_tag_end;
	}

	//
	//check if client that is sending request has permission to create new server tags
	//

	does_sender_have_permissions_to_add_new_tag_to_server = client->is_admin;

	if (does_sender_have_permissions_to_add_new_tag_to_server == FALSE)
	{
		DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_set_server_settings_add_new_tag sender with sender_client_index", sender_client_index, "does not have permission to add newly_added_tag \n");
		goto label_client_msg__process_server_settings_add_new_tag_end;
	}

	//do not allow creation of new tag if tag with same name already exists

	for (tag_index = 0; tag_index < MAX_TAGS; tag_index++)
	{
		tag_in_loop = &tags_array[tag_index];
		if (tag_in_loop->is_existing == TRUE)
		{
			status = clib__is_string_equal(json_tag_name->valuestring, tag_in_loop->name);

			if (status == TRUE)
			{
				is_same_tag_name_found = TRUE;
				break;
			}
		}
	}

	if (is_same_tag_name_found)
	{
		DBG_CLIENT_MESSAGE log_info("%s %s %s", " is_same_tag_name_found == TRUE", json_tag_name->valuestring, "\n");
		goto label_client_msg__process_server_settings_add_new_tag_end;
	}

	//
	//at this point client is verified, icon id is correct, newly_added_tag length is correct
	//

	for (tag_index = 0; tag_index < MAX_TAGS; tag_index++)
	{
		tag_in_loop = &tags_array[tag_index];

		if (tag_in_loop->is_existing == FALSE)
		{
			tag_in_loop->is_existing = TRUE;
			tag_in_loop->icon_id = json_linked_icon_id->valueint;
			clib__copy_memory((void *)&json_tag_name->valuestring[0], (void *)&tag_in_loop->name[0], clib__utf8_string_length(json_tag_name->valuestring), TAG_MAX_NAME_LENGTH);
			tag_in_loop->id = tag_index;

			newly_added_tag = tag_in_loop;
			break;
		}
	}

	if (newly_added_tag == NULL_POINTER)
	{
		DBG_CLIENT_MESSAGE log_info("%s", "newly_added_tag == NULL_POINTER");
		goto label_client_msg__process_server_settings_add_new_tag_end;
	}

	server_msg__send_create_new_tag_event_to_all_clients(tag_in_loop->id, tag_in_loop->name, tag_in_loop->icon_id);

label_client_msg__process_server_settings_add_new_tag_end:
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
}