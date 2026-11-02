#ifndef LEMON_DESKTOP_UI_CONNECTION_H
#define LEMON_DESKTOP_UI_CONNECTION_H

#include "clib.h"

void desktop_ui_connection__initialize(void);
boole desktop_ui_connection__refresh(void);
boole desktop_ui_connection__address(char* output, uint64 capacity);
boole desktop_ui_connection__load_address(const char* address);
boole desktop_ui_connection__keys(char* output, uint64 capacity);
boole desktop_ui_connection__load_keys(const char* keys);
void desktop_ui_connection__clear_keys(void);
void desktop_ui_connection__shutdown(void);

#endif
