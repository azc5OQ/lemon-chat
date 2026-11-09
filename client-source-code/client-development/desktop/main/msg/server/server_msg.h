#ifndef LEMON_DESKTOP_SERVER_MSG_H
#define LEMON_DESKTOP_SERVER_MSG_H

#include "clib.h"
#include "session/session.h"
#include "cJSON.h"

const char *server_msg__read_string(const cJSON *object, const char *key);
int64 server_msg__read_number(const cJSON *object, const char *key);
boole server_msg__connection__read_client(desktop_session_t *session, const cJSON *data);
boole server_msg__connection__process_challenge(desktop_session_t *session, const cJSON *message);
boole server_msg__channel__read_channel(desktop_session_t *session, const cJSON *data);
void server_msg__channel__read_key_policy(desktop_session_t *session, const cJSON *message);
void server_msg__chat__append(desktop_session_t *session, int64 channel, const char *sender, const char *text, uint64 local_id);
void server_msg__chat__append_direct(desktop_session_t *session, int64 slot, const char *text, uint64 local_id);
void server_msg__chat__receive_text(desktop_session_t *session, int64 channel, int64 slot, const char *sender, const char *payload, int64 author_slot, int64 server_id);
void server_msg__chat__acknowledge(desktop_session_t *session, const cJSON *message);
void server_msg__chat__process_direct(desktop_session_t *session, const cJSON *message);
void server_msg__dispatch(desktop_session_t *session, const cJSON *message);
void server_msg__actions__receive(desktop_session_t *session, const cJSON *message);
void server_msg__tree__read_client(desktop_client_t *client, const cJSON *item);
void server_msg__tree__receive(desktop_session_t *session, const cJSON *message);

void server_msg__sounds__receive(desktop_session_t *session, const cJSON *message);

#endif
