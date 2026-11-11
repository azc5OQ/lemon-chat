#ifndef LEMON_DESKTOP_CLIENT_MSG_H
#define LEMON_DESKTOP_CLIENT_MSG_H

#include "clib.h"
#include "session/session.h"
#include "cJSON.h"

cJSON *client_msg__delivery__new_message(const char *type);
boole client_msg__delivery__send(desktop_session_t *session, cJSON *message, boole complete);
boole client_msg__delivery__try_send(desktop_session_t *session, cJSON *message);
boole client_msg__channel__distribute_keys(desktop_session_t *session);
void client_msg__channel__refresh_keys(desktop_session_t *session);
void client_msg__channel__member_changed(desktop_session_t *session, const desktop_client_t *previous, const desktop_client_t *current);
boole client_msg__channel__send_join_request(desktop_session_t *session, int64 channel, const char *password);
boole client_msg__channel__send_reset_maintainer(desktop_session_t *session);
boole client_msg__connection__send_public_key(desktop_session_t *session, const char *public_mix);
boole client_msg__connection__send_challenge_response(desktop_session_t *session, const char *challenge);
boole client_msg__connection__send_keepalive(desktop_session_t *session);
void client_msg__chat__send_channel_message(desktop_session_t *session, int64 channel, const char *text, const desktop_chat_format_t *format);
void client_msg__chat__send_direct_message(desktop_session_t *session, int64 peer, uint64 serial, const char *text, const desktop_chat_format_t *format);
void client_msg__chat__send_direct_closed_notice(desktop_session_t *session, int64 peer, uint64 serial);
void client_msg__chat__send_edit(desktop_session_t *session, uint64 sequence, uint64 connection_serial, const char *text);
void client_msg__dispatch_command(desktop_session_t *session);
void client_msg__actions__send(desktop_session_t *session, const desktop_action_t *action);
void client_msg__channel__send_change(desktop_session_t *session, const desktop_channel_request_t *request);

#endif
