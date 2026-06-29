#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

void   http_server__start(int64 port, char* webroot);
void   http_server__set_client_config(char* config_script);

#endif
