#ifndef LEMON_DESKTOP_SESSION_H
#define LEMON_DESKTOP_SESSION_H

#include "clib.h"
#include "overlay_data.h"
#include "audio/audio.h"
#include "attachments/attachments.h"
#include "session/server_info.h"
#include "session/chat_format.h"

#define DESKTOP_SESSION_CLIENTS 1024
#define DESKTOP_SESSION_HISTORY 128
#define DESKTOP_CHAT_CAPACITY 2048
#define DESKTOP_TREE_GROUPS 1000
#define DESKTOP_TREE_CHANNEL_GROUPS 128
#define DESKTOP_TREE_GROUP_WORDS 16
#define DESKTOP_TREE_ICON_BYTES (32 * 32 * 4)

typedef struct desktop_tree_group_t
{
    boole exists;
    int64 icon;
    char name[128];
    boole default_guest;
    boole can_assign;
    boole can_remove;
    int64 protection[2];
    boole negate_protection[2];
} desktop_tree_group_t;

typedef enum desktop_action_e
{
    DESKTOP_ACTION_NONE,
    DESKTOP_ACTION_RENAME,
    DESKTOP_ACTION_IDLE,
    DESKTOP_ACTION_RETURN,
    DESKTOP_ACTION_SERVER_INFO,
    DESKTOP_ACTION_POKE,
    DESKTOP_ACTION_ACCESS,
    DESKTOP_ACTION_MOVE,
    DESKTOP_ACTION_KICK_CHANNEL,
    DESKTOP_ACTION_MUTE_CHANNEL,
    DESKTOP_ACTION_STOP_STREAM,
    DESKTOP_ACTION_KICK_SERVER,
    DESKTOP_ACTION_BAN,
    DESKTOP_ACTION_GROUP_ADD,
    DESKTOP_ACTION_GROUP_REMOVE,
    DESKTOP_ACTION_CHANNEL_GROUP,
    DESKTOP_ACTION_IGNORE,
    DESKTOP_ACTION_VOLUME
} desktop_action_e;

typedef struct desktop_action_t
{
    desktop_action_e type;
    int64 peer;
    uint64 serial;
    int64 channel;
    int64 value;
    uint64 catalog_revision;
    char text[256];
} desktop_action_t;

enum
{
    DESKTOP_ACCESS_MOVE = 1,
    DESKTOP_ACCESS_KICK_CHANNEL = 2,
    DESKTOP_ACCESS_MUTE_CHANNEL = 4,
    DESKTOP_ACCESS_STOP_STREAM = 8
};

typedef struct desktop_connection_t
{
    char address[1024];
    char identity[513];
    char username[101];
    char server_keys[2049];
    int64 key_bits;
    boole automatic_transport;
} desktop_connection_t;

typedef struct desktop_client_t
{
    uint64 serial;
    int64 id;
    int64 channel;
    char name[101];
    int64 audio;
    boole idle;
    boole music;
    boole muted;
    boole ignored;
    int64 volume_offset;
    boole access_known;
    uint64 access;
    uint64 assignable_roles[2];
    char country[3];
    char song[256];
    int64 channel_group;
    uint64 groups[DESKTOP_TREE_GROUP_WORDS];
    uint64 direct_received;
} desktop_client_t;

typedef struct desktop_channel_t
{
    uint64 revision;
    int64 id;
    int64 parent;
    char name[128];
    char description[1000];
    boole password;
    boole audio;
    boole video;
    boole temporary;
    boole limited;
    int64 capacity;
    int64 powers[4];
    uint64 access;
    boole access_known;
    boole can_temporary;
    boole can_permanent;
    int64 icon;
} desktop_channel_t;

enum
{
    DESKTOP_CHANNEL_NAME = 1,
    DESKTOP_CHANNEL_DESCRIPTION = 2,
    DESKTOP_CHANNEL_PASSWORD = 4,
    DESKTOP_CHANNEL_POWERS = 8,
    DESKTOP_CHANNEL_OPTIONS = 16,
    DESKTOP_CHANNEL_DELETE = 32
};

typedef enum desktop_channel_action_e
{
    DESKTOP_CHANNEL_NONE,
    DESKTOP_CHANNEL_ACCESS,
    DESKTOP_CHANNEL_CREATE,
    DESKTOP_CHANNEL_EDIT,
    DESKTOP_CHANNEL_REMOVE
} desktop_channel_action_e;

typedef struct desktop_channel_request_t
{
    desktop_channel_action_e action;
    desktop_channel_t channel;
    boole replace_password;
    char password[121];
} desktop_channel_request_t;

typedef struct desktop_chat_t
{
    uint64 sequence;
    uint64 attachment_id;
    int64 channel;
    int64 peer;
    uint64 peer_serial;
    char peer_name[101];
    uint64 local_id;
    uint64 edit_local_id;
    uint64 connection_serial;
    uint64 author_serial;
    int64 server_id;
    int64 latest_edit_sequence;
    char reaction_id[33];
    char author_public_key[1369];
    char sender[101];
    char text[DESKTOP_CHAT_CAPACITY];
    desktop_chat_format_t format;
    boole pending;
    boole edited;
} desktop_chat_t;

typedef struct desktop_session_view_t
{
    uint64 revision;
    uint64 connection_serial;
    boole connected;
    boole ws_fallback;
    boole tls_certificate_rejected;
    boole keys_ready;
    boole audio_ready;
    boole microphone_ready;
    boole talking;
    boole voice_detected;
    int64 input_level;
    boole idle;
    boole administrator;
    boole permission_system;
    boole can_create_channel;
    boole can_create_temp_channel;
    boole policy_idle;
    boole policy_rename;
    boole policy_pictures;
    boole policy_files;
    boole policy_private;
    uint64 picture_limit;
    uint64 file_limit;
    boole policy_song_titles;
    boole can_assign_groups;
    boole can_remove_groups;
    boole can_kick;
    boole can_ban;
    int64 moderation_power[2];
    uint64 tree_revision;
    desktop_tree_group_t groups[DESKTOP_TREE_GROUPS];
    desktop_tree_group_t channel_groups[DESKTOP_TREE_CHANNEL_GROUPS];
    int64 server_capacity;
    desktop_server_info_t server_info;
    uint64 channel_result_sequence;
    int64 channel_result_id;
    desktop_channel_action_e channel_result_action;
    uint64 action_error_sequence;
    uint64 notice_sequence;
    char notice_sender[101];
    char notice_text[256];
    uint64 audio_packets_received;
    uint64 audio_packets_sent;
    char audio_detail[192];
    int64 local_id;
    int64 channel;
    char detail[256];
    int64 client_count;
    int64 channel_count;
    int64 chat_count;
    desktop_client_t clients[DESKTOP_SESSION_CLIENTS];
    desktop_chat_t chats[DESKTOP_SESSION_HISTORY];
    desktop_attachments_view_t attachments;
    // Owned channel storage. Copy views with desktop_session__view_copy, never by assignment.
    desktop_channel_t *channels;
    int64 channel_capacity;
} desktop_session_view_t;

typedef struct desktop_session_t desktop_session_t;

desktop_session_t *desktop_session__create(void);
boole desktop_session__allows(const desktop_session_view_t *view, const desktop_client_t *client, desktop_action_e action, int64 value);
boole desktop_session__tree_icon(desktop_session_t *session, int64 id, uint64 revision, ubyte *pixels, uint64 capacity);
desktop_attachments_t *desktop_session__attachments(desktop_session_t *session);
boole desktop_session__attach(desktop_session_t *session, const desktop_attachment_request_t *request);
void desktop_session__destroy(desktop_session_t *session);
boole desktop_session__connect(desktop_session_t *session, const desktop_connection_t *connection);
boole desktop_session__busy(desktop_session_t *session);
void desktop_session__disconnect(desktop_session_t *session);
boole desktop_session__view(desktop_session_t *session, desktop_session_view_t *view);
boole desktop_session__reserve_channels(desktop_session_view_t *view, int64 count);
boole desktop_session__view_copy(desktop_session_view_t *destination, const desktop_session_view_t *source);
boole desktop_session__view_equal(const desktop_session_view_t *left, const desktop_session_view_t *right);
void desktop_session__view_release(desktop_session_view_t *view);
boole desktop_session__join(desktop_session_t *session, int64 channel, const char *password);
boole desktop_session__chat(desktop_session_t *session, const char *text);
void desktop_session__set_chat_format(desktop_session_t *session, const desktop_chat_format_t *format);
boole desktop_session__direct(desktop_session_t *session, int64 peer, uint64 serial, const char *text);
boole desktop_session__direct_closed(desktop_session_t *session, int64 peer, uint64 serial);
boole desktop_session__can_edit(const desktop_session_view_t *view, const desktop_chat_t *chat);
boole desktop_session__edit(desktop_session_t *session, uint64 sequence, uint64 connection_serial, const char *text);
boole desktop_session__mute(desktop_session_t *session, int64 peer, uint64 serial, boole muted);
boole desktop_session__action(desktop_session_t *session, const desktop_action_t *action);
boole desktop_session__channel_request(desktop_session_t *session, const desktop_channel_request_t *request);
void desktop_session__audio(desktop_session_t *session, const desktop_audio_options_t *options, int64 key);
boole desktop_session__devices(desktop_session_t *session, desktop_devices_view_t *view, boole refresh);
void desktop_session__overlay(const desktop_session_view_t *view, desktop_overlay_snapshot_t *overlay);

void desktop_session__sound(desktop_session_t *session, desktop_sound_t id);
void desktop_session__notifications(desktop_session_t *session, boole enabled, int64 volume, const char *output);

#endif
