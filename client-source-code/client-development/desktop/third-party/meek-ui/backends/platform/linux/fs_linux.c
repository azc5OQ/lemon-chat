// platforms/linux/fs_linux.c - Linux fs.h implementation.
// Body lives in ../_fs_posix.h (shared with fs_macos.c). This TU
// owns the platform-specific stubs (set_asset_manager + set_sideload_dir,
// both no-ops on Linux -- those are Android-only bridges) and emits
// the slurp + mmap implementations via the FS_POSIX_DEFINE_* macros.

#define FS_POSIX_PLATFORM_TAG "linux"
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
    // Android-only bridge. Linux hosts read the real filesystem
    // directly. Accepted silently so host init code stays
    // platform-neutral.
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
    // Android-only override mechanism for adb-push hot reload.
    // Same accept-and-ignore pattern as fs__set_asset_manager.
    (void)dir;
}

FS_POSIX_DEFINE_SLURP()
FS_POSIX_DEFINE_MMAP()
FS_POSIX_DEFINE_WRITE()
FS_POSIX_DEFINE_LIST_DIR()
