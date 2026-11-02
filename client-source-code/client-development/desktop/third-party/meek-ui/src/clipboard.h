#ifndef MEEK_UI_CLIPBOARD_H
#define MEEK_UI_CLIPBOARD_H

#include "types.h"
#include "gui_api.h"

// UTF-8 text. Readers return a NUL-terminated prefix fitting capacity bytes,
// never a partial codepoint. Writers copy length bytes before returning.
typedef boole (*gui_clipboard_read_fn)(char* output, int64 capacity);
typedef boole (*gui_clipboard_write_fn)(const char* text, int64 length);

// Optional host overrides. NULL callbacks use the native Windows clipboard;
// other platforms return FALSE until their host supplies the corresponding hook.
GUI_API void clipboard__set_callbacks(gui_clipboard_read_fn read, gui_clipboard_write_fn write);
GUI_API boole clipboard__read(char* output, int64 capacity);
GUI_API boole clipboard__write(const char* text, int64 length);

#endif
