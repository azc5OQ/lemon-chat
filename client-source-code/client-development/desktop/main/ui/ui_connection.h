#ifndef LEMON_DESKTOP_UI_CONNECTION_H
#define LEMON_DESKTOP_UI_CONNECTION_H

#include "clib.h"
#include "settings/settings.h"

void desktop_ui_connection__initialize(desktop_settings_t *settings);
void desktop_ui_connection__apply_identity(void);
void desktop_ui_connection__state(boole connected, boole busy);
boole desktop_ui_connection__refresh(void);
boole desktop_ui_connection__address(char *output, uint64 capacity);
boole desktop_ui_connection__load_address(const char *address);
boole desktop_ui_connection__keys(char *output, uint64 capacity);
boole desktop_ui_connection__load_keys(const char *keys);
boole desktop_ui_connection__identity_valid(const char *identity);
boole desktop_ui_connection__identity_decode(const ubyte *bytes, uint64 size, char *identity, uint64 capacity);
void desktop_ui_connection__clear_keys(void);
void desktop_ui_connection__shutdown(void);

#endif
