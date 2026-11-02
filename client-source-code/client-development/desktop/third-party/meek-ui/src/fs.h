#ifndef FS_H
#define FS_H

#include "types.h"
#include "gui_api.h"

// fs.h - small filesystem helpers built directly on Win32.
// paired with fs.c. extracted because three modules (parser_xml,
// parser_style, hot_reload) all needed the same "read a whole file
// into a heap buffer" routine and were maintaining identical copies.

GUI_API char* fs__read_entire_file(char* path, int64* out_size);

// fs__write_entire_file creates or replaces the file at `path` with the
// `size` bytes at `data`. built on the OS file API directly (CreateFile /
// WriteFile on Windows, open / write elsewhere) so the capture and
// screenshot paths do not pull stdio into the library.
GUI_API boole fs__write_entire_file(char* path, const void* data, int64 size);

// ============================================================================
// memory-mapped files
// ============================================================================
// fs__map_file maps a file read-only into the process's address space
// without copying. multiple processes that map the same file share the
// same physical pages via the kernel's page cache -- zero extra RAM
// per additional process. this is the idiomatic way to share
// read-only blobs (TTF fonts, shader bytecode, compiled .ui, etc.)
// across processes on every platform:
// Windows:          CreateFileMapping + MapViewOfFile
// POSIX / Android:  mmap(fd, PROT_READ, MAP_SHARED)
// iOS / macOS:      same as POSIX
// semantics: the mapping is read-only. writes through `data` SIGSEGV /
// raise an access violation. the mapping stays alive until
// fs__unmap_file; keep the fs_mapped_file_t struct around while any code
// reads through `data`.
// on Windows, the API requires three handles (file, mapping object,
// view pointer). on POSIX one pointer + length is enough. the struct
// holds both shapes of platform state behind a single _platform array
// so callers don't have to know which OS they're on.

typedef struct fs_mapped_file_t
{
    // pointer to mapped region (read-only).
    const ubyte* data;    // 0x0
    // byte length of the region.
    int64        size;    // 0x8

    // opaque platform handles. on Windows: [0] = file HANDLE,
    // [1] = mapping HANDLE, [2] = view pointer (same as data cast to
    // void*). on POSIX: unused -- `data` + `size` are passed to munmap
    // directly.
    void* _platform[3];   // 0x10
} fs_mapped_file_t;

GUI_API boole fs__map_file(char* path, fs_mapped_file_t* out);

GUI_API void  fs__unmap_file(fs_mapped_file_t* f);

// ============================================================================
// Android: asset manager bridge.
// ============================================================================
// On Android, the files a host app ships (main.ui, main.style, TTFs,
// etc.) live inside the APK under `assets/` and aren't reachable via
// plain open() -- they're entries in the APK's zip archive. the
// framework exposes them via AAssetManager, which needs to be plumbed
// into fs so the parser and font modules can stay path-based.
// The platform layer calls fs__set_asset_manager(app->activity->assetManager)
// after ANativeActivity_onCreate gives it an app pointer. After that,
// fs__read_entire_file on Android tries AAssetManager_open(path) FIRST
// and only falls back to plain open() if the asset-manager path misses.
// Windows builds ignore the setter entirely (compiled out).
// The parameter is typed `void*` to keep fs.h from having to include
// the NDK headers; fs.c casts it to `AAssetManager*` inside its
// #if !defined(_WIN32) branch.

GUI_API void fs__set_asset_manager(void* mgr);

// Sideload directory override. When set, fs__read_entire_file tries
// <sideload_dir>/<path> (with any leading `/` stripped from path)
// BEFORE the AAssetManager / POSIX fallback chain. Intended for dev
// builds on Android: `adb push main.ui <sideload_dir>/main.ui` drops
// a newer copy next to the APK-packaged version, and the next
// hot_reload poll picks it up. Passing NULL (or never calling this)
// turns the feature off.
// Typical wiring on Android:
// fs__set_sideload_dir(app->activity->externalDataPath);
// which resolves to /storage/emulated/0/Android/data/<pkg>/files/ --
// readable by the app and writable by adb on every modern Android
// version without runtime permissions or root.
// Ignored on non-Android builds (Windows uses real paths already).

GUI_API void fs__set_sideload_dir(const char* dir);

// ============================================================================
// directory listing
// ============================================================================
// fs__list_dir calls fn once per entry of the directory at `path`, in
// the order the OS returns them, never for "." and "..". `size` is the
// file size in bytes, 0 for directories. Used by the file picker
// widget. Returns FALSE when the directory cannot be opened.
typedef void (*fs_dir_entry_fn)(const char* name, boole is_dir, int64 size, void* user);

GUI_API boole fs__list_dir(char* path, fs_dir_entry_fn fn, void* user);

#endif
