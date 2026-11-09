#ifndef LEMON_DESKTOP_UI_FONTS_H
#define LEMON_DESKTOP_UI_FONTS_H

#include "clib.h"

boole desktop_ui_fonts__scan(boole force);
int64 desktop_ui_fonts__count(void);
const char *desktop_ui_fonts__name(int64 index);
const char *desktop_ui_fonts__token(int64 index);
const char *desktop_ui_fonts__family(const char *token);

#endif
