#ifndef MEEK_UI_CONFIG_H
#define MEEK_UI_CONFIG_H

// meek_ui_config.h - compile-time configuration of the library.
// Every knob is a preprocessor macro so a host that copies meek-ui
// into its own tree can either edit this file or pass -D flags.
// Nothing in here is required: the defaults build a plain Windows
// DLL / POSIX static library that owns its own window.

// MEEK_UI_STATIC
// Define when meek-ui is compiled into the host directly or as a
// static library. Turns GUI_API into nothing on Windows so no
// dllimport / dllexport thunks are generated. The Linux, macOS and
// Android builds are always "static" in this sense.

// GUI_BUILDING_DLL
// Defined by the library's own shared-library build (CMakeLists.txt
// does this). Turns GUI_API into __declspec(dllexport). Consumers
// of the DLL leave it undefined and get dllimport.

// MEEK_UI_HOST_OWNS_MAIN
// By default platform.h renames the host's `main` to `app_main` so
// the platform backend can provide the real entry point. A host that
// keeps its own main (an overlay injected into another process, a
// managed host, a plugin) defines this macro; it then never calls
// platform__init and drives scene__*, renderer__* and input itself.

// GUI_TRACK_ALLOCATIONS
// Route every allocation through memory_manager's tracked path so a
// clean shutdown prints leaks per allocation type. Costs a few ns per
// allocation; leave off for shipping builds.

// Each selector/state registration consumes one slot, including !important.
// Override when compiling the library; this does not change gui_node_t's ABI.
#ifndef MEEK_UI_MAX_STYLES
    #define MEEK_UI_MAX_STYLES 256
#endif
#if MEEK_UI_MAX_STYLES < 1
    #error MEEK_UI_MAX_STYLES must be positive
#endif

// GUI_FONTS_SOURCE_DIR
// Optional. A directory font__init scans for .ttf files at startup.
// Only meaningful when the host compiles font.c itself (single-binary
// builds). The recommended way is to call font__register_dir or
// font__register_from_file from the host after platform__init.

// MEEK_UI_OPENGL_SRGB_FRAMEBUFFER
// Opt-in: the opengl3 backend asks for an sRGB-capable pixel format and
// blends in linear light. Off by default since 2026-09-13: the other
// backends and browsers blend in sRGB space, and the visual sweep showed
// translucent colors and the backdrop blur lighter on opengl3 with it.

// Maximum text bytes per node, excluding NUL. Nodes allocate only as needed.
// Hosts may impose a smaller per-node limit with scene__set_text_limit.
#ifndef MEEK_UI_MAX_TEXT_BYTES
    #define MEEK_UI_MAX_TEXT_BYTES (64 * 1024)
#endif
#if MEEK_UI_MAX_TEXT_BYTES < 1 || MEEK_UI_MAX_TEXT_BYTES > 2147483646
    #error MEEK_UI_MAX_TEXT_BYTES must be between 1 and 2147483646
#endif

// MEEK_DBG_*
// Gated debug logging switches; see debug_definitions.h.

// CPU gradient samples are retained without reducing their floating-point precision.
// Set this byte budget to zero to disable the cache, or lower it for constrained hosts.
#ifndef MEEK_UI_CPU_BACKGROUND_CACHE_BYTES
    #define MEEK_UI_CPU_BACKGROUND_CACHE_BYTES (64 * 1024 * 1024)
#endif

// Finished CPU decoration surfaces, including filters and masks. Zero disables
// retention. Transform and group opacity are applied again on every frame.
#ifndef MEEK_UI_CPU_DECORATION_CACHE_BYTES
    #define MEEK_UI_CPU_DECORATION_CACHE_BYTES (64 * 1024 * 1024)
#endif

// Layer mode: backend textures holding finished group surfaces and implicit
// background layers, reused across frames while their content is unchanged.
// Beyond the budget a layer is drawn through a transient texture instead.
#ifndef MEEK_UI_LAYER_TEXTURE_CACHE_BYTES
    #define MEEK_UI_LAYER_TEXTURE_CACHE_BYTES (96 * 1024 * 1024)
#endif

// Conic geometry is independent of animated colors and rotation. Retain exact
// floating-point angular coordinates so each pixel needs atan2 only once.
#ifndef MEEK_UI_CPU_COORDINATE_CACHE_BYTES
    #define MEEK_UI_CPU_COORDINATE_CACHE_BYTES (32 * 1024 * 1024)
#endif

#if defined(_WIN32) && !defined(MEEK_UI_STATIC)
    #ifdef GUI_BUILDING_DLL
        #define GUI_API __declspec(dllexport)
    #else
        #define GUI_API __declspec(dllimport)
    #endif
    // Event handlers defined in the host are exported even from an
    // .exe so the library's by-name resolver can find them through
    // GetProcAddress(GetModuleHandleW(NULL), name).
    #define UI_HANDLER __declspec(dllexport)
#elif defined(_WIN32)
    #define GUI_API
    #define UI_HANDLER __declspec(dllexport)
#else
    // POSIX builds combine -fvisibility=hidden on the command line
    // with an explicit default visibility on exported symbols.
    // UI_HANDLER also carries `used` so --gc-sections cannot drop a
    // handler that is only referenced by name at runtime.
    #define GUI_API    __attribute__((visibility("default")))
    #define UI_HANDLER __attribute__((visibility("default"), used))
#endif

// When a host does not install meek_host_services_t (host_services.h),
// the library falls back to the C runtime for its log lines and its file
// reads. Build with -DMEEK_UI_HOST_SERVICES_FALLBACK=0 to drop those
// fallbacks and with them every libc symbol they pull in: the host must
// then supply the services before the first call. The heap has its own
// switch: the memory manager (clib/memory_manager.h) uses the C runtime
// unless MEMORY_MANAGER__NO_COMMON_RUNTIME is defined.
#ifndef MEEK_UI_HOST_SERVICES_FALLBACK
#define MEEK_UI_HOST_SERVICES_FALLBACK 1
#endif

#endif
