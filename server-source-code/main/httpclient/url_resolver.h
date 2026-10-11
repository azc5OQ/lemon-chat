#ifndef URL_RESOLVER_H
#define URL_RESOLVER_H

void url_resolver__request(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket);
#endif
