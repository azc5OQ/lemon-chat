#ifndef EMBEDDED_WIN32_INPUT_H
#define EMBEDDED_WIN32_INPUT_H

// embedded_win32_input.h - Win32 message translation for embedded hosts.
// An overlay sees the host's window messages (through its own window
// procedure or a hook) and forwards them here; meek-ui gets mouse,
// wheel, character and key events, and the return value tells the host
// whether the UI consumed the message, so a game can decide when the
// overlay swallows input.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "types.h"
#include "meek_ui_config.h"

// Forward capture-change, cancel-mode and focus-loss notifications too: they
// cancel an interrupted press. They return FALSE so the host also processes them.
// Left press/release (including the second double-click press) stays with its
// initial UI/host owner. FALSE means forward to the original window procedure;
// do not reclassify a host-owned release by testing its current cursor position.
GUI_API boole embedded_win32__on_window_event(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
GUI_API boole embedded_win32__pointer_over_ui(int64 x, int64 y);

#endif
