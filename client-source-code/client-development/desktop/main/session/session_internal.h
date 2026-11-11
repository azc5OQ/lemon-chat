#ifndef LEMON_DESKTOP_SESSION_INTERNAL_H
#define LEMON_DESKTOP_SESSION_INTERNAL_H

#include "clib.h"
#include "session/session.h"
#include "crypto/crypto_session.h"
#include "network/transport.h"
#include "platform/thread.h"
#include "audio/voice.h"

typedef struct desktop_session_command_t
{
    desktop_action_t action;
    desktop_channel_request_t channel_request;
    desktop_attachment_request_t attachment_request;
    boole attach;
    boole join;
    boole direct;
    boole is_direct_closed;
    boole edit;
    uint64 chat_sequence;
    uint64 connection_serial;
    boole mute;
    boole muted;
    int64 peer;
    uint64 peer_serial;
    int64 channel;
    char text[DESKTOP_CHAT_CAPACITY];
    desktop_chat_format_t format;
} desktop_session_command_t;

typedef struct desktop_channel_key_recipient_t
{
    int64 id;
    uint64 serial;
} desktop_channel_key_recipient_t;

struct desktop_session_t
{
    desktop_mutex_t *mutex;
    desktop_thread_t *thread;
    desktop_transport_t *transport;
    desktop_crypto_t *crypto;
    desktop_voice_t *voice;
    desktop_attachments_t *attachments;
    ubyte tree_icons[DESKTOP_TREE_GROUPS][DESKTOP_TREE_ICON_BYTES];
    boole tree_icon_present[DESKTOP_TREE_GROUPS];
    uint64 next_tree_revision;
    uint64 tree_icons_revision;
    desktop_audio_options_t audio_options;
    int64 push_to_talk_key;
    desktop_connection_t connection;
    desktop_chat_format_t chat_format; // GUI preference, copied into commands under mutex.
    desktop_session_view_t model;
    desktop_session_view_t published;
    char public_key[1369];
    char client_keys[DESKTOP_SESSION_CLIENTS][1369];
    desktop_content_keys_t keys;
    desktop_content_keys_t previous_keys;
    boole have_previous;
    int64 maintainer;
    int64 generation;
    int64 key_channel;
    boole rotate;
    boole key_reuse_enabled;
    boole key_reuse_on_join;
    boole key_reuse_on_leave;
    int64 key_reuse_join_min_clients;
    int64 key_reuse_leave_min_clients;
    desktop_channel_key_recipient_t key_recipients[DESKTOP_SESSION_CLIENTS];
    int64 key_recipient_count;
    uint64 next_local_id;
    uint64 next_client_serial;
    uint64 next_channel_revision;
    uint64 next_chat_sequence;
    uint64 next_connection_serial;
    uint64 waiting_since;
    desktop_session_command_t commands[8];
    int64 command_count;
    boole stop;
    boole running;
    boole failed;
};

int64 desktop_session__client(desktop_session_t *session, int64 id);
int64 desktop_session__channel(desktop_session_t *session, int64 id);
void desktop_session__publish(desktop_session_t *session);
void desktop_session__detail(desktop_session_t *session, const char *text, boole fatal);
boole desktop_session__stopping(desktop_session_t *session);
void desktop_session__keys_reset(desktop_session_t *session);
boole desktop_chat__valid_id(const char *id);
boole desktop_chat__valid_text(const char *text);

#endif
