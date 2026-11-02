// scene.c - retained scene graph orchestration: tree primitives, style
// registration + resolution, layout coordination, hit test, input
// state machine, handler dispatch.
// all type-aware behavior (per-widget layout, draw, attribute parsing,
// drag handling) moved to gui/src/widgets/widget_<name>.c. scene.c
// never switches on gui_node_type_e any more -- it dispatches through
// the widget vtable retrieved via widget_registry__get.
// no os calls here; everything in this file is portable c.

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "motion.h"
#include "compositor.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "renderer.h"
#include "scene.h"
#include "scene_text.h"
#include "scene_style.h"
#include "scene_render.h"
#include "scene_input.h"
#include "widgets/widget_navigation.h"
#include "scroll.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// ===== forward declarations of file-scope statics ===========================
// Project convention: every static helper used inside this TU is
// forward-declared here so any function defined later can refer to
// it regardless of textual order. The actual definitions live near
// the file-scope state they touch.

static void _scene_internal__on_node_freed(gui_node_t* node);

// ===== color constructors ===================================================

/**
 * @brief build an rgba color from four floats in 0..1.
 *
 * @param float r -> Red component.
 * @param float g -> Green component.
 * @param float b -> Blue component.
 * @param float a -> Alpha component.
 *
 * @return gui_color_t -> The composed color value.
 */
gui_color_t scene__rgba(float r, float g, float b, float a)
{
    gui_color_t c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

/**
 * @brief build an rgb color from three floats in 0..1; alpha defaults to 1.0.
 *
 * @param float r -> Red component.
 * @param float g -> Green component.
 * @param float b -> Blue component.
 *
 * @return gui_color_t -> The composed color value, fully opaque.
 */
gui_color_t scene__rgb(float r, float g, float b)
{
    return scene__rgba(r, g, b, 1.0f);
}

/**
 * @brief build an rgb color from a packed 0xRRGGBB integer; alpha defaults to 1.0.
 *
 * @param uint rrggbb -> 24-bit color, red in the high byte.
 *
 * @return gui_color_t -> The composed color value, fully opaque.
 */
gui_color_t scene__hex(uint rrggbb)
{
    float r = (float)((rrggbb >> 16) & 0xFFu) / 255.0f;
    float g = (float)((rrggbb >>  8) & 0xFFu) / 255.0f;
    float b = (float)((rrggbb      ) & 0xFFu) / 255.0f;
    return scene__rgba(r, g, b, 1.0f);
}

// ===== fnv-1a 32-bit =========================================================
// used by the handler registration table and the .ui parser to hash
// on_click="name" / on_change="name" once at parse time.

/**
 * @brief compute the fnv-1a 32-bit hash of a handler-name string. parser_xml uses this to set on_click_hash at parse time so dispatch is a hash compare.
 *
 * @param char* name -> Null-terminated string to hash.
 *
 * @return uint -> 32-bit fnv-1a hash.
 */
uint scene__hash_name(char* name)
{
    uint hash = 2166136261u;
    const ubyte* p = (const ubyte*)name;
    while (*p != 0)
    {
        hash ^= (uint)(*p);
        hash *= 16777619u;
        p++;
    }
    return hash;
}

// ===== handler registration table ===========================================

#define _SCENE_INTERNAL__MAX_HANDLERS 256

/**
 *one row of the handler registration table. parser_xml computes the
 *fnv-1a hash once at parse time; dispatch checks the full name on a hash hit.
 */
typedef struct _scene_internal__handler_entry_t
{
    uint           hash;
    char           name[64];
    gui_handler_fn fn;
} _scene_internal__handler_entry_t;

static _scene_internal__handler_entry_t _scene_internal__handlers[_SCENE_INTERNAL__MAX_HANDLERS];
static int64                          _scene_internal__handler_count = 0;

/**
 * @brief register a click handler under the given name. parser_xml resolves on_click="foo" by hashing "foo" and looking it up in the handler table.
 *
 * @param char* name -> Source-level name of the handler (typically the function's identifier).
 * @param gui_handler_fn fn -> Function pointer invoked when an event matches.
 *
 * @return void
 */
void scene__register_handler(char* name, gui_handler_fn fn)
{
    if (name == NULL_POINTER || name[0] == 0 || stdlib__strlen(name) >= (int64)sizeof(_scene_internal__handlers[0].name) || fn == NULL_POINTER)
    {
        log_error("%s", "scene__register_handler: expected a nonempty name of at most 63 bytes and a callback");
        return;
    }
    uint hash = scene__hash_name(name);
    for (int64 i = 0; i < _scene_internal__handler_count; i++)
    {
        if (_scene_internal__handlers[i].hash == hash && stdlib__strcmp(_scene_internal__handlers[i].name, name) == 0)
        {
            _scene_internal__handlers[i].fn = fn;
            return;
        }
    }
    if (_scene_internal__handler_count >= _SCENE_INTERNAL__MAX_HANDLERS)
    {
        log_error("%s", "scene__register_handler: table full");
        return;
    }
    _scene_internal__handler_entry_t* e = &_scene_internal__handlers[_scene_internal__handler_count++];
    e->hash = hash;
    e->fn   = fn;

    size_t n = stdlib__strlen(name);
    if (n >= sizeof(e->name))
    {
        n = sizeof(e->name) - 1;
    }
    stdlib__memcpy(e->name, name, n);
    e->name[n] = 0;
}

static gui_handler_fn _scene_internal__lookup_handler(uint hash, const char* name)
{
    if (name == NULL_POINTER) { return NULL_POINTER; }
    for (int64 i = 0; i < _scene_internal__handler_count; i++)
    {
        if (_scene_internal__handlers[i].hash == hash && stdlib__strcmp(_scene_internal__handlers[i].name, name) == 0)
        {
            return _scene_internal__handlers[i].fn;
        }
    }
    return NULL_POINTER;
}

// host symbol resolver. installed by the platform layer (Win32:
// GetProcAddress against the host exe). NULL means "no fallback".
static gui_symbol_resolver_fn _scene_internal__symbol_resolver = NULL_POINTER;

/**
 * @brief install a host-symbol resolver. The resolver is invoked when the registration table has no entry for a click target's hash; on success the result is cached back into the table so subsequent dispatches skip the resolver entirely.
 *
 * @param gui_symbol_resolver_fn resolver -> Function that maps a handler name to a function pointer, or NULL to disable the fallback.
 *
 * @return void
 */
void scene__set_symbol_resolver(gui_symbol_resolver_fn resolver)
{
    _scene_internal__symbol_resolver = resolver;
}

// try the registration table first; on miss, ask the host resolver
// and cache the result so subsequent lookups don't pay the syscall.
static gui_handler_fn _scene_internal__resolve_handler(uint hash, char* name)
{
    gui_handler_fn fn = _scene_internal__lookup_handler(hash, name);
    if (fn != NULL_POINTER)
    {
        return fn;
    }
    if (_scene_internal__symbol_resolver == NULL_POINTER || name == NULL_POINTER || name[0] == 0)
    {
        return NULL_POINTER;
    }
    fn = _scene_internal__symbol_resolver(name);
    if (fn != NULL_POINTER)
    {
        scene__register_handler(name, fn);
    }
    return fn;
}

// ===== tree construction ====================================================

/**
 * @brief allocate a zero-initialised node of the given type.
 *
 * @param gui_node_type_e type -> Node kind.
 *
 * @return gui_node_t* -> Newly allocated node, or NULL on allocation failure. The caller must eventually call scene__node_free on the root of the subtree the node belongs to.
 */
gui_node_t* scene__node_new(gui_node_type_e type)
{
    gui_node_t* n = (gui_node_t*)GUI_CALLOC_T(1, sizeof(gui_node_t), MM_TYPE__NODE);
    if (n == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    n->text = n->text_inline;
    n->text_capacity = sizeof(n->text_inline);
    n->text_limit = MEEK_UI_MAX_TEXT_BYTES;
    n->type  = type;
    n->state = GUI_NODE_STATE__DEFAULT;
    return n;
}

/**
 * @brief append child to parent's child list and link parent <-> child.
 *
 * @param gui_node_t* parent -> Parent node.
 * @param gui_node_t* child -> Node to append. Must not already be in any tree.
 *
 * @return void
 */
void scene__add_child(gui_node_t* parent, gui_node_t* child)
{
    if (parent == NULL_POINTER || child == NULL_POINTER)
    {
        return;
    }
    child->parent       = parent;
    child->next_sibling = NULL_POINTER;

    if (parent->first_child == NULL_POINTER)
    {
        parent->first_child = child;
        parent->last_child  = child;
    }
    else
    {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
    parent->child_count++;

    // Fire on_attach AFTER the link is established so the hook can
    // walk up via child->parent. Optional; widgets that don't care
    // leave the slot NULL.
    const widget_vtable_t* vt = widget_registry__get(child->type);
    if (vt != NULL_POINTER && vt->on_attach != NULL_POINTER)
    {
        vt->on_attach(child, parent);
    }
}

/**
 * @brief lazy-alloc accessor for per-node widget state. If the widget's vtable declares state_size > 0 and node->user_data is NULL, this allocates `state_size` zero-initialized bytes via GUI_CALLOC_T(MM_TYPE__GENERIC) and stashes the pointer on node->user_data. Subsequent calls return the cached pointer. scene__node_free auto-frees this allocation AFTER the widget's on_destroy returns. Widgets that prefer to manage user_data themselves can leave state_size = 0 and continue using the legacy `_widget_X_internal__state_of` pattern.
 *
 * @param gui_node_t* n -> The node.
 *
 * @return void* -> Pointer to the state struct, or NULL if vtable->state_size is 0 or allocation failed.
 */
void* scene__widget_state(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    if (n->user_data != NULL_POINTER)
    {
        return n->user_data;
    }

    const widget_vtable_t* vt = widget_registry__get(n->type);
    if (vt == NULL_POINTER || vt->state_size <= 0)
    {
        return NULL_POINTER;
    }

    n->user_data = GUI_CALLOC_T(1, (size_t)vt->state_size, MM_TYPE__GENERIC);
    return n->user_data;
}

/**
 * @brief wire a click handler onto a node by name. The name is hashed once here; the handler must also be registered via scene__register_handler or exported from the host exe with UI_HANDLER for auto-discovery.
 *
 * @param gui_node_t* node -> Target node.
 * @param char* handler_name -> Handler name to look up at dispatch.
 *
 * @return void
 */
void scene__set_on_click(gui_node_t* node, char* handler_name)
{
    if (node == NULL_POINTER)
    {
        return;
    }
    if (handler_name == NULL_POINTER || handler_name[0] == 0)
    {
        node->on_click_hash = 0;
        node->on_click_name[0] = 0;
        return;
    }
    size_t n = stdlib__strlen(handler_name);
    if (n >= sizeof(node->on_click_name))
    {
        log_warn("%s", "on_click name exceeds 63 bytes; binding unchanged");
        return;
    }
    node->on_click_hash = scene__hash_name(handler_name);
    stdlib__memcpy(node->on_click_name, handler_name, n);
    node->on_click_name[n] = 0;
}

/**
 * @brief wire a change handler onto a node by name. Used for sliders (and any future widget that emits GUI_EVENT_TYPE__CHANGE).
 *
 * @param gui_node_t* node -> Target node.
 * @param char* handler_name -> Handler name to look up at dispatch.
 *
 * @return void
 */
void scene__set_on_change(gui_node_t* node, char* handler_name)
{
    if (node == NULL_POINTER)
    {
        return;
    }
    if (handler_name == NULL_POINTER || handler_name[0] == 0)
    {
        node->on_change_hash = 0;
        node->on_change_name[0] = 0;
        return;
    }
    size_t n = stdlib__strlen(handler_name);
    if (n >= sizeof(node->on_change_name))
    {
        log_warn("%s", "on_change name exceeds 63 bytes; binding unchanged");
        return;
    }
    node->on_change_hash = scene__hash_name(handler_name);
    stdlib__memcpy(node->on_change_name, handler_name, n);
    node->on_change_name[n] = 0;
}

// Per-node teardown shared between the iterative + recursive paths
// of scene__node_free. Clears any file-scope pointers that may
// reference `n` (overlay / root / focus / pressed / hover), runs
// the widget's on_destroy hook, and frees auto-allocated user_data.
// Does NOT free `n` itself or recurse into children.
static void _scene_internal__node_teardown_one(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    // Defensive clear of every file-scope slot that might point at
    // this node. Each clear is independent so a node that's both,
    // say, the focused AND overlay owner gets BOTH slots NULLed.
    _scene_internal__on_node_freed(n);
    motion__forget(n);
    // Let the widget release any per-node heap / GPU resources before
    // we drop user_data + the node itself. Dispatch through the vtable
    // so widget_image's texture, widget_select's popup state, etc. all
    // get proper cleanup.
    const widget_vtable_t* vt = widget_registry__get(n->type);
    if (vt != NULL_POINTER && vt->on_destroy != NULL_POINTER)
    {
        vt->on_destroy(n);
    }
    // Auto-free user_data IF the widget opted into auto-alloc via
    // vtable->state_size. on_destroy had its chance to release
    // sub-resources stashed inside; now drop the outer allocation.
    // on_destroy is allowed to NULL user_data itself (legacy widgets
    // do this), in which case the auto-free becomes a no-op.
    if (vt != NULL_POINTER && vt->state_size > 0 && n->user_data != NULL_POINTER)
    {
        GUI_FREE(n->user_data);
        n->user_data = NULL_POINTER;
    }
    scene_text__release(n);
}

// Recursive fallback used only when the iterative path can't allocate
// its work list. Same semantics as the historical recursive
// implementation; depth is bounded by the C stack, so a pathological
// 100k-deep tree could blow it -- but if we got here, the system is
// already out of memory, and recursion at least makes forward progress.
static void _scene_internal__node_free_recursive(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    _scene_internal__node_teardown_one(n);
    gui_node_t* c = n->first_child;
    while (c != NULL_POINTER)
    {
        gui_node_t* next = c->next_sibling;
        _scene_internal__node_free_recursive(c);
        c = next;
    }
    GUI_FREE(n);
}

/**
 * @brief recursively free a subtree rooted at node.
 *
 * @param gui_node_t* node -> Root of the subtree (may be NULL).
 *
 * @return void
 */
void scene__node_free(gui_node_t* node)
{
    if (node == NULL_POINTER)
    {
        return;
    }
    // Iterative free so deeply nested trees can't overflow the C
    // stack. Three-phase:
    // Phase 1: walk the tree level-order, collecting every node
    // pointer into a dynamic array. PURE collection --
    // no teardown, no free. The walk uses first_child /
    // next_sibling, which we don't touch.
    // Phase 2: iterate the collected list once, calling per-node
    // teardown (on_destroy + user_data free) on each.
    // Order doesn't matter for correctness; we go in
    // collected order (parents-before-children) to
    // match the historical recursive contract.
    // Phase 3: reverse-iterate the list and GUI_FREE each node
    // so children's allocations get returned before
    // their parents'.
    // Splitting collection from teardown means the realloc-failure
    // recovery path is straightforward: we know exactly which nodes
    // are in the list (none teardown'd yet) and which still need
    // recursive handling (the un-collected suffix of `n`'s child
    // chain). No risk of double-teardown.
    // Falls back to recursion entirely if the initial allocation
    // fails -- recursion uses C stack but at least makes forward
    // progress, which is what matters when memory is already
    // exhausted.
    int64       cap   = 64;
    gui_node_t**  list  = (gui_node_t**)GUI_MALLOC_T(cap* sizeof(gui_node_t*), MM_TYPE__GENERIC);
    if (list == NULL_POINTER)
    {
        _scene_internal__node_free_recursive(node);
        return;
    }
    int64       count          = 0;
    int64       visit_idx      = 0;
    gui_node_t*   uncollected    = NULL_POINTER; // first un-collected sibling, or NULL.
    list[count++] = node;

    while (visit_idx < count && uncollected == NULL_POINTER)
    {
        gui_node_t* n = list[visit_idx++];
        for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
        {
            if (count >= cap)
            {
                int64       new_cap  = cap * 2;
                gui_node_t**  new_list = (gui_node_t**)GUI_REALLOC(list, new_cap* sizeof(gui_node_t*));
                if (new_list == NULL_POINTER)
                {
                    // Grow failed. Stop collecting. Remember `c` so
                    // phase-2.5 can recursively free it and its
                    // later siblings -- those subtrees never made
                    // it into the list, so handling them via the
                    // recursive helper (which does its own
                    // teardown + free) keeps the bookkeeping clean.
                    uncollected = c;
                    break;
                }
                list = new_list;
                cap  = new_cap;
            }
            list[count++] = c;
        }
    }

    // Phase 2: per-node teardown. on_destroy hooks may inspect
    // first_child / next_sibling to walk children -- those pointers
    // are still valid here because phase 3 hasn't run yet, and the
    // un-collected siblings (if any) haven't been freed yet either.
    for (int64 i = 0; i < count; i++)
    {
        _scene_internal__node_teardown_one(list[i]);
    }

    // Phase 2.5 (only if grow failed): recursively free the
    // un-collected suffix of children. Walks via next_sibling so
    // we visit ALL un-collected siblings, not just `c`'s subtree.
    // recursive_free does its own teardown + free.
    while (uncollected != NULL_POINTER)
    {
        gui_node_t* next = uncollected->next_sibling;
        _scene_internal__node_free_recursive(uncollected);
        uncollected = next;
    }

    // Phase 3: reverse-iterate the list so children are freed
    // before their parents. Matches the historical recursive
    // ordering for any external observer (memory-tracker logs,
    // leak-sweep tools, etc.).
    for (int64 i = count - 1; i >= 0; i--)
    {
        GUI_FREE(list[i]);
    }
    GUI_FREE(list);
}

// ===== tree reconciliation for hot reload ====================================
// hot_reload__reload_ui parses the new .ui file and wants to swap
// it in without resetting every stateful interaction. The naive
// approach (free old, install new) loses scroll position, open
// dropdowns, slider values, text input contents, canvas pixels,
// image textures -- everything on user_data. scene__reconcile_tree
// fixes that by walking both trees in parallel, matching nodes by
// id (or a path-based fallback), and moving runtime state from
// old to new in place.
// Matching strategy:
// 1. Collect every node in the OLD tree that has a non-empty id
// into a small flat array keyed by id hash. O(n) build, O(1)
// lookup during the walk.
// 2. Walk the NEW tree. For each node with an id, try the hash
// table. If hit AND types match, transfer state.
// 3. For id-less nodes, recurse pairwise: if both parents matched
// and the child at the same sibling index has the same type,
// transfer state. This is best-effort; a child reordering
// breaks the match for that subtree.
// State moved:
// - scroll_x, scroll_y, scroll_y_target, scroll_drag_* -- scroll
// position survives a .ui edit.
// - value, value_min, value_max -- slider / checkbox / select
// selection.
// - is_open -- select dropdowns stay open, popup stays open.
// - text, text_len, appear_age_ms -- input / textarea content
// survives; appear-animation doesn't replay.
// - user_data -- widget-specific heap state (image texture,
// canvas pixels, colorpicker HSV, select items, etc.). After
// transfer, old->user_data is NULLed so the old node's
// on_destroy becomes a no-op instead of freeing the resource
// we just handed to the new node.

#define _SCENE_INTERNAL__RECONCILE_MAP_CAP 256

typedef struct _scene_internal__recon_entry_t
{
    uint       id_hash;    // 0x0
    gui_node_t*  old_node;   // 0x8
} _scene_internal__recon_entry_t;

static void _scene_internal__recon_collect_ids(gui_node_t* n, _scene_internal__recon_entry_t* map, int64* map_count)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    if (n->id[0] != 0 && *map_count < _SCENE_INTERNAL__RECONCILE_MAP_CAP)
    {
        map[*map_count].id_hash  = scene__hash_name(n->id);
        map[*map_count].old_node = n;
        (*map_count)++;
    }
    gui_node_t* c = n->first_child;
    while (c != NULL_POINTER)
    {
        _scene_internal__recon_collect_ids(c, map, map_count);
        c = c->next_sibling;
    }
}

static gui_node_t* _scene_internal__recon_find(_scene_internal__recon_entry_t* map, int64 count, uint id_hash)
{
    for (int64 i = 0; i < count; i++)
    {
        if (map[i].id_hash == id_hash)
        {
            return map[i].old_node;
        }
    }
    return NULL_POINTER;
}

// Recursive id-walk used by scene__reconcile_tree. Visits every
// node in the new subtree; for each one that has an id, looks it
// up in the old-tree map and transfers state. The root is matched
// by the caller before this fires (and skipped here via the
// `root` parameter compare) so we don't double-transfer.
static void _scene_internal__recon_transfer(gui_node_t* old, gui_node_t* new_n);

static void _scene_internal__recon_walk_ids(gui_node_t* n, gui_node_t* root, _scene_internal__recon_entry_t* map, int64 map_count)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    if (n->id[0] != 0 && n != root)
    {
        uint h = scene__hash_name(n->id);
        gui_node_t* matched = _scene_internal__recon_find(map, map_count, h);
        if (matched != NULL_POINTER)
        {
            _scene_internal__recon_transfer(matched, n);
        }
    }
    gui_node_t* c = n->first_child;
    while (c != NULL_POINTER)
    {
        _scene_internal__recon_walk_ids(c, root, map, map_count);
        c = c->next_sibling;
    }
}

// Copy runtime state from `old` to `new` in place. Both pointers
// must be non-NULL, the nodes must be of the same type, and the
// caller takes responsibility for not double-freeing old's
// user_data (we NULL it here so on_destroy becomes safe).
static void _scene_internal__recon_transfer(gui_node_t* old, gui_node_t* new_n)
{
    if (old == NULL_POINTER || new_n == NULL_POINTER)
    {
        return;
    }
    if (old->type != new_n->type)
    {
        return;
    }

    new_n->scroll_x                 = old->scroll_x;
    new_n->scroll_y                 = old->scroll_y;
    new_n->scroll_y_target          = old->scroll_y_target;
    new_n->scroll_drag_axis         = old->scroll_drag_axis;
    new_n->scroll_drag_mouse_start  = old->scroll_drag_mouse_start;
    new_n->scroll_drag_scroll_start = old->scroll_drag_scroll_start;

    // Value fields: slider / checkbox / select / colorpicker all
    // stash their interactable value here. Transfer unconditionally
    // -- if the .ui file re-declared `value="0.5"` the parser set
    // that into `new_n->value`, and we overwrite it with the old
    // runtime value. That's the desired behavior for hot reload:
    // the user's current slider position survives a style tweak.
    new_n->value     = old->value;
    new_n->value_min = old->value_min;
    new_n->value_max = old->value_max;

    new_n->is_open         = old->is_open;
    new_n->appear_age_ms   = old->appear_age_ms; // don't replay appear animation
    new_n->prev_visibility = old->prev_visibility;
    new_n->prev_display    = old->prev_display;

    // Text: only preserve for editable widgets (<input>, <textarea>).
    // Otherwise a hot reload that changed `text="..."` on a <label>
    // or <button> in the .ui would silently lose its update because
    // the runtime-side text would clobber the parser-loaded text.
    // Editable widgets are the inverse: their text IS the runtime
    // state, and the parser doesn't override it. Restricting the
    // transfer to those types gets both right.
    if (new_n->type == GUI_NODE_TYPE__INPUT || new_n->type == GUI_NODE_TYPE__TEXTAREA)
    {
        // Move ownership: even under memory pressure the user's draft survives.
        // Empty drafts must also replace the new document's initial text.
        scene_text__move(new_n, old);
    }

    // user_data transfer policy. Two modes, per-widget vtable flag:
    // preserve_user_data = TRUE (canvas, colorpicker, ...):
    // OLD's user_data takes over the new node, parser-allocated
    // user_data on the new node is freed first via on_destroy.
    // Runtime state (canvas pixels, picked color) survives the
    // reload at the cost of declarative state changes being
    // ignored.
    // preserve_user_data = FALSE (default; popup, image, ...):
    // New's parser-allocated user_data wins. Old's gets freed
    // when the old tree is torn down (on_destroy, which sees
    // old->user_data still set and releases it normally). A
    // hot-reload edit to `type="..."` / `src="..."` etc. takes
    // effect immediately at the cost of losing any runtime
    // state the widget had built up there.
    const widget_vtable_t* vt = widget_registry__get(new_n->type);
    stdlib__strcpy_safe(new_n->navigation_focus, sizeof(new_n->navigation_focus), old->navigation_focus);
    if (new_n->type == GUI_NODE_TYPE__NAVIGATION)
    {
        widget_navigation__reconcile(old, new_n);
    }
    if (vt != NULL_POINTER && vt->preserve_user_data)
    {
        if (new_n->user_data != NULL_POINTER)
        {
            // Drop the parser-allocated state on the new node first
            // so it's not leaked when we overwrite the slot below.
            // on_destroy handles widget-specific cleanup (releasing
            // GPU textures, free'ing sub-allocations, etc.).
            if (vt->on_destroy != NULL_POINTER)
            {
                vt->on_destroy(new_n);
            }
            // on_destroy may have set user_data = NULL itself;
            // belt-and-suspenders to ensure no double-free below.
            new_n->user_data = NULL_POINTER;
        }
        new_n->user_data = old->user_data;
        old->user_data   = NULL_POINTER;
    }
    // The default branch leaves new_n->user_data alone (parser
    // allocated it). The old's user_data is NOT touched here -- it
    // gets released naturally when scene__node_free walks the old
    // tree.

    // Host-side overrides survive the transfer the same way. If
    // the host toggled a theme override on a node, the new tree's
    // corresponding node should start in that same themed state
    // rather than reverting to the raw .style color.
    new_n->background_color_override     = old->background_color_override;
    new_n->has_background_color_override = old->has_background_color_override;
    new_n->font_color_override           = old->font_color_override;
    new_n->has_font_color_override       = old->has_font_color_override;
    stdlib__memcpy(new_n->background_image_override, old->background_image_override, sizeof(new_n->background_image_override));
    new_n->has_background_image_override = old->has_background_image_override;
}

// Sibling-index pairwise transfer for the id-less case. Old and
// new must have already had their children counted; we iterate
// by index. If types diverge at some index we stop -- the trees
// have forked at that point and deeper matching would be wrong
// more often than right.
static void _scene_internal__recon_pair_siblings(gui_node_t* old, gui_node_t* new_n)
{
    if (old == NULL_POINTER || new_n == NULL_POINTER)
    {
        return;
    }

    gui_node_t* oc = old->first_child;
    gui_node_t* nc = new_n->first_child;
    while (oc != NULL_POINTER && nc != NULL_POINTER)
    {
        if (oc->type != nc->type)
        {
            break;
        }
        // Skip transfer if the new node has an id -- it'll be (or
        // has already been) matched via the id map; doing it here
        // too would be double-work and could race if we haven't
        // hit it in the id-walk yet.
        if (nc->id[0] == 0)
        {
            _scene_internal__recon_transfer(oc, nc);
        }
        _scene_internal__recon_pair_siblings(oc, nc);
        oc = oc->next_sibling;
        nc = nc->next_sibling;
    }
}

/**
 * @brief transfer runtime interaction state from `old_tree` into `new_tree` in place, matching nodes by stable key (id first, else parent path + type + sibling index). State that gets moved: scroll offset, slider value, is_open, text content, user_data (widget-specific state). After the call, `old_tree` still owns its topology but its user_data pointers on matched nodes are NULLed so the subsequent scene__node_free(old_tree) does not double-free. Use in hot reload: freeze the focused id, reparse, reconcile, scene__set_root(new), scene__node_free(old), re-focus by id. Without this, every hot reload resets scroll + slider + popup + input text to their .ui declaration, which is jarring in active dev workflows.
 *
 * @param gui_node_t* old_tree -> Tree about to be freed. May be NULL.
 * @param gui_node_t* new_tree -> Freshly parsed tree that takes over. May be NULL.
 *
 * @return void
 */
void scene__reconcile_tree(gui_node_t* old_tree, gui_node_t* new_tree)
{
    if (old_tree == NULL_POINTER || new_tree == NULL_POINTER)
    {
        return;
    }

    // Build the id -> old_node map. Capped at 256 ids; beyond that
    // we silently drop the later ones (unlikely in practice --
    // most .ui files have a dozen ids max).
    _scene_internal__recon_entry_t map[_SCENE_INTERNAL__RECONCILE_MAP_CAP];
    int64 map_count = 0;
    _scene_internal__recon_collect_ids(old_tree, map, &map_count);

    // Walk the new tree, matching by id first. For each id hit,
    // transfer state. id-less nodes piggyback on their parent's
    // subtree pairing below.
    // Match the root pair first (even if they have no ids; the
    // root is always the root). Then recurse.
    _scene_internal__recon_transfer(old_tree, new_tree);

    // Visit the new tree; every new node with an id looks itself
    // up in the old map and pulls state from there. Plain recursion
    // walks the whole tree regardless of breadth -- the previous
    // 256-slot fixed stack would silently drop ids past that bound
    // on wide trees.
    _scene_internal__recon_walk_ids(new_tree, new_tree, map, map_count);

    // Pairwise sibling walk for id-less nodes. Starts from the
    // root pair; each descent follows the subtree structure.
    // Skips nodes with ids (already matched above).
    _scene_internal__recon_pair_siblings(old_tree, new_tree);
}

// ===== root + input + scale =================================================

/**
 *current input state for the hover + press state machine.
 */

static gui_node_t*                     _scene_internal__root  = NULL_POINTER;

// global ui scale factor. multiplied into every dimension by widget
// layout functions and into corner radii at draw time. 1.0 = identity.
static float _scene_internal__scale = 1.0f;

/**
 * @brief set the active scene root. The previous root is not freed.
 *
 * @param gui_node_t* root -> New root node, or NULL to clear.
 *
 * @return void
 */
void scene__set_root(gui_node_t* root)
{
    _scene_internal__root = root;
}

/**
 * @brief get the currently set scene root.
 *
 * @return gui_node_t* -> Active root node, or NULL if none has been set.
 */
gui_node_t* scene__root(void)
{
    return _scene_internal__root;
}

/**
 * @brief set the global UI scale factor. All sizing, padding, gap, and corner radius values are multiplied by this factor before being committed to bounds (layout) or passed to the renderer (radius). A value of 1.0 is identity; 2.0 doubles every dimension.
 *
 * @param float scale -> New scale factor. Clamped to a sensible positive range internally.
 *
 * @return void
 */
void scene__set_scale(float scale)
{
    if (scale < 0.05f)
    {
        scale = 0.05f;
    }
    if (scale > 100.0f)
    {
        scale = 100.0f;
    }
    _scene_internal__scale = scale;
}

/**
 * @brief read the current global UI scale factor.
 *
 * @return float -> The currently-set scale factor (1.0 by default).
 */
float scene__scale(void)
{
    return _scene_internal__scale;
}

/**
 * @brief convert a logical (style-file) pixel count to physical pixels at the current UI scale. Equivalent to `logical * scene__scale()`; named helper so overlay-drawing code (widget_popup, widget_select dropdown, widget_keyboard) doesn't have to remember the multiplication on every dimension. The normal widget layout path already does this implicitly by taking `scale` as an argument; the helper is for code paths outside that pipeline.
 *
 * @param float logical -> Logical-pixel dimension (same units as .style values).
 *
 * @return float -> Physical-pixel dimension.
 */
float scene__px(float logical)
{
    return logical * _scene_internal__scale;
}

// ===== frame timing =========================================================
// Captured by the platform layer at the START of each tick via
// scene__begin_frame_time. Stable for the entire frame. The animator
// reads frame_delta_ms once per tick to advance per-node animation
// timers.
// Implementation note on the delta clamp: large gaps happen when the
// debugger is paused or the OS suspends the process. Without a clamp,
// resuming would fast-forward every running animation through its
// entire duration in a single frame -- the user would see every
// in-flight transition snap straight to its target. Capping at 100 ms
// bounds a single frame's animation advance to ~6 frames worth at
// 60 Hz, which is enough for legitimate frame-rate variation but not
// enough for a debugger pause to skip an animation.

static int64 _scene_internal__frame_time_ms     = 0;
static int64 _scene_internal__frame_prev_time   = 0;
static int64 _scene_internal__frame_delta_ms    = 0;
// Deterministic-clock override for the visual-regression runner.
// When _sim_step_ms > 0, scene__begin_frame_time ignores the
// platform's real wall-clock value and instead advances the scene
// clock by exactly _sim_step_ms each tick. Without this, different
// graphics backends present at different rates (vsync timing, swap
// interval differences), so "frame 8" means different elapsed ms on
// different backends, and animations captured at frame 8 end up at
// different points. Deterministic mode eliminates that variance:
// every backend sees the same animation progress.
static int64 _scene_internal__sim_step_ms       = 0;
static int64 _scene_internal__sim_clock_ms      = 0;

/**
 * @brief switch the scene clock into deterministic mode. When `step_ms` is > 0, scene__begin_frame_time IGNORES the wall-clock value it receives from the platform and instead advances by exactly `step_ms` each tick. When `step_ms` is 0, deterministic mode is off and the clock tracks real time. Used by the visual-regression test runner: different graphics backends present at different rates, so 8 frames of wall-clock time varies substantially across backends. Deterministic mode lets the runner say "every tick is 16 ms regardless of what the GPU / vsync did", so an animation captured at frame N lands on exactly the same progress on every backend. No effect on normal host apps (they don't call this).
 *
 * @param int64 step_ms -> Fixed ms increment per tick, or 0 to revert to wall-clock tracking.
 *
 * @return void
 */
void scene__set_deterministic_clock(int64 step_ms)
{
    _scene_internal__sim_step_ms  = step_ms;
    _scene_internal__sim_clock_ms = 0;
    // Reset the delta tracker so the first tick after switching
    // modes doesn't report a gigantic delta between the real clock
    // and the synthetic one.
    _scene_internal__frame_prev_time = 0;
}

/**
 * @brief stamp the start of a new frame with the current monotonic-clock time in milliseconds. Computes frame_delta_ms as (now - previous frame's now), clamping huge jumps (debugger pause, system sleep) so a resumed app doesn't fast-forward animations through to completion.
 *
 * @param int64 now_ms -> Current monotonic time in milliseconds.
 *
 * @return void
 */
void scene__begin_frame_time(int64 now_ms)
{
    // If deterministic mode is active, discard the platform's
    // wall-clock reading and substitute a monotonic synthetic clock
    // that advances by step_ms each call. Produces identical
    // animation-progress readings on every tick across every
    // backend, regardless of how long the real frame took.
    if (_scene_internal__sim_step_ms > 0)
    {
        _scene_internal__sim_clock_ms += _scene_internal__sim_step_ms;
        now_ms = _scene_internal__sim_clock_ms;
    }

    if (_scene_internal__frame_prev_time == 0)
    {
        // first frame: no previous timestamp, so delta is 0. animator
        // simply doesn't advance on frame 1, which is fine -- new
        // nodes start at appear_age_ms = 0 anyway.
        _scene_internal__frame_delta_ms = 0;
    }
    else
    {
        int64 delta = now_ms - _scene_internal__frame_prev_time;
        // clock went backwards (rare; system clock change).
        if (delta < 0)
        {
            delta = 0;
        }
        // clamp huge jumps (debugger pause, sleep).
        if (delta > 100)
        {
            delta = 100;
        }
        _scene_internal__frame_delta_ms = delta;
    }
    _scene_internal__frame_prev_time = now_ms;
    _scene_internal__frame_time_ms   = now_ms;
}

/**
 * @brief read the timestamp captured by the most recent scene__begin_frame_time call. Stable for the entire frame.
 *
 * @return int64 -> Monotonic milliseconds, or 0 before the first frame.
 */
int64 scene__frame_time_ms(void)
{
    return _scene_internal__frame_time_ms;
}

/**
 * @brief read the elapsed milliseconds since the previous frame's scene__begin_frame_time. Animation code multiplies this by their own speed parameters to advance state. Always positive; clamped to a sensible upper bound to avoid debugger-pause weirdness.
 *
 * @return int64 -> Milliseconds since previous frame, clamped to [0, 100].
 */
int64 scene__frame_delta_ms(void)
{
    return _scene_internal__frame_delta_ms;
}

// ===== overlay (top-most popup layer) =======================================
// Single-slot overlay used by widgets that need to draw ABOVE the rest
// of the tree (currently only <select>'s dropdown menu). The tree-walk
// draw order is depth-first depth, so a widget's emit_draws/_post fires
// at the widget's tree position -- meaning siblings drawn later cover
// it. The overlay path bypasses that: we record (node, bounds, draw_fn)
// here, scene__emit_draws calls draw_fn AFTER the normal walk is done,
// and scene__hit_test routes clicks inside bounds to the overlay node
// instead of whatever's spatially under the cursor.
// Only ONE overlay can be active at a time. Opening a second
// auto-closes the first (which matches how native OS dropdowns
// behave -- only ever one menu open).

static gui_node_t*           _scene_internal__overlay_node     = NULL_POINTER;
static gui_rect_t            _scene_internal__overlay_bounds   = { 0.0f, 0.0f, 0.0f, 0.0f };
static gui_overlay_draw_fn _scene_internal__overlay_draw_fn  = NULL_POINTER;

// Defensive clear of every file-scope slot that might point at a
// node about to be freed. Called from scene__node_free at the top
// of the destruction path so the next emit_draws / hit-test walk
// can't dereference dangling pointers. Each clear is independent
// so a node that was, say, both the overlay owner AND focused
// gets BOTH slots cleared.
// Definition lives down here (rather than next to scene__node_free
// up at line ~250) because every static it touches is declared in
// THIS region of the file. Forward declaration at the top of the
// TU lets scene__node_free call it.
static void _scene_internal__on_node_freed(gui_node_t* node)
{
    if (node == NULL_POINTER)
    {
        return;
    }

    if (_scene_internal__overlay_node == node)
    {
        _scene_internal__overlay_node    = NULL_POINTER;
        _scene_internal__overlay_draw_fn = NULL_POINTER;
    }
    if (_scene_internal__root == node)
    {
        _scene_internal__root = NULL_POINTER;
    }
    scene_input__on_node_freed(node);
}

/**
 * @brief open (or close, when node is NULL) a top-most overlay. Subsequent scene__hit_test calls return `node` for any cursor position inside `bounds`; scene__emit_draws calls `draw_fn(node, scale)` after the normal tree walk so the overlay paints on top of everything.
 *
 * @param gui_node_t* node -> Owning widget; passed back to draw_fn and returned by hit-test inside bounds. NULL closes the overlay.
 * @param gui_rect_t bounds -> Screen rect the overlay covers. Hit-test uses this to determine "is this click on the popup or below it?".
 * @param gui_overlay_draw_fn draw_fn -> Called once per frame after the main draw walk. NULL closes the overlay.
 *
 * @return void
 */
void scene__set_overlay(gui_node_t* node, gui_rect_t bounds, gui_overlay_draw_fn draw_fn)
{
    _scene_internal__overlay_node    = node;
    _scene_internal__overlay_bounds  = bounds;
    _scene_internal__overlay_draw_fn = draw_fn;
}

/**
 * @brief read the currently active overlay node, or NULL if no overlay is set.
 *
 * @return gui_node_t* -> The overlay's owner node, or NULL.
 */
gui_node_t* scene__overlay_node(void)
{
    return _scene_internal__overlay_node;
}

// ===== style entry point =====================================================
// Heavy lifting lives in scene_style.c. scene.c keeps the root
// pointer + calls in once per frame. See scene_style.h.

/**
 * @brief apply registered selectors to per-state slots, then resolve the active state into each node's `resolved` style.
 *
 * @return void
 */
void scene__resolve_styles(void)
{
    if (_scene_internal__root == NULL_POINTER)
    {
        return;
    }
    widget_navigation__prepare_tree(_scene_internal__root);
    scene_style__apply_rules_tree(_scene_internal__root);
    scene_style__resolve_tree(_scene_internal__root);
}

// ===== draw emission entry point ============================================
// Heavy lifting lives in scene_render.c. scene.c retains this
// orchestration because it also drives the overlay pass.

/**
 * @brief walk the active tree and submit one or more renderer__submit_rect calls per node (one for plain nodes; track + thumb for sliders). Called from platform_win32__tick between begin_frame and end_frame.
 *
 * @return void
 */
void scene__emit_draws(void)
{
    if (_scene_internal__root == NULL_POINTER)
    {
        return;
    }
    scene_render__emit_tree(_scene_internal__root, _scene_internal__scale);

    // Overlay pass. Lands ON TOP of the normal walk so a <select>
    // dropdown etc. sits above its siblings in the tree.
    if (_scene_internal__overlay_node != NULL_POINTER && _scene_internal__overlay_draw_fn != NULL_POINTER)
    {
        _scene_internal__overlay_draw_fn(_scene_internal__overlay_node, _scene_internal__scale);
    }
}

// ===== hit testing ==========================================================

static boole _scene_internal__rect_contains(gui_rect_t r, float x, float y)
{
    return (boole)(x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
}

static gui_node_t* _scene_internal__hit_recursive(gui_node_t* n, float x, float y)
{
    char pointer_events[32];
    gui_node_t* pointer_scope = n;

    stdlib__null_memory(pointer_events, sizeof(pointer_events));
    if (n == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    // display:none and visibility:hidden nodes must NOT be hit-tested.
    // They're not drawn either (emit_recursive early-outs on them), so
    // clicking over one would otherwise reach children that the user
    // cannot see. Without this check, a hidden shell-column under a
    // visible fullscreen-view steals every click that lands outside
    // the fullscreen-view's own widget bounds.
    if (n->navigation_inactive == TRUE || n->resolved.display == GUI_DISPLAY__NONE)
    {
        return NULL_POINTER;
    }
    if (n->resolved.visibility == GUI_VISIBILITY__HIDDEN)
    {
        return NULL_POINTER;
    }
    boole hit_outside = FALSE;

    compositor__map_point(n, &x, &y);
    while (pointer_scope != NULL_POINTER)
    {
        if (motion__value_id(pointer_scope, MOTION_PROP__POINTER_EVENTS, pointer_events, sizeof(pointer_events)) == TRUE)
        {
            break;
        }
        pointer_scope = pointer_scope->parent;
    }
    // a point outside the box: a clipping node (overflow other than
    // visible) cuts its subtree off, as its children are clipped there;
    // a plain node is not a candidate itself but its overflowing children
    // still are (as in CSS: a menu dragged past its parent stays clickable)
    hit_outside = (boole)(_scene_internal__rect_contains(n->bounds, x, y) == FALSE);
    if (hit_outside == TRUE && (n->resolved.overflow_x != GUI_OVERFLOW__VISIBLE || n->resolved.overflow_y != GUI_OVERFLOW__VISIBLE))
    {
        return NULL_POINTER;
    }

    // scrollbar-strip priority. if this node is a scrollable container
    // and the click lands inside the rightmost bar_size pixels of its
    // bounds, route the click to THIS node regardless of whether any
    // child would otherwise match deeper. without this, pinned-width
    // children (e.g. a Column with size_w = 380 px scaled up past the
    // parent's content_w) extend past the scrollbar strip at large
    // scale, and the deepest-wins rule below sends clicks to the
    // column -- which has no scroll handler, so the thumb becomes
    // unclickable even though wheel scrolling still works (wheel
    // walks the parent chain explicitly in scene__on_mouse_wheel).
    // applies to both AUTO and SCROLL overflow modes. VISIBLE / HIDDEN
    // don't draw a bar so there's nothing to intercept.
    if (hit_outside == FALSE && scroll__vbar_visible(n))
    {
        float bar_size  = scroll__bar_size(n, _scene_internal__scale);
        float strip_x0  = n->bounds.x + n->bounds.w - bar_size;
        float strip_x1  = n->bounds.x + n->bounds.w;
        if (x >= strip_x0 && x < strip_x1 && y >= n->bounds.y && y < n->bounds.y + n->bounds.h)
        {
            return n;
        }
    }

    gui_node_t* best = (stdlib__strcmp(pointer_events, "none") == 0 || hit_outside == TRUE) ? NULL_POINTER : n;
    gui_node_t* c    = n->first_child;
    gui_node_t* slots[256];
    gui_node_t* key = NULL_POINTER;
    gui_node_t* child_hit = NULL_POINTER;
    int64 count = 0;
    int64 i = 0;
    int64 j = 0;
    boole sorted_walk = TRUE;

    // children are tested in paint order (z-index, then positioned above
    // static, then tree order) so the child drawn on top wins the hit
    while (c != NULL_POINTER)
    {
        if (count >= 256)
        {
            sorted_walk = FALSE;
            break;
        }

        slots[count] = c;
        count = count + 1;
        c = c->next_sibling;
    }

    if (sorted_walk == TRUE)
    {
        for (i = 1; i < count; i++)
        {
            key = slots[i];
            j = i - 1;

            while (j >= 0 && scene_render__paint_key(slots[j]) > scene_render__paint_key(key))
            {
                slots[j + 1] = slots[j];
                j = j - 1;
            }

            slots[j + 1] = key;
        }

        for (i = 0; i < count; i++)
        {
            child_hit = _scene_internal__hit_recursive(slots[i], x, y);

            if (child_hit != NULL_POINTER)
            {
                best = child_hit;
            }
        }

        return best;
    }

    c = n->first_child;

    while (c != NULL_POINTER)
    {
        child_hit = _scene_internal__hit_recursive(c, x, y);

        if (child_hit != NULL_POINTER)
        {
            best = child_hit;
        }

        c = c->next_sibling;
    }

    return best;
}

/**
 * @brief find the deepest node whose bounds contain the given point.
 *
 * @param gui_node_t* root -> Subtree to search.
 * @param int64 x -> Pixel x, top-left origin.
 * @param int64 y -> Pixel y, top-left origin.
 *
 * @return gui_node_t* -> The deepest matching node, or NULL if no node contains the point.
 */
gui_node_t* scene__hit_test(gui_node_t* root, int64 x, int64 y)
{
    // Overlay takes priority over the normal tree walk. When a
    // <select>'s dropdown is open and the cursor lands inside the
    // popup's projected bounds, we route the click to the select
    // (its on_mouse_down figures out which option was hit from x/y).
    // Without this the click would fall through to whatever sibling
    // the popup is visually covering.
    if (_scene_internal__overlay_node != NULL_POINTER)
    {
        gui_rect_t ob = _scene_internal__overlay_bounds;
        if ((float)x >= ob.x && (float)x < ob.x + ob.w && (float)y >= ob.y && (float)y < ob.y + ob.h)
        {
            return _scene_internal__overlay_node;
        }
    }
    return _scene_internal__hit_recursive(root, (float)x, (float)y);
}

// ===== event dispatch =======================================================

/**
 * @brief dispatch a click event to a node's on_click handler. Resolves the handler through the registration table + symbol-resolver fallback and caches on first hit. Used by scene's input state machine and by widgets that programmatically fire clicks.
 *
 * @param gui_node_t* n -> Sender node.
 * @param int64 x -> Cursor x at release.
 * @param int64 y -> Cursor y at release.
 * @param int64 button -> Button index (0 = left).
 *
 * @return void
 */
void scene__dispatch_click(gui_node_t* n, int64 x, int64 y, int64 button)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    // Walk UP from the deepest hit until we find a node with
    // on_click set. Lets a host put the handler on a parent (e.g.
    // a tile column) and have clicks anywhere in the tile's
    // subtree dispatch through that single handler. Without this
    // walk, on_click had to be set on every clickable leaf of a
    // tile (icon-frame, image, label, etc.) which made the press-
    // group state-chain logic in scene_input awkward.
    // ev.sender is set to the node that ACTUALLY carries the
    // handler (not the deepest hit) so the handler sees the click-
    // target, not the leaf decoration that intercepted the press.
    gui_node_t* target = n;
    while (target != NULL_POINTER && target->on_click_hash == 0 && target->parent != NULL_POINTER)
    {
        target = target->parent;
    }
    if (target == NULL_POINTER || target->on_click_hash == 0)
    {
        return;
    }
    gui_handler_fn fn = _scene_internal__resolve_handler(target->on_click_hash, target->on_click_name);
    if (fn == NULL_POINTER)
    {
        log_warn("%s %s %s %u", "on_click handler not found: name='", target->on_click_name, "' hash=", target->on_click_hash);
        return;
    }
    gui_event_t ev;
    stdlib__memset(&ev, 0, sizeof(ev));
    ev.type         = GUI_EVENT_TYPE__CLICK;
    ev.sender       = target;
    ev.mouse.x      = x;
    ev.mouse.y      = y;
    ev.mouse.button = button;
    fn(&ev);
}

// Fire a window-level handler by name. Unlike dispatch_click, this
// doesn't involve a scene node -- the caller has the handler name
// directly (gesture recognizer calls with "on_swipe_up_bottom" etc.).
// Looks up by hash in the same registry used by on_click/on_change;
// silently no-ops if the handler isn't registered (user's UI just
// doesn't care about this gesture).
/**
 * @brief look up a registered handler by name (FNV-1a of the name; same hashing as on_click) and invoke it with a zero-initialized event (ev->sender = NULL). Used for window-level events that don't target a specific node -- notably gesture recognition (`on_swipe_up_bottom` isn't a click on any particular widget). If no handler with this name is registered, the call is a cheap no-op.
 *
 * @param const char* handler_name -> Name registered via UI_HANDLER or scene__register_handler.
 *
 * @return void
 */
void scene__dispatch_gesture_by_name(const char* handler_name)
{
    if (handler_name == NULL_POINTER || handler_name[0] == 0)
    {
        return;
    }
    uint hash = scene__hash_name((char*)handler_name);
    gui_handler_fn fn = _scene_internal__resolve_handler(hash, (char*)handler_name);
    if (fn == NULL_POINTER)
    {
        // Logged at info so it's visible by default during
        // development. If a user .ui simply doesn't bind a
        // gesture, this is "harmless" but confusing without the
        // log line. When the shell stabilizes we can drop this
        // back to trace.
        log_info("%s %s %s", "scene__dispatch_gesture_by_name: no handler registered for '", handler_name, "'");
        return;
    }
    gui_event_t ev;
    stdlib__memset(&ev, 0, sizeof(ev));
    ev.type   = GUI_EVENT_TYPE__CLICK; // closest existing type; ev.sender=NULL signals global.
    ev.sender = NULL_POINTER;
    fn(&ev);
}

/**
 * @brief dispatch a change event to a node's on_change handler with a scalar payload. Used by sliders and any future widget that emits GUI_EVENT_TYPE__CHANGE.
 *
 * @param gui_node_t* n -> Sender node.
 * @param float new_value -> The new scalar value to deliver via ev->change.scalar.
 *
 * @return void
 */
void scene__dispatch_change(gui_node_t* n, float new_value)
{
    if (n == NULL_POINTER || n->on_change_hash == 0)
    {
        return;
    }
    gui_handler_fn fn = _scene_internal__resolve_handler(n->on_change_hash, n->on_change_name);
    if (fn == NULL_POINTER)
    {
        log_warn("%s %s %s %u", "on_change handler not found: name='", n->on_change_name, "' hash=", n->on_change_hash);
        return;
    }
    gui_event_t ev;
    stdlib__memset(&ev, 0, sizeof(ev));
    ev.type          = GUI_EVENT_TYPE__CHANGE;
    ev.sender        = n;
    ev.change.scalar = new_value;
    fn(&ev);
}

/**
 * @brief dispatch a prebuilt event. Lets widgets attach payloads that don't fit the scalar/click model (e.g. colorpicker emits a gui_color_t via ev->color.value; popup emits ev->popup.confirmed). Resolves the handler via the same table + symbol-resolver fallback as scene__dispatch_change / scene__dispatch_click.
 *
 * @param gui_node_t* n -> Sender node; handler is looked up from its on_click / on_change slot based on ev->type.
 * @param gui_event_t* ev -> Event to deliver. `sender` is overwritten.
 *
 * @return void
 */
void scene__dispatch_event(gui_node_t* n, gui_event_t* ev)
{
    if (n == NULL_POINTER || ev == NULL_POINTER)
    {
        return;
    }
    // The event's type decides which handler to resolve. Same
    // fallback path as scene__dispatch_click / scene__dispatch_change:
    // the scene's handler table first, then the platform symbol
    // resolver (GetProcAddress / dlsym) if the named handler was
    // exported via UI_HANDLER.
    uint  hash = 0;
    char* name = NULL_POINTER;
    if (ev->type == GUI_EVENT_TYPE__CLICK)
    {
        hash = n->on_click_hash;
        name = n->on_click_name;
    }
    else
    {
        hash = n->on_change_hash;
        name = n->on_change_name;
    }
    if (hash == 0)
    {
        return;
    }
    gui_handler_fn fn = _scene_internal__resolve_handler(hash, name);
    if (fn == NULL_POINTER)
    {
        log_warn("%s %s %s %u", "handler not found: name='", name, "' hash=", hash);
        return;
    }
    ev->sender = n;
    fn(ev);
}

// ===== id lookup ===========================================================
// Left in scene.c (not scene_input.c) because id lookup is tree-walk
// utility, not input-state. Used by hot_reload and meek-shell's
// meek_shell_v1_client to resolve a named widget at runtime.

static gui_node_t* _scene_internal__find_by_id_recursive(gui_node_t* n, char* id)
{
    if (n == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    if (n->id[0] != 0 && stdlib__strcmp(n->id, id) == 0)
    {
        return n;
    }
    gui_node_t* c = n->first_child;
    while (c != NULL_POINTER)
    {
        gui_node_t* hit = _scene_internal__find_by_id_recursive(c, id);
        if (hit != NULL_POINTER)
        {
            return hit;
        }
        c = c->next_sibling;
    }
    return NULL_POINTER;
}

/**
 * @brief walk the current scene tree and return the first node whose id matches the given string. NULL if no match. O(n) in tree size -- cheap for PoC-scale scenes; callers that query often should cache the returned pointer across frames since tree identity is stable between hot reloads.
 *
 * @param char* id -> id string to look up (case-sensitive).
 *
 * @return gui_node_t* -> The matching node, or NULL.
 */
gui_node_t* scene__find_by_id(char* id)
{
    if (id == NULL_POINTER || id[0] == 0 || _scene_internal__root == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    return _scene_internal__find_by_id_recursive(_scene_internal__root, id);
}

/**
 * @brief step a widget's animation slot toward a target (an exponential ease; the first step snaps)
 * @param gui_node_t* node -> the widget
 * @param float target -> the value to reach
 * @param float tau_ms -> time constant in milliseconds
 * @return float -> the new value of widget_anim
 */
float scene__widget_ease(gui_node_t* node, float target, float tau_ms)
{
    int64 now = scene__frame_time_ms();
    float dt = 0.0f;
    float step = 0.0f;

    if (node->widget_anim_ms == 0)
    {
        node->widget_anim = target;
    }
    else
    {
        dt = (float)(now - (node->widget_anim_ms < 0 ? 0 : node->widget_anim_ms));
        if (dt < 0.0f)
        {
            dt = 0.0f;
        }
        if (dt > 100.0f)
        {
            dt = 100.0f;
        }
        step = (tau_ms > 0.0f) ? 1.0f - stdlib__expf(-dt / tau_ms) : 1.0f;
        node->widget_anim += (target - node->widget_anim) * step;
    }
    node->widget_anim_ms = now > 0 ? now : -1;
    if (node->widget_anim - target < 0.005f && target - node->widget_anim < 0.005f)
    {
        node->widget_anim = target;
        node->widget_animating = FALSE;
    }
    else
    {
        node->widget_animating = TRUE;
    }
    return node->widget_anim;
}
