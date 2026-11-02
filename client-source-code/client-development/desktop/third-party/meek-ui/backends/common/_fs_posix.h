#ifndef _FS_POSIX_H
#define _FS_POSIX_H

// _fs_posix.h - shared POSIX implementations of the fs.h slurp +
// mmap entry points. Used by fs_linux.c, fs_macos.c, and the
// POSIX fall-through in fs_android.c. Header-only because each
// platform TU still wants its own translation unit (per the
// per-platform-dir convention) but the bodies are identical
// modulo error-message phrasing.
// Inclusion shape:
// - Define FS_POSIX_PLATFORM_TAG before including this header
// ("linux", "macos", or "android-posix"); used in log messages
// so a runtime error tells you which platform's path failed.
// - Then call FS_POSIX_DEFINE_SLURP() and FS_POSIX_DEFINE_MMAP()
// to emit the function bodies into the including TU.
// Why macros instead of plain functions: the per-platform TUs also
// own the `fs__set_asset_manager` / `fs__set_sideload_dir` stubs
// (which differ on Android), and we want a single-binary build
// (no static lib) so each platform contributes ONE .o with the
// full fs interface. Macro expansion fits that shape with zero
// link-time gymnastics.

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>

#include "types.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

#ifndef FS_POSIX_PLATFORM_TAG
  #define FS_POSIX_PLATFORM_TAG "posix"
#endif

#define FS_POSIX_DEFINE_SLURP()                                                          \
char* fs__read_entire_file(char* path, int64* out_size)                                  \
{                                                                                        \
    if (path == NULL_POINTER || out_size == NULL_POINTER) { return NULL_POINTER; }                               \
    *out_size = 0;                                                                       \
    int fd = open(path, O_RDONLY);                                                       \
    if (fd < 0) { return NULL_POINTER; }                                                         \
    struct stat st;                                                                      \
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 0x7fffffff)                \
    {                                                                                    \
        close(fd);                                                                       \
        return NULL_POINTER;                                                                     \
    }                                                                                    \
    char* buf = (char*)GUI_MALLOC_T((size_t)st.st_size + 1, MM_TYPE__FS);                 \
    if (buf == NULL_POINTER) { close(fd); return NULL_POINTER; }                                         \
    ssize_t total = 0;                                                                   \
    while (total < st.st_size)                                                           \
    {                                                                                    \
        ssize_t n = read(fd, buf + total, (size_t)(st.st_size - total));                 \
        if (n <= 0) { GUI_FREE(buf); close(fd); return NULL_POINTER; }                           \
        total += n;                                                                      \
    }                                                                                    \
    buf[total] = 0;                                                                      \
    close(fd);                                                                           \
    *out_size = (int64)total;                                                            \
    return buf;                                                                          \
}

#define FS_POSIX_DEFINE_MMAP()                                                           \
boole fs__map_file(char* path, fs_mapped_file_t* out)                                      \
{                                                                                        \
    if (path == NULL_POINTER || out == NULL_POINTER) { return FALSE; }                                   \
    stdlib__memset(out, 0, sizeof(*out));                                                        \
    int fd = open(path, O_RDONLY);                                                       \
    if (fd < 0)                                                                          \
    {                                                                                    \
        log_error("fs__map_file[" FS_POSIX_PLATFORM_TAG "]: open('%s') failed (%s)",     \
                  path, strerror(errno));                                                \
        return FALSE;                                                                    \
    }                                                                                    \
    struct stat st;                                                                      \
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 0x7fffffff)               \
    {                                                                                    \
        close(fd);                                                                       \
        return FALSE;                                                                    \
    }                                                                                    \
    void* mapped = mmap(NULL_POINTER, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);         \
    if (mapped == MAP_FAILED)                                                            \
    {                                                                                    \
        log_error("fs__map_file[" FS_POSIX_PLATFORM_TAG "]: mmap('%s') failed (%s)",     \
                  path, strerror(errno));                                                \
        close(fd);                                                                       \
        return FALSE;                                                                    \
    }                                                                                    \
    close(fd);                                                                           \
    out->data         = (const ubyte*)mapped;                                            \
    out->size         = (int64)st.st_size;                                               \
    out->_platform[0] = NULL_POINTER;                                                            \
    out->_platform[1] = NULL_POINTER;                                                            \
    out->_platform[2] = mapped;                                                          \
    return TRUE;                                                                         \
}                                                                                        \
                                                                                         \
void fs__unmap_file(fs_mapped_file_t* f)                                                   \
{                                                                                        \
    if (f == NULL_POINTER || f->data == NULL_POINTER || f->size <= 0) { return; }                        \
    munmap((void*)f->data, (size_t)f->size);                                             \
    f->data = NULL_POINTER;                                                                      \
    f->size = 0;                                                                         \
    stdlib__memset(f->_platform, 0, sizeof(f->_platform));                                       \
}

#define FS_POSIX_DEFINE_WRITE()                                                          \
boole fs__write_entire_file(char* path, const void* data, int64 size)                    \
{                                                                                        \
    int fd = -1;                                                                         \
    const unsigned char* bytes = (const unsigned char*)data;                             \
    int64 done = 0;                                                                      \
    ssize_t written = 0;                                                                 \
    if (path == NULL_POINTER || size < 0 || (data == NULL_POINTER && size > 0))         \
    {                                                                                    \
        return FALSE;                                                                    \
    }                                                                                    \
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);                                 \
    if (fd < 0)                                                                          \
    {                                                                                    \
        log_error("%s %s %s", "fs(" FS_POSIX_PLATFORM_TAG "): open for write failed:", path, strerror(errno)); \
        return FALSE;                                                                    \
    }                                                                                    \
    while (done < size)                                                                  \
    {                                                                                    \
        written = write(fd, bytes + done, (size_t)(size - done));                        \
        if (written <= 0)                                                                \
        {                                                                                \
            log_error("%s %s %s", "fs(" FS_POSIX_PLATFORM_TAG "): write failed:", path, strerror(errno)); \
            close(fd);                                                                   \
            return FALSE;                                                                \
        }                                                                                \
        done = done + (int64)written;                                                    \
    }                                                                                    \
    close(fd);                                                                           \
    return TRUE;                                                                         \
}

#define FS_POSIX_DEFINE_LIST_DIR()                                                       \
boole fs__list_dir(char* path, fs_dir_entry_fn fn, void* user)                           \
{                                                                                        \
    DIR*           d    = NULL_POINTER;                                                  \
    struct dirent* ent  = NULL_POINTER;                                                  \
    char           full[1024];                                                           \
    struct stat    st;                                                                   \
    size_t         n    = 0;                                                             \
    if (path == NULL_POINTER || fn == NULL_POINTER) { return FALSE; }                    \
    d = opendir(path);                                                                   \
    if (d == NULL_POINTER) { return FALSE; }                                             \
    n = strlen(path);                                                                    \
    while ((ent = readdir(d)) != NULL_POINTER)                                           \
    {                                                                                    \
        const char* name   = ent->d_name;                                                \
        size_t      m      = strlen(name);                                               \
        boole       is_dir = FALSE;                                                      \
        int64       size   = 0;                                                          \
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) { continue; } \
        if (n + 1 + m >= sizeof(full)) { continue; }                                     \
        memcpy(full, path, n);                                                           \
        full[n] = '/';                                                                   \
        memcpy(full + n + 1, name, m + 1);                                               \
        if (stat(full, &st) == 0)                                                        \
        {                                                                                \
            is_dir = (boole)S_ISDIR(st.st_mode);                                         \
            size   = is_dir ? 0 : (int64)st.st_size;                                     \
        }                                                                                \
        fn(name, is_dir, size, user);                                                    \
    }                                                                                    \
    closedir(d);                                                                         \
    return TRUE;                                                                         \
}

#endif
