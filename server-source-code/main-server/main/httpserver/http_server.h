#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "../definitions.h"

void http_server__initialize(int64 port, char *webroot);
boole http_server__set_mode(boole enabled, boole blank_when_disabled);
void http_server__start(int64 port, char *webroot);
void http_server__set_client_config(char *config_script);
void http_server__set_https_redirect(boole enabled, int64 https_port);

#endif
