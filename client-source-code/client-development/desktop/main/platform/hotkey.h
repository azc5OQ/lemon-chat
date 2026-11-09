#ifndef LEMON_DESKTOP_HOTKEY_H
#define LEMON_DESKTOP_HOTKEY_H

#include "clib.h"

#define DESKTOP_HOTKEY_DEFAULT 'Q'

typedef struct desktop_hotkey_t desktop_hotkey_t;

int64 desktop_hotkey__count(void);
int64 desktop_hotkey__key(int64 index);
int64 desktop_hotkey__index(int64 key);
const char *desktop_hotkey__name(int64 key);
boole desktop_hotkey__initialize(void);
desktop_hotkey_t *desktop_hotkey__create(void);
void desktop_hotkey__destroy(desktop_hotkey_t *hotkey);
boole desktop_hotkey__down(desktop_hotkey_t *hotkey, int64 key);

#endif
