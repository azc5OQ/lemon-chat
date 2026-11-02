// hot_reload.c - polling content-hash watcher for .ui and .style files.
// on every tick, each watched file is opened, read, and hashed
// (fnv-1a 32-bit over the bytes). when a watch's stored hash differs
// from the new one, the file is re-applied:
// - .ui:    parser_xml__load_ui -> new tree -> scene__set_root,
// previous tree freed.
// - .style: scene__clear_styles -> parser_style__load_styles.
// a small fixed table holds up to MAX_WATCHES entries. host apps
// typically register one .ui watch + one .style watch; the table
// tolerates a handful more without dynamic allocation.
// file io is delegated to fs__read_entire_file.
// polling cadence: tick() throttles itself to one filesystem check per
// ~1000ms. 60 fps polling on a tiny .ui file is harmless but pointless;
// once-a-second is well below human reaction time and keeps disk
// activity invisible. Clock source is per-platform -- GetTickCount64
// on Windows, clock_gettime(CLOCK_MONOTONIC) on Android / POSIX.

// Request the POSIX clock declarations even in strict C11 builds.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
    #define _POSIX_C_SOURCE 200809L
#endif

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <time.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "parser_xml.h"
#include "parser_style.h"
#include "hot_reload.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

#define _HOT_RELOAD_INTERNAL__POLL_INTERVAL_MS 1000

/**
 *which kind of file a watch entry tracks. dispatch in tick switches
 *on this to pick the right reload action.
 */
typedef enum _hot_reload_internal__kind_e
{
    _HOT_RELOAD_INTERNAL__KIND_UI,
    _HOT_RELOAD_INTERNAL__KIND_STYLE
} _hot_reload_internal__kind_e;

/**
 *one entry in the watch table. path + last successfully-loaded hash
 *plus the kind. tree is the tree we own (only meaningful for UI
 *watches); freed on next successful reload or on shutdown.
 */
typedef struct _hot_reload_internal__watch_t
{
    char                       path[512];   // 0x0
    uint                       last_hash;   // 0x200
    _hot_reload_internal__kind_e kind;        // 0x204
    boole                      active;      // 0x208
    // only valid when kind == KIND_UI.
    gui_node_t*                  tree;        // 0x210
} _hot_reload_internal__watch_t;

#define _HOT_RELOAD_INTERNAL__MAX_WATCHES 8

static _hot_reload_internal__watch_t _hot_reload_internal__watches[_HOT_RELOAD_INTERNAL__MAX_WATCHES];
static int64                       _hot_reload_internal__count = 0;

// timestamp (in ms-since-boot) of the previous tick that actually ran
// the poll loop. zero means "never polled yet"; the first tick will
// always poll because (now - 0) >= interval.
static uint64 _hot_reload_internal__last_poll_ms = 0;

// portable monotonic ms-since-boot. Win32: GetTickCount64 rolls over
// every ~584 million years, plenty. POSIX: CLOCK_MONOTONIC is the
// documented drop-in; tv_sec*1000 + tv_nsec/1e6 yields the same ms
// scale GetTickCount64 returns, so the same subtraction math works
// on both platforms without per-branch special-casing at call sites.
static uint64 _hot_reload_internal__now_ms(void)
{
#if defined(_WIN32)
    return (uint64)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64)ts.tv_sec * 1000u + (uint64)(ts.tv_nsec / 1000000);
#endif
}

// forward declarations of file-local helpers.
static uint                         _hot_reload_internal__fnv1a(ubyte* data, int64 len);
static _hot_reload_internal__watch_t* _hot_reload_internal__alloc_watch(char* path, _hot_reload_internal__kind_e kind);
static void                         _hot_reload_internal__poll_one(_hot_reload_internal__watch_t* w);
static void                         _hot_reload_internal__reload_ui(_hot_reload_internal__watch_t* w);
static void                         _hot_reload_internal__reload_style(_hot_reload_internal__watch_t* w);

// ===== public api ===========================================================

/**
 * @brief begin watching a .ui file. Performs the initial load synchronously, sets the parsed tree as the scene root, and stores its content hash.
 *
 * @param char* path -> Absolute path to the .ui file.
 *
 * @return boole -> TRUE if the initial load succeeded; FALSE otherwise.
 */
boole hot_reload__watch_ui(char* path)
{
    _hot_reload_internal__watch_t* w = _hot_reload_internal__alloc_watch(path, _HOT_RELOAD_INTERNAL__KIND_UI);
    if (w == NULL_POINTER)
    {
        return FALSE;
    }

    int64 size = 0;
    char* buf  = fs__read_entire_file(w->path, &size);
    if (buf == NULL_POINTER)
    {
        log_error("%s %s", "initial read failed:", w->path);
        // Release the slot we just claimed -- otherwise after 8 failed
        // watch attempts the table is full and no further watches can
        // be added. _alloc_watch incremented _count but the slot stays
        // active=FALSE so tick() ignores it; we just need to roll the
        // count back.
        _hot_reload_internal__count--;
        return FALSE;
    }
    uint hash = _hot_reload_internal__fnv1a((ubyte*)buf, size);
    GUI_FREE(buf);

    gui_node_t* tree = parser_xml__load_ui(w->path);
    if (tree == NULL_POINTER)
    {
        log_error("%s %s", "initial ui parse failed:", w->path);
        _hot_reload_internal__count--;
        return FALSE;
    }

    w->last_hash = hash;
    w->tree      = tree;
    w->active    = TRUE;
    scene__set_root(tree);

    log_info("%s %s", "watching ui:", w->path);
    return TRUE;
}

/**
 * @brief begin watching a .style file. Performs the initial load + style registration synchronously and stores the file's content hash.
 *
 * @param char* path -> Absolute path to the .style file.
 *
 * @return boole -> TRUE if the initial load succeeded; FALSE otherwise. Subsequent reloads call scene__clear_styles before reparsing, so removing a rule from the file actually un-registers it.
 */
boole hot_reload__watch_style(char* path)
{
    _hot_reload_internal__watch_t* w = _hot_reload_internal__alloc_watch(path, _HOT_RELOAD_INTERNAL__KIND_STYLE);
    if (w == NULL_POINTER)
    {
        return FALSE;
    }

    int64 size = 0;
    char* buf  = fs__read_entire_file(w->path, &size);
    if (buf == NULL_POINTER)
    {
        log_error("%s %s", "initial read failed:", w->path);
        // Same slot-release as watch_ui above. Without this, repeated
        // failed watch attempts would exhaust the 8-slot table.
        _hot_reload_internal__count--;
        return FALSE;
    }
    uint hash = _hot_reload_internal__fnv1a((ubyte*)buf, size);
    GUI_FREE(buf);

    // Initial parse MAY fail (typo, missing property, etc.). We used
    // to treat that as fatal and refuse to watch -- which meant a
    // single typo forced a full app restart once the user fixed it.
    // Now we arm the watcher regardless: parse errors are logged,
    // the watcher stays live, and the next file change triggers a
    // re-parse (which may succeed and apply styles). `last_hash` is
    // set to the current file's hash so the very-next tick doesn't
    // spuriously re-parse an unchanged (still-broken) file.
    if (parser_style__load_styles(w->path) == FALSE)
    {
        log_error("%s %s %s", "initial style parse failed:", w->path, "(watcher still armed -- fix the file and save)");
    }

    w->last_hash = hash;
    w->active    = TRUE;

    log_info("%s %s", "watching style:", w->path);
    return TRUE;
}

// Bump count whenever a real reload (.ui or .style) lands. Read +
// reset by hot_reload__tick so callers can detect "did anything
// change this tick?" without subscribing to a callback.
static int _hot_reload_internal__reloads_pending = 0;

/**
 * @brief poll every watched file once. For each file whose content hash differs from the previous tick, run the appropriate reload. Cheap when nothing changed; safe to call every frame. Returns the number of reloads that landed during this tick (0 on a quiet tick, >=1 if any .ui or .style was reparsed). Hosts whose platform backend uses render-gating (see Phase 2 in platform_linux_wayland_client.c) use the return to call platform_wayland__request_render() so the reloaded content becomes visible without waiting for the next input event. Existing call sites that don't care about the count can ignore the return -- it's a strict superset of the previous void signature.
 *
 * @return int -> Count of reloads landed this tick (0 = nothing changed). Transient open / parse failures are silently swallowed.
 */
int hot_reload__tick(void)
{
    // throttle: at most one filesystem sweep per POLL_INTERVAL_MS.
    // the previous implementation polled every frame which works but
    // hits the disk 60+ times a second for no benefit -- 1 hz is well
    // below human reaction time and keeps disk activity invisible.
    uint64 now = _hot_reload_internal__now_ms();
    if (now - _hot_reload_internal__last_poll_ms < _HOT_RELOAD_INTERNAL__POLL_INTERVAL_MS)
    {
        return 0;
    }
    _hot_reload_internal__last_poll_ms = now;

    for (int64 i = 0; i < _hot_reload_internal__count; i++)
    {
        _hot_reload_internal__watch_t* w = &_hot_reload_internal__watches[i];
        if (w->active == FALSE)
        {
            continue;
        }
        _hot_reload_internal__poll_one(w);
    }

    int n = _hot_reload_internal__reloads_pending;
    _hot_reload_internal__reloads_pending = 0;
    return n;
}

/**
 * @brief free any owned trees (from .ui watches) and clear the watch table.
 *
 * @return void
 */
void hot_reload__shutdown(void)
{
    for (int64 i = 0; i < _hot_reload_internal__count; i++)
    {
        _hot_reload_internal__watch_t* w = &_hot_reload_internal__watches[i];
        if (w->kind == _HOT_RELOAD_INTERNAL__KIND_UI && w->tree != NULL_POINTER)
        {
            scene__node_free(w->tree);
        }
    }
    stdlib__memset(_hot_reload_internal__watches, 0, sizeof(_hot_reload_internal__watches));
    _hot_reload_internal__count        = 0;
    _hot_reload_internal__last_poll_ms = 0;
    scene__clear_styles();
}

// ===== watch helpers ========================================================

// reserve a slot in the table, copy the path, set the kind. returns
// NULL if the table is full or the path is too long.
static _hot_reload_internal__watch_t* _hot_reload_internal__alloc_watch(char* path, _hot_reload_internal__kind_e kind)
{
    if (_hot_reload_internal__count >= _HOT_RELOAD_INTERNAL__MAX_WATCHES)
    {
        log_error("%s %d %s", "watch table full (max", _HOT_RELOAD_INTERNAL__MAX_WATCHES, ")");
        return NULL_POINTER;
    }
    _hot_reload_internal__watch_t* w = &_hot_reload_internal__watches[_hot_reload_internal__count];
    stdlib__memset(w, 0, sizeof(*w));

    size_t n = stdlib__strlen(path);
    if (n >= sizeof(w->path))
    {
        log_error("%s %s", "path too long:", path);
        return NULL_POINTER;
    }
    stdlib__memcpy(w->path, path, n);
    w->path[n] = 0;
    w->kind    = kind;

    _hot_reload_internal__count++;
    return w;
}

// read + hash + (if changed) reload one watch.
static void _hot_reload_internal__poll_one(_hot_reload_internal__watch_t* w)
{
    int64 size = 0;
    char* buf  = fs__read_entire_file(w->path, &size);
    if (buf == NULL_POINTER)
    {
        // transient open failure (editor briefly holding the file open
        // for write, antivirus scan, etc.). silently retry next tick.
        return;
    }
    uint hash = _hot_reload_internal__fnv1a((ubyte*)buf, size);
    GUI_FREE(buf);

    if (hash == w->last_hash)
    {
        return;
    }

    switch (w->kind)
    {
        case _HOT_RELOAD_INTERNAL__KIND_UI:
        {
            _hot_reload_internal__reload_ui(w);
            break;
        }
        case _HOT_RELOAD_INTERNAL__KIND_STYLE:
        {
            _hot_reload_internal__reload_style(w);
            break;
        }
    }

    // only update the stored hash if the reload reported success
    // (reload functions set w->last_hash themselves on success). this
    // means a parse failure leaves the old hash in place so the next
    // change retries automatically.
}

// .ui reload: reparse, swap in the new tree, free the old one.
// on parse failure, keep the previous tree and DO NOT update the
// stored hash so the next tick will retry.
static void _hot_reload_internal__reload_ui(_hot_reload_internal__watch_t* w)
{
    gui_node_t* fresh = parser_xml__load_ui(w->path);
    if (fresh == NULL_POINTER)
    {
        log_error("%s", "ui parse failed; keeping previous tree");
        return;
    }

    // Snapshot the focused node's id (if any) BEFORE we touch the
    // tree. After reconcile + set_root + free, we re-find the node
    // with that id in the new tree and re-focus it so typing into
    // an <input> survives a hot reload. Without this the focused
    // pointer becomes dangling the moment the old tree is freed.
    char focus_id[64];
    focus_id[0] = 0;
    gui_node_t* focused = scene__focus();
    if (focused != NULL_POINTER && focused->id[0] != 0)
    {
        size_t n = stdlib__strlen(focused->id);
        if (n >= sizeof(focus_id))
        {
            n = sizeof(focus_id) - 1;
        }
        stdlib__memcpy(focus_id, focused->id, n);
        focus_id[n] = 0;
    }

    gui_node_t* old = w->tree;

    // Transfer runtime state (scroll / value / text / is_open /
    // user_data) from matching nodes in `old` into `fresh`. After
    // this call, `old`'s user_data pointers on matched nodes are
    // NULLed so the subsequent scene__node_free(old) is safe (each
    // widget's on_destroy runs but sees NULL and no-ops).
    if (old != NULL_POINTER)
    {
        scene__reconcile_tree(old, fresh);
    }

    w->tree = fresh;
    scene__set_root(fresh);

    // Safe to free the old tree now that state has been moved.
    if (old != NULL_POINTER)
    {
        scene__node_free(old);
    }

    // Re-focus. If the previously focused node had an id that
    // still exists in the new tree, it becomes focus again. If
    // not, focus is dropped silently.
    if (focus_id[0] != 0)
    {
        gui_node_t* refound = scene__find_by_id(focus_id);
        if (refound != NULL_POINTER)
        {
            scene__set_focus(refound);
        }
    }

    int64 size = 0;
    char* buf  = fs__read_entire_file(w->path, &size);
    if (buf != NULL_POINTER)
    {
        w->last_hash = _hot_reload_internal__fnv1a((ubyte*)buf, size);
        GUI_FREE(buf);
    }

    log_info("%s %s", "reloaded ui:", w->path);
    _hot_reload_internal__reloads_pending++;
}

// .style reload: clear the entire style table and reparse. note that
// if the host has multiple .style watches this wipes all rules from
// all of them; the next tick will re-register the others on their
// normal cadence (their hashes haven't changed so they won't, until
// we add a re-apply primitive). for the poc with one .style watch
// this is correct.
static void _hot_reload_internal__reload_style(_hot_reload_internal__watch_t* w)
{
    scene__clear_styles();
    if (parser_style__load_styles(w->path) == FALSE)
    {
        log_error("%s", "style parse failed; styles may be partial");
        return;
    }

    int64 size = 0;
    char* buf  = fs__read_entire_file(w->path, &size);
    if (buf != NULL_POINTER)
    {
        w->last_hash = _hot_reload_internal__fnv1a((ubyte*)buf, size);
        GUI_FREE(buf);
    }

    log_info("%s %s", "reloaded style:", w->path);
    _hot_reload_internal__reloads_pending++;
}

// ===== fnv-1a 32-bit over a byte range ======================================
// distinct from scene__hash_name (which is fnv-1a over a null-terminated
// string). this one takes an explicit length so it's safe on arbitrary
// file content and won't stop early at a 0 byte.

static uint _hot_reload_internal__fnv1a(ubyte* data, int64 len)
{
    uint hash = 2166136261u;
    for (int64 i = 0; i < len; i++)
    {
        hash ^= (uint)data[i];
        hash *= 16777619u;
    }
    return hash;
}
