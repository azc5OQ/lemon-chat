#ifndef LEMON_DESKTOP_SYSTEM_H
#define LEMON_DESKTOP_SYSTEM_H

#include "clib.h"

uint64 desktop_system__milliseconds(void);
void desktop_system__sleep(int64 milliseconds);
boole desktop_system__asset_directory(void);
void desktop_system__window_icon(void);
boole desktop_system__font_path(char *path, uint64 capacity, boole bold);

#endif
