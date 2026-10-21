#ifndef SERVER_LOGS_H
#define SERVER_LOGS_H

// the admin log

void server_logs__client_joined(client_t* client);
void server_logs__fast_reconnect(client_t* client);
void server_logs__client_disconnect_reason(client_t* client, char* reason);
void server_logs__username_changed(char* old_username, char* new_username);
void server_logs__tag_added(int tag_id, char* target_username, char* admin_username);
void server_logs__server_settings_updated(char* admin_username, boole save_succeeded);
void server_logs__client_kicked(char* target_username, char* admin_username);
void server_logs__client_banned(char* target_username, char* target_ip, char* admin_username);
void server_logs__client_disconnected(client_t* client);
void server_logs__join_refused(char* reason, char* ip_address);
void server_logs__join_refused_country(char* country_iso_code, char* ip_address);
void server_logs__socket_opened(char* ip_address);
void server_logs__socket_closed(char* ip_address, char* username);
void server_logs__admin_password_failed(char* username, char* ip_address, char* attempted_password);
void server_logs__cleared_by(char* admin_username);

cJSON* server_logs__build_json_array(void);
void server_logs__purge_tick(void);

#endif
