#ifndef _MAIN_TRAMPOLINE_H
#define _MAIN_TRAMPOLINE_H

// _main_trampoline.h - shared int main() that forwards to app_main.
// platform.h does `#define main app_main` so the host's
// `int main(int argc, char** argv)` is renamed at compile time;
// every platform backend then needs to provide a real `main` that
// forwards to that renamed entry. This header packs that 4-line
// stub into a macro so each platform TU emits it identically:
// #define _PLATFORM_INTERNAL
// #include "platform.h"             // suppresses the rename here
// #undef _PLATFORM_INTERNAL
// ...
// #include "_main_trampoline.h"
// GUI_DEFINE_MAIN_TRAMPOLINE()
// Used by platforms/windows/main_entry_win32.c, platform_linux_drm.c,
// platform_linux_x11.c, and platform_macos.m.
// On Android there's no `int main` at all -- the entry is
// `void android_main(struct android_app*)` which is implemented
// directly in platform_android.c with its own forwarding logic, so
// that file does NOT include this header.

#include "renderer.h"

#if defined(MEEK_UI_HOST_OWNS_MAIN)
#define GUI_DEFINE_MAIN_TRAMPOLINE()
#else
#define GUI_DEFINE_MAIN_TRAMPOLINE()           \
    extern int app_main(int argc, char** argv);\
    int main(int argc, char** argv)            \
    {                                          \
        renderer__parse_launch_flags(&argc, argv); \
        return app_main(argc, argv);           \
    }

#endif

#endif
