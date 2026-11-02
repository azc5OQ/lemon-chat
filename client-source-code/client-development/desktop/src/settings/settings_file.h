#ifndef LEMON_DESKTOP_SETTINGS_FILE_H
#define LEMON_DESKTOP_SETTINGS_FILE_H

#include "clib.h"

#define DESKTOP_SETTINGS_FILE_CAPACITY 65536

boole desktop_settings_file__read(char* bytes, uint64 capacity, uint64* length);
boole desktop_settings_file__write(const char* bytes, uint64 length);

#endif
