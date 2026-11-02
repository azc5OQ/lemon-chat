#ifndef HOT_RELOAD_H
#define HOT_RELOAD_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// hot_reload.h - poll-based hot reload for .ui and .style files.
// paired with hot_reload.c.
// mechanism: each watched file has its content hashed (fnv-1a 32-bit
// over the bytes) on every tick. when the hash changes, the file is
// re-read and re-applied:
// .ui files    -> parser_xml__load_ui produces a new tree;
// the previous tree is freed and the new one
// becomes the scene root.
// .style files -> scene__clear_styles wipes all registered styles
// and parser_style__load_styles re-registers from
// the file.
// file-open / parse failures are silent so editors that briefly hold
// the file open for write don't spam the log.
// the polling implementation is the fallback. ReadDirectoryChangesW +
// IOCP would replace it without changing this header.
// multiple watches are supported (one .ui + one .style is the typical
// host-app case, but the table holds up to 8 watches total).

GUI_API boole hot_reload__watch_ui(char* path);

GUI_API boole hot_reload__watch_style(char* path);

GUI_API int   hot_reload__tick(void);

GUI_API void  hot_reload__shutdown(void);

#endif
