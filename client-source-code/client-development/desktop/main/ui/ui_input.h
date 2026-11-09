#ifndef LEMON_DESKTOP_UI_INPUT_H
#define LEMON_DESKTOP_UI_INPUT_H

#include "clib.h"

void desktop_ui_input__register(void (*submit)(void));
boole desktop_ui_input__limits(void);

#endif
