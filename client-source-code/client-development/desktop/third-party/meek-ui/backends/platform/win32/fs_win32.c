// platforms/windows/fs_win32.c - Windows implementation of fs.h.
// Compiled only into the Windows build (see gui/CMakeLists.txt).
// Pairs with:
// - platforms/android/fs_android.c   Android + POSIX fallback
// - platforms/linux/fs_linux.c       POSIX (uses _fs_posix.h shim)
// - platforms/macos/fs_macos.c       POSIX (uses _fs_posix.h shim)
// Conventions:
// - fs__read_entire_file heap-copies the entire file contents and
// appends a trailing null, same shape as on every other platform.
// - fs__map_file returns a read-only shared view backed by
// CreateFileMapping + MapViewOfFile. The kernel page cache dedups
// physical pages across every process that maps the same file,
// which is how the font subsystem gets multi-process font sharing
// for free.
// - fs__set_asset_manager is a no-op on Windows: asset-manager paths
// only exist inside an APK. host code calls this unconditionally
// and this file accepts the call silently.
// The fs_mapped_file_t struct carries platform-private state in its
// _platform[] slots:
// [0] = file   HANDLE  from CreateFileW
// [1] = mapping HANDLE  from CreateFileMappingW
// [2] = view   pointer from MapViewOfFile (also == data)

#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "types.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

/**
 * @brief install the AAssetManager used for APK asset reads on Android. Must be called before any fs__read_entire_file / fs__map_file call that should resolve against APK-packaged assets; otherwise those fall back to plain filesystem lookups, which won't find APK-internal files.
 *
 * @param void* mgr -> AAssetManager* on Android. Ignored on non-Android builds.
 *
 * @return void
 */
void fs__set_asset_manager(void* mgr)
{
    // Android-only bridge. On Windows the host has direct filesystem
    // access so there's nothing to install. Accepting the call keeps
    // the host API identical across platforms.
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
    // Android-only. Windows hosts read .ui / .style directly from the
    // dev tree via DEMO_SOURCE_DIR, so there's no APK to override and
    // no sideload. Accept the call so host code can set it
    // unconditionally regardless of target.
    (void)dir;
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
    // UTF-8 -> UTF-16 wide path. Win32 filesystem APIs with non-ASCII
    // paths require the W variants; the *A variants go through the
    // system ANSI code page which varies by locale.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL_POINTER, 0);
    if (wlen <= 0)
    {
        return NULL_POINTER;
    }

    wchar_t* wpath = (wchar_t*)GUI_MALLOC_T((size_t)wlen * sizeof(wchar_t), MM_TYPE__FS);
    if (wpath == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL_POINTER, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL_POINTER);
    GUI_FREE(wpath);
    if (h == INVALID_HANDLE_VALUE)
    {
        return NULL_POINTER;
    }

    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) == FALSE)
    {
        CloseHandle(h);
        return NULL_POINTER;
    }
    if (size.QuadPart > 0x7fffffff)
    {
        // Refuse files larger than ~2 GiB. .ui / .style / .ttf files
        // should be tiny; this guards against accidentally pointing
        // the loader at an unrelated giant file (a disk image, a DB).
        CloseHandle(h);
        return NULL_POINTER;
    }

    char* buf = (char*)GUI_MALLOC_T((size_t)size.QuadPart + 1, MM_TYPE__FS);
    if (buf == NULL_POINTER)
    {
        CloseHandle(h);
        return NULL_POINTER;
    }

    DWORD read = 0;
    if (ReadFile(h, buf, (DWORD)size.QuadPart, &read, NULL_POINTER) == FALSE)
    {
        GUI_FREE(buf);
        CloseHandle(h);
        return NULL_POINTER;
    }
    buf[read] = 0;
    CloseHandle(h);

    *out_size = (int64)read;
    return buf;
}

// Memory-mapped read-only view. Three kernel objects to keep around
// for cleanup: file handle, mapping handle, view pointer.
// PAGE_READONLY matches the GENERIC_READ open -- attempts to write
// through the returned pointer will access-violate. The mapping is
// unnamed: sharing with other processes happens via the common backing
// file, not via a named section, so no explicit setup is needed.
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

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL_POINTER, 0);
    if (wlen <= 0)
    {
        return FALSE;
    }
    wchar_t* wpath = (wchar_t*)GUI_MALLOC_T((size_t)wlen * sizeof(wchar_t), MM_TYPE__FS);
    if (wpath == NULL_POINTER)
    {
        return FALSE;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    // Broad share flags so other processes can also map the file, and
    // so editors holding the file open briefly don't block us.
    HANDLE file = CreateFileW( wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL_POINTER, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL_POINTER );
    GUI_FREE(wpath);
    if (file == INVALID_HANDLE_VALUE)
    {
        log_error("%s %s %s %lu %s", "fs__map_file: CreateFileW failed for '", path, "' (error", (unsigned long)GetLastError(), ")");
        return FALSE;
    }

    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0)
    {
        log_error("%s %s %s", "fs__map_file: GetFileSizeEx failed or empty file: '", path, "'");
        CloseHandle(file);
        return FALSE;
    }
    if (size.QuadPart > 0x7fffffff)
    {
        log_error("%s %s %s", "fs__map_file: refusing file > 2 GiB: '", path, "'");
        CloseHandle(file);
        return FALSE;
    }

    HANDLE mapping = CreateFileMappingW( file, NULL_POINTER, /* default security. */ PAGE_READONLY, 0, 0, /* mapping size = file size. */ NULL_POINTER /* unnamed mapping. */ );
    if (mapping == NULL_POINTER)
    {
        log_error("%s %s %s %lu %s", "fs__map_file: CreateFileMappingW failed for '", path, "' (error", (unsigned long)GetLastError(), ")");
        CloseHandle(file);
        return FALSE;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == NULL_POINTER)
    {
        log_error("%s %s %s %lu %s", "fs__map_file: MapViewOfFile failed for '", path, "' (error", (unsigned long)GetLastError(), ")");
        CloseHandle(mapping);
        CloseHandle(file);
        return FALSE;
    }

    out->data         = (const ubyte*)view;
    out->size         = (int64)size.QuadPart;
    out->_platform[0] = (void*)file;
    out->_platform[1] = (void*)mapping;
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
    // Tear down in reverse of construction. Each of these is a no-op
    // on NULL / invalid handles, but we null-check for resilience
    // against zero-initialized structs.
    if (f->_platform[2] != NULL_POINTER)
    {
        UnmapViewOfFile(f->_platform[2]);
    }
    if (f->_platform[1] != NULL_POINTER)
    {
        CloseHandle((HANDLE)f->_platform[1]);
    }
    if (f->_platform[0] != NULL_POINTER)
    {
        CloseHandle((HANDLE)f->_platform[0]);
    }
    stdlib__memset(f, 0, sizeof(*f));
}

// ============================================================================
// PUBLIC: fs__write_entire_file
// ============================================================================

/**
 * @brief writes a buffer to a file, creating it or replacing its contents; built on CreateFileA / WriteFile so the capture and screenshot paths need no stdio
 *
 * @param char* path -> file path
 * @param const void* data -> bytes to write; may be NULL_POINTER when size is 0
 * @param int64 size -> number of bytes
 *
 * @return boole -> TRUE when every byte was written
 */
boole fs__write_entire_file(char* path, const void* data, int64 size)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    const unsigned char* bytes = (const unsigned char*)data;
    int64 done = 0;
    DWORD chunk = 0;
    DWORD written = 0;

    if (path == NULL_POINTER || size < 0 || (data == NULL_POINTER && size > 0))
    {
        return FALSE;
    }

    file = CreateFileA(path, GENERIC_WRITE, 0, NULL_POINTER, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL_POINTER);

    if (file == INVALID_HANDLE_VALUE)
    {
        log_error("%s %s %lu", "fs__write_entire_file: CreateFileA failed for", path, (unsigned long)GetLastError());
        return FALSE;
    }

    while (done < size)
    {
        // WriteFile takes a 32-bit count; large captures go out in slices
        chunk = (size - done > 0x40000000) ? (DWORD)0x40000000 : (DWORD)(size - done);
        written = 0;

        if (WriteFile(file, bytes + done, chunk, &written, NULL_POINTER) == 0 || written == 0)
        {
            log_error("%s %s %lu", "fs__write_entire_file: WriteFile failed for", path, (unsigned long)GetLastError());
            CloseHandle(file);
            return FALSE;
        }

        done = done + (int64)written;
    }

    CloseHandle(file);

    return TRUE;
}

/**
 * @brief list a directory: fn is called for every entry except "." and "..", in the order FindNextFileW returns them.
 *
 * @param char* path -> UTF-8 directory path, with or without a trailing separator.
 * @param fs_dir_entry_fn fn -> Receives name (UTF-8), is_dir, size (0 for directories) and user.
 * @param void* user -> Passed through to fn.
 *
 * @return boole -> FALSE when the directory cannot be opened or the path is too long.
 */
boole fs__list_dir(char* path, fs_dir_entry_fn fn, void* user)
{
    char             pattern[1024];
    WCHAR            wpattern[1024];
    WIN32_FIND_DATAW fd;
    HANDLE           h = INVALID_HANDLE_VALUE;
    size_t           n = 0;

    if (path == NULL_POINTER || fn == NULL_POINTER)
    {
        return FALSE;
    }

    n = stdlib__strlen(path);

    if (n == 0 || n + 3 >= sizeof(pattern))
    {
        return FALSE;
    }

    stdlib__memcpy(pattern, path, n);

    if (pattern[n - 1] != '\\' && pattern[n - 1] != '/')
    {
        pattern[n++] = '\\';
    }

    pattern[n++] = '*';
    pattern[n]   = 0;

    if (MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, (int)(sizeof(wpattern) / sizeof(wpattern[0]))) == 0)
    {
        return FALSE;
    }

    h = FindFirstFileW(wpattern, &fd);

    if (h == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    do
    {
        char  name[512];
        boole is_dir = (boole)((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        int64 size   = ((int64)fd.nFileSizeHigh << 32) | (int64)fd.nFileSizeLow;

        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, (int)sizeof(name), NULL_POINTER, NULL_POINTER) == 0)
        {
            continue;
        }

        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        {
            continue;
        }

        fn(name, is_dir, is_dir ? 0 : size, user);
    } while (FindNextFileW(h, &fd) != 0);

    FindClose(h);

    return TRUE;
}
