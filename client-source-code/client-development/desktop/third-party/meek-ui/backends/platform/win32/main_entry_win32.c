// platforms/windows/main_entry_win32.c - Windows real-main stub.
// This translation unit is compiled into the HOST EXECUTABLE, NOT
// into gui.dll. Reason: `app_main` (the host's renamed `main`) is
// defined in the host's own main.c, which the linker knows about
// only when building the exe -- if the entry stub lived inside the
// DLL, `app_main` would be an undefined external at DLL-link time
// and the build would fail.
// Shape:
// - gui/CMakeLists.txt does NOT add this file to the gui target.
// - demo-windows/CMakeLists.txt (and any other Windows host) DOES
// add it to the executable target.
// - The file has no #include "platform.h" because that would
// rename `main` to `app_main` and defeat the point. The only
// external symbol is `app_main` itself, forward-declared below.
// SDL does roughly the same thing with SDL_main / SDL2main.lib --
// the entry stub is a tiny per-host compilation unit that forwards
// to the user's renamed main.

#include "_main_trampoline.h"
GUI_DEFINE_MAIN_TRAMPOLINE()
