// platforms/macos/fs_macos.c - macOS fs.h implementation.
// Body lives in ../_fs_posix.h (shared with fs_linux.c). macOS is a
// POSIX system for our purposes (open / read / mmap behave identically
// on XNU and Linux), so this TU is a thin shim around the shared
// macros plus the Android-only stubs that every non-Android platform
// must accept-and-ignore.

#define FS_POSIX_PLATFORM_TAG "macos"
#include "_fs_posix.h"

/**
 * @brief install the AAssetManager used for APK asset reads on Android. Must be called before any fs__read_entire_file / fs__map_file call that should resolve against APK-packaged assets; otherwise those fall back to plain filesystem lookups, which won't find APK-internal files.
 *
 * @param void* mgr -> AAssetManager* on Android. Ignored on non-Android builds.
 *
 * @return void
 */
void fs__set_asset_manager(void* mgr)
{
    // Android-only bridge. macOS hosts read the real filesystem
    // directly. No-op here.
    (void)mgr;
}

/**
 * @brief install a sideload directory that fs__read_entire_file checks before AAssetManager / absolute-path fallbacks. The string is borrowed, not copied; keep it alive for the process lifetime.
 *
 * @param const char* dir -> UTF-8 directory path, no trailing slash. NULL to disable.
 *
 * @return void
 */
void fs__set_sideload_dir(const char* dir)
{
    // Android-only adb-push hot-reload mechanism. No-op on macOS.
    (void)dir;
}

FS_POSIX_DEFINE_SLURP()
FS_POSIX_DEFINE_MMAP()
FS_POSIX_DEFINE_WRITE()
FS_POSIX_DEFINE_LIST_DIR()
