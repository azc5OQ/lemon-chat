#ifndef LEMON_DESKTOP_UI_THEME_H
#define LEMON_DESKTOP_UI_THEME_H

#include "../definitions.h"

#include "settings/settings.h"

void desktop_ui_theme__fonts(void);
boole desktop_ui_theme__load(const char *name);
boole desktop_ui_theme__save(const desktop_settings_t *settings);
void desktop_ui_theme__apply(void);
boole desktop_ui_theme__scale(int64 percent);
void desktop_ui_theme__shutdown(void);

#endif
