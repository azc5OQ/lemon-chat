#ifndef URL_RESOLVER_H
#define URL_RESOLVER_H

void url_resolver__queue_lookup(cstring url, cstring request_id, uint64 client_id, ws_cli_conn_t* socket);
#endif
