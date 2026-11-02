// platforms/android/fs_android.c - Android implementation of fs.h.
// Compiled only into the Android build (see demo-android/build.ps1 /
// build.sh source lists). Pairs with:
// - platforms/windows/fs_win32.c     Win32 file mapping
// - platforms/linux/fs_linux.c       POSIX (uses _fs_posix.h shim)
// - platforms/macos/fs_macos.c       POSIX (uses _fs_posix.h shim)
// Two paths inside each fs__* function:
// 1. AAssetManager path -- resolves names against the APK's assets/
// directory. This is what lets parser_xml__load_ui("main.ui") and
// parser_style__load_styles("main.style") work transparently on
// Android. platform_android__init installs the asset manager
// pointer via fs__set_asset_manager before any asset read.
// 2. POSIX fallback -- open + read / mmap against the device
// filesystem. Used when the asset manager isn't set yet, or when
// the asset path resolves to nothing (lets a sideloaded file at
// /sdcard/<name> override the packaged asset for debug).
// fs_mapped_file_t carries platform-private state the same way as on
// Windows but with POSIX values:
// [0] = fd cast to (void*)(intptr_t) -- 0 is a valid fd (stdin), so
// f->size > 0 is the "mapped?" sentinel, not NULL-check.
// [1] = unused
// [2] = view pointer from mmap (also == data)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>

#include <android/asset_manager.h>
#include <sys/types.h>
#include <dirent.h>

#include "types.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// Asset manager bridge. platform_android__init installs this after
// ANativeActivity_onCreate hands us the struct android_app.
// Typed as void* in fs.h so the header doesn't have to pull in the
// NDK's <android/asset_manager.h>; the cast happens here.
static AAssetManager* _fs_android_internal__asset_manager = NULL_POINTER;

// Sideload directory for adb-push hot reload. When non-NULL every
// fs__read_entire_file call first tries <sideload_dir>/<name> via
// POSIX open; only if that misses do we fall through to the
// AAssetManager + absolute-path paths. Installed by
// platform_android__init once the NDK hands us an
// ANativeActivity->externalDataPath. Borrowed pointer -- lives as
// long as the android_app struct does, i.e. the process.
static const char* _fs_android_internal__sideload_dir = NULL_POINTER;

/**
 * @brief install the AAssetManager used for APK asset reads on Android. Must be called before any fs__read_entire_file / fs__map_file call that should resolve against APK-packaged assets; otherwise those fall back to plain filesystem lookups, which won't find APK-internal files.
 *
 * @param void* mgr -> AAssetManager* on Android. Ignored on non-Android builds.
 *
 * @return void
 */
void fs__set_asset_manager(void* mgr)
{
    _fs_android_internal__asset_manager = (AAssetManager*)mgr;
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
    _fs_android_internal__sideload_dir = dir;
    if (dir != NULL_POINTER)
    {
        log_info("%s %s %s", "fs: sideload dir =", dir, "(adb push <file> here for hot reload)");
    }
}

// Strip any leading `/` from a path. AAssetManager and our sideload
// join both want a relative name; the convention for unified main.c
// code is `DEMO_SOURCE_DIR "/main.ui"` which produces `"/main.ui"`
// when the Android build sets DEMO_SOURCE_DIR to the empty string.
// Without this strip AAssetManager_open treats `/main.ui` as missing.
static const char* _fs_android_internal__strip_leading_slash(const char* path)
{
    while (path != NULL_POINTER && *path == '/')
    {
        path++;
    }
    return path;
}

// Open a POSIX file at an absolute path and slurp its entire contents
// into a newly-allocated heap buffer. Returns NULL if the file is
// missing, too big (>2 GiB), or any read step fails. Shared between
// the sideload + POSIX-fallback branches so we don't duplicate the
// fstat/read loop twice.
static char* _fs_android_internal__read_posix(const char* absolute_path, int64* out_size)
{
    int fd = open(absolute_path, O_RDONLY);
    if (fd < 0)
    {
        return NULL_POINTER;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 0x7fffffff)
    {
        close(fd);
        return NULL_POINTER;
    }
    char* buf = (char*)GUI_MALLOC_T((size_t)st.st_size + 1, MM_TYPE__FS);
    if (buf == NULL_POINTER)
    {
        close(fd);
        return NULL_POINTER;
    }
    ssize_t total = 0;
    while (total < st.st_size)
    {
        ssize_t n = read(fd, buf + total, (size_t)(st.st_size - total));
        if (n <= 0)
        {
            GUI_FREE(buf);
            close(fd);
            return NULL_POINTER;
        }
        total += n;
    }
    buf[total] = 0;
    close(fd);
    *out_size = (int64)total;
    return buf;
}

/**
 * @brief read the entire contents of a file at the given UTF-8 path into a heap-allocated buffer. The buffer is null-terminated for convenience but the returned size is authoritative; binary data may contain embedded null bytes.
 *
 * @param char* path -> UTF-8 path. Converted to UTF-16 internally for the W-suffixed Win32 api.
 * @param int64* out_size -> On success, receives the byte length of the read content (excluding the trailing null).
 *
 * @return char* -> Heap buffer (caller must free), or NULL on any I/O error (file not found, read failure, file > 2GB, allocation failure). Open mode shares both read and write so editors that briefly hold the file open mid-save don't block this read. Errors are silent; callers that want logging should add their own.
 */
char* fs__read_entire_file(char* path, int64* out_size)
{
    const char* name = _fs_android_internal__strip_leading_slash(path);

    // 1. SIDELOAD. If the host has installed a sideload dir, check
    // <sideload>/<name> first -- wins over APK-packaged assets so
    // adb-pushed files immediately override. No log on miss; the
    // expected steady state is "no sideloaded file, fall through".
    if (_fs_android_internal__sideload_dir != NULL_POINTER && name != NULL_POINTER && *name != 0)
    {
        char joined[1024];
        int64 written = 0;
        stdlib__strcpy_safe(joined, (int64)sizeof(joined), _fs_android_internal__sideload_dir);
        stdlib__strcat_safe(joined, (int64)sizeof(joined), "/");
        written = stdlib__strcat_safe(joined, (int64)sizeof(joined), name);
        if (written > 0 && written < (int64)sizeof(joined))
        {
            char* buf = _fs_android_internal__read_posix(joined, out_size);
            if (buf != NULL_POINTER)
            {
                log_info("%s %s %s %lld %s", "fs: sideload hit '", joined, "' (", (long long)*out_size, "bytes)");
                return buf;
            }
        }
    }

    // 2. AAssetManager. Parser callers pass paths like "main.ui" that
    // live inside the APK's assets/ directory; plain open() can't
    // reach into the APK zip. Leading `/` was already stripped so
    // `"/main.ui"` and `"main.ui"` both land on `"main.ui"`.
    if (_fs_android_internal__asset_manager != NULL_POINTER && name != NULL_POINTER && *name != 0)
    {
        AAsset* asset = AAssetManager_open(_fs_android_internal__asset_manager, name, AASSET_MODE_BUFFER);
        if (asset != NULL_POINTER)
        {
            off_t asset_len = AAsset_getLength(asset);
            if (asset_len < 0 || asset_len > 0x7fffffff)
            {
                log_error("%s %s %s", "fs: asset length out of range for '", name, "'");
                AAsset_close(asset);
                return NULL_POINTER;
            }
            char* buf = (char*)GUI_MALLOC_T((size_t)asset_len + 1, MM_TYPE__FS);
            if (buf == NULL_POINTER)
            {
                log_error("%s %lld %s %s %s", "fs: alloc(", (long long)(asset_len + 1), ") failed for asset '", name, "'");
                AAsset_close(asset);
                return NULL_POINTER;
            }
            int read_n = AAsset_read(asset, buf, (size_t)asset_len);
            AAsset_close(asset);
            if (read_n < 0 || read_n != (int)asset_len)
            {
                log_error("%s %d %s %lld %s %s %s", "fs: AAsset_read short/failed (got", read_n, "of", (long long)asset_len, ") for '", name, "'");
                GUI_FREE(buf);
                return NULL_POINTER;
            }
            buf[read_n] = 0;
            *out_size = (int64)read_n;
            return buf;
        }
    }

    // 3. POSIX fallback with the ORIGINAL path. Lets callers reach
    // absolute paths like /sdcard/foo.ui when they want them. Same
    // 2 GiB guard as the Windows branch.
    return _fs_android_internal__read_posix(path, out_size);
}

// POSIX mmap(MAP_SHARED, PROT_READ). The kernel's page cache
// transparently shares physical pages across every process that
// maps the same file -- same deduplication semantics as Windows'
// CreateFileMapping. We stash the fd in _platform[0] (cast through
// intptr_t) so unmap can close it.
// NOT used for APK-packaged assets: AAssetManager assets are typically
// compressed in the zip, so they have no addressable fd + offset the
// caller could mmap directly. Asset bytes go through the heap-copy
// fs__read_entire_file path above. mmap is for /sdcard paths, external
// files, debug sideloads, etc.
/**
 * @brief memory-map a file read-only. On all platforms this is shared-by- default: if another process maps the same file, the kernel reuses the same physical pages. Ideal for font files, shader blobs, and other read-only assets that a host app wants to share across instances (multiple windows in one app, or multiple co-operating daemons).
 *
 * @param char* path -> UTF-8 path.
 * @param fs_mapped_file_t* out -> Filled on success.
 *
 * @return boole -> TRUE on success; FALSE on any I/O or mapping error.
 */
boole fs__map_file(char* path, fs_mapped_file_t* out)
{
    if (path == NULL_POINTER || out == NULL_POINTER)
    {
        return FALSE;
    }
    stdlib__memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        log_error("%s %s %s %d %s", "fs__map_file: open failed for '", path, "' (errno", errno, ")");
        return FALSE;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        log_error("%s %s %s", "fs__map_file: fstat failed or empty file: '", path, "'");
        close(fd);
        return FALSE;
    }

    void* view = mmap(NULL_POINTER, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED)
    {
        log_error("%s %s %s %d %s", "fs__map_file: mmap failed for '", path, "' (errno", errno, ")");
        close(fd);
        return FALSE;
    }

    out->data         = (const ubyte*)view;
    out->size         = (int64)st.st_size;
    out->_platform[0] = (void*)(intptr_t)fd;
    out->_platform[1] = NULL_POINTER;
    out->_platform[2] = view;
    return TRUE;
}

/**
 * @brief release a mapping obtained via fs__map_file. After this call the pointer stored in `data` is invalid. Safe to call on a zero-filled struct (no-op).
 *
 * @param fs_mapped_file_t* f -> Mapping to release.
 *
 * @return void
 */
void fs__unmap_file(fs_mapped_file_t* f)
{
    if (f == NULL_POINTER || f->data == NULL_POINTER)
    {
        return;
    }
    if (f->_platform[2] != NULL_POINTER && f->size > 0)
    {
        munmap(f->_platform[2], (size_t)f->size);
    }
    // fd stashed as (void*)(intptr_t)fd. 0 is a valid fd (stdin), so
    // we can't use NULL as the sentinel -- check f->size > 0 instead
    // as the "was mapped" signal.
    if (f->size > 0)
    {
        close((int)(intptr_t)f->_platform[0]);
    }
    stdlib__memset(f, 0, sizeof(*f));
}

// ============================================================================
// PUBLIC: fs__write_entire_file
// ============================================================================

/**
 * @brief writes a buffer to a file, creating it or replacing its contents; built on open / write so the capture and screenshot paths need no stdio
 *
 * @param char* path -> file path
 * @param const void* data -> bytes to write; may be NULL_POINTER when size is 0
 * @param int64 size -> number of bytes
 *
 * @return boole -> TRUE when every byte was written
 */
boole fs__write_entire_file(char* path, const void* data, int64 size)
{
    int fd = -1;
    const unsigned char* bytes = (const unsigned char*)data;
    int64 done = 0;
    ssize_t written = 0;

    if (path == NULL_POINTER || size < 0 || (data == NULL_POINTER && size > 0))
    {
        return FALSE;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
    {
        log_error("%s %s", "fs__write_entire_file: open failed for", path);
        return FALSE;
    }

    while (done < size)
    {
        written = write(fd, bytes + done, (size_t)(size - done));

        if (written <= 0)
        {
            log_error("%s %s", "fs__write_entire_file: write failed for", path);
            close(fd);
            return FALSE;
        }

        done = done + (int64)written;
    }

    close(fd);

    return TRUE;
}

/**
 * @brief list a directory on the real filesystem (not the APK): fn is called for every entry except "." and "..".
 *
 * @param char* path -> Directory path.
 * @param fs_dir_entry_fn fn -> Receives name, is_dir, size (0 for directories) and user.
 * @param void* user -> Passed through to fn.
 *
 * @return boole -> FALSE when the directory cannot be opened.
 */
boole fs__list_dir(char* path, fs_dir_entry_fn fn, void* user)
{
    DIR*           d   = NULL_POINTER;
    struct dirent* ent = NULL_POINTER;
    char           full[1024];
    struct stat    st;
    size_t         n   = 0;

    if (path == NULL_POINTER || fn == NULL_POINTER)
    {
        return FALSE;
    }

    d = opendir(path);

    if (d == NULL_POINTER)
    {
        return FALSE;
    }

    n = stdlib__strlen(path);

    while ((ent = readdir(d)) != NULL_POINTER)
    {
        const char* name   = ent->d_name;
        size_t      m      = stdlib__strlen(name);
        boole       is_dir = FALSE;
        int64       size   = 0;

        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        {
            continue;
        }

        if (n + 1 + m >= sizeof(full))
        {
            continue;
        }

        stdlib__memcpy(full, path, n);
        full[n] = '/';
        stdlib__memcpy(full + n + 1, name, m + 1);

        if (stat(full, &st) == 0)
        {
            is_dir = (boole)S_ISDIR(st.st_mode);
            size   = is_dir ? 0 : (int64)st.st_size;
        }

        fn(name, is_dir, size, user);
    }

    closedir(d);

    return TRUE;
}
