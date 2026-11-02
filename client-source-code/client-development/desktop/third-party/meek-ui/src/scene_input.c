// scene_input.c -- pointer + touch + keyboard input state machine,
// split out of scene.c. Owns:
// * hover / pressed / focus tracking (_input struct)
// * scene__on_mouse_move / scene__on_mouse_button
// * scene__on_mouse_wheel / scene__on_touch_scroll
// * scene__on_char / scene__on_key / scene__deliver_*
// * scene__set_focus / scene__focus / scene__pressed
// * scene__on_resize (stub; layout runs every frame anyway)
// Reaches the tree via scene__root() / scene__hit_test() from
// scene.h, and fires keyboard auto-show/hide through the existing
// scene__show_keyboard / hide_keyboard / virtual_keyboard_enabled
// public API. scene.c's node-free path calls
// scene_input__on_node_freed so a destroyed node can't be left
// hanging as focus/press/hover target.

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "scene_input.h"
#include "widgets/widget_navigation.h"
#include "scroll.h"
#include "third_party/log.h"
#include "debug_definitions.h"

// Optional char-redirect hook. Set by a host (meek-shell) to
// forward character input from widget_keyboard to an alternative
// sink (e.g., a zwp_text_input_v3 commit_string on the compositor).
// When non-null, scene__deliver_char and scene__on_char call the
// redirect; if it returns TRUE, normal vtable delivery is skipped.
// NULL means "disabled" -- characters go to the focused widget's
// on_char as before.
static scene_char_redirect_fn _scene_input_internal__char_redirect = NULL_POINTER;

void scene__set_char_redirect(scene_char_redirect_fn fn)
{
    _scene_input_internal__char_redirect = fn;
}

/**
 * @brief query whether a char redirect is currently installed.
 *
 * @return boole -> TRUE iff a non-null redirect was set via scene__set_char_redirect. Widgets that emit non-character keys (backspace, return) use this to pick between two dispatch modes: * Redirect present -> convert the key to its ASCII equivalent (0x08 for backspace, 0x0D for return), call scene__on_char(ch). The redirect consumes it and can forward to a foreign sink (e.g. wl_keyboard.key via meek_shell_v1). * Redirect absent -> call scene__on_key(vk, down) so the locally focused widget (input / textarea) handles it in its on_key vtable, which is where VK_BACK / VK_RETURN live in the meek-ui widget contract.
 */
boole scene__has_char_redirect(void)
{
    return (boole)(_scene_input_internal__char_redirect != NULL_POINTER);
}

// Text-input focus hook. See scene.h for the design rationale.
// When set, scene__set_focus calls this on text-widget focus
// transitions (active=TRUE on enter, FALSE on leave) and SKIPS the
// in-tree widget_keyboard auto-show -- the host platform handles
// the keyboard out-of-process (e.g. meek-ui's wayland-client backend
// fires zwp_text_input_v3.enable/disable, the compositor turns that
// into a meek_shell_v1.ime_request_on/off, and meek-shell shows its
// keyboard).
static scene_text_input_focus_fn _scene_input_internal__text_input_focus_hook = NULL_POINTER;

void scene__set_text_input_focus_hook(scene_text_input_focus_fn fn)
{
    _scene_input_internal__text_input_focus_hook = fn;
}

// Active modifier bitmask. widget_keyboard writes before each
// scene__on_char / scene__on_key call; the char redirect
// (meek_shell_v1_client on the shell side) reads during the
// redirect callback to forward ctrl / shift state to the target
// wl_keyboard client.
static uint _scene_input_internal__active_modifiers = 0;

/**
 * @brief active keyboard modifier bitmask, pushed by widget_keyboard right before scene__on_char / scene__on_key, consumed by char redirect callbacks that want to forward the active ctrl / shift state along with the character (terminal keybindings: ctrl-C, ctrl-D, etc.). Bit layout matches wl_keyboard.modifiers: 1 << 0  shift (depressed) 1 << 2  ctrl  (depressed) Other bits (caps, alt, mod3/4/5) reserved for future use and should be zero for now.
 *
 * @return void
 */
void scene__set_active_modifiers(uint mask)
{
    _scene_input_internal__active_modifiers = mask;
}

static void _scene_input_internal__key_modifier(int64 vk, boole down)
{
    uint bit = 0;
    if (vk == 0x10 || vk == 0xa0 || vk == 0xa1)
    {
        bit = 1;
    }
    else if (vk == 0x11 || vk == 0xa2 || vk == 0xa3)
    {
        bit = 4;
    }
    else if (vk == 0x12 || vk == 0xa4 || vk == 0xa5)
    {
        bit = 8;
    }
    if (down)
    {
        _scene_input_internal__active_modifiers |= bit;
    }
    else
    {
        _scene_input_internal__active_modifiers &= ~bit;
    }
}

// Touch-pan-scroll fallback state. The press chain pinned a leaf
// widget on mouse_button(down); if that leaf doesn't consume the
// drag (most leaves -- buttons, images, labels -- have no
// on_mouse_drag), we walk the parent chain and apply the drag
// delta to the nearest overflow-scroll ancestor's scroll_y_target.
// Standard mobile "drag the content to scroll" behavior. Without
// it, overflow-y: auto on a <div> only scrolls when the user grabs
// the thin scrollbar thumb, which is unusable on a touch screen.
// pan_lock_node is set the first time the drag exceeds
// _PAN_THRESHOLD_PX away from the press position; once locked, all
// subsequent moves go to that ancestor's scroll_y_target until the
// next mouse_button(down). pan_drag_active flips on the first
// committed pan and tells mouse_button(up) to suppress the click
// dispatch (a drag-to-scroll shouldn't also fire a click on the
// thing the user happened to start touching).
#define _SCENE_INPUT_INTERNAL__PAN_THRESHOLD_PX 8

static int64    _scene_input_internal__press_x       = 0;
static int64    _scene_input_internal__press_y       = 0;
static int64    _scene_input_internal__pan_last_y    = 0;
static gui_node_t* _scene_input_internal__pan_lock_node = NULL_POINTER;
static boole    _scene_input_internal__pan_drag_active = FALSE;
// app-region: drag. The positioned node a press on a drag region moves,
// and the last mouse position applied to it.
static gui_node_t* _scene_input_internal__drag_target = NULL_POINTER;
static int64    _scene_input_internal__drag_last_x = 0;
static int64    _scene_input_internal__drag_last_y = 0;

// Walk up from `start` (a leaf-pressed node) and return the nearest
// ancestor whose overflow-y is SCROLL or AUTO AND whose laid-out
// content exceeds its bounds. Returns NULL when no ancestor wants
// the drag (e.g. content fits, or no overflow declared anywhere
// up-tree).
static gui_node_t* _scene_input_internal__find_pan_target(gui_node_t* start)
{
    for (gui_node_t* n = start; n != NULL_POINTER; n = n->parent)
    {
        gui_overflow_e oy = n->resolved.overflow_y;
        if (oy != GUI_OVERFLOW__SCROLL && oy != GUI_OVERFLOW__AUTO)
        {
            continue;
        }
        if (n->content_h <= n->bounds.h + 0.5f)
        {
            continue;
        }
        return n;
    }
    return NULL_POINTER;
}

uint scene__get_active_modifiers(void)
{
    return _scene_input_internal__active_modifiers;
}

// current input state for the hover + press + focus state machine.
typedef struct _scene_input_internal__state_t
{
    gui_node_t* hover;     // 0x0
    gui_node_t* pressed;   // 0x8
    int64 pressed_button; // 0x10
    gui_node_t* focus;     // 0x18
} _scene_input_internal__state_t;

static _scene_input_internal__state_t _scene_input_internal__input = { 0 };
static int64 _scene_input_internal__pointer_x, _scene_input_internal__pointer_y;
static boole _scene_input_internal__has_pointer;

boole scene__pointer_position(int64* x, int64* y)
{
    if (x) *x = _scene_input_internal__pointer_x;
    if (y) *y = _scene_input_internal__pointer_y;
    return _scene_input_internal__has_pointer;
}

static void _scene_input_internal__set_state(gui_node_t* n, gui_node_state_e s)
{
    if (n != NULL_POINTER)
    {
        n->state = s;
    }
}

// Walk from `deepest` up the parent chain, setting `state` on each
// node. Lets `:active` / `:hover` style rules fire on ancestors of
// the hit node, not just on the hit node itself.
// Why: meek-ui's hit-test returns the deepest node under the touch
// point; its `state` field gets the new value, and the resolver
// overlays `style[state]` for THAT node. Parents in the same
// click-chain stay at GUI_NODE_STATE__DEFAULT, so a `:active` rule on a
// container (e.g. a launcher tile wrapping an icon image) never
// fires when the inner image is the deepest hit.
// CSS proper propagates :active and :hover up to the document
// element. We do the same -- walk all the way up to root. Cheap
// (chain is ~5 nodes deep in practice) and matches the mental
// model devs bring from CSS.
// Recursive descent helper for set_state_chain. Walks the entire
// subtree rooted at `n` setting each node's state to `s`. Used by
// the chain to also propagate state DOWN to a "press group" -- the
// hit node's immediate parent's full subtree -- so siblings of the
// deepest hit (and their descendants) light up alongside the hit
// itself. Without this, clicking a hit-tested leaf only marks the
// up-chain (leaf + ancestors); a sibling's :active rule wouldn't
// fire even though the user thinks of the whole tile-like
// container as a single press unit.
static void _scene_input_internal__set_state_subtree(gui_node_t* n, gui_node_state_e s)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    n->state = s;
    for (gui_node_t* c = n->first_child; c != NULL_POINTER; c = c->next_sibling)
    {
        _scene_input_internal__set_state_subtree(c, s);
    }
}

static void _scene_input_internal__set_state_chain(gui_node_t* deepest, gui_node_state_e s)
{
    // Walk UP from the deepest-hit node, marking each ancestor with
    // the new state. CSS-equivalent of :active firing on the chain
    // of nodes from the hit to the root.
    for (gui_node_t* n = deepest; n != NULL_POINTER; n = n->parent)
    {
        n->state = s;
    }
    // Press-group expansion: find the deepest ancestor (or self)
    // that carries an `on_click` handler -- that node defines the
    // "click target", and its entire subtree should visually
    // respond to the press, not just the leaf the hit-test landed
    // on. So a tile column with `on_click` set + a frame + image +
    // label inside reads as one press unit; tapping any of those
    // children marks the whole tile :active.
    // If no ancestor has on_click, skip the down-walk (the up-walk
    // already covered the chain). Common case for non-clickable
    // chrome where there's nothing to "press" anyway.
    gui_node_t* group_root = deepest;
    while (group_root != NULL_POINTER && group_root->on_click_hash == 0 && group_root->parent != NULL_POINTER)
    {
        group_root = group_root->parent;
    }
    if (group_root != NULL_POINTER && group_root->on_click_hash != 0)
    {
        _scene_input_internal__set_state_subtree(group_root, s);
    }
}

void scene_input__on_node_freed(gui_node_t* n)
{
    if (n == NULL_POINTER)
    {
        return;
    }
    if (_scene_input_internal__input.focus   == n)
    {
        _scene_input_internal__input.focus   = NULL_POINTER;
    }
    if (_scene_input_internal__input.pressed == n)
    {
        _scene_input_internal__input.pressed = NULL_POINTER;
    }
    if (_scene_input_internal__input.hover   == n)
    {
        _scene_input_internal__input.hover   = NULL_POINTER;
    }
}

/**
 * @brief update the hover-state machine for a new mouse position.
 *
 * @param int64 x -> Pixel x.
 * @param int64 y -> Pixel y.
 *
 * @return void
 */
void scene__on_mouse_move(int64 x, int64 y)
{
    _scene_input_internal__pointer_x = x;
    _scene_input_internal__pointer_y = y;
    _scene_input_internal__has_pointer = TRUE;
    gui_node_t* root = scene__root();
    if (root == NULL_POINTER)
    {
        return;
    }

    gui_node_t* pressed = _scene_input_internal__input.pressed;
    if (pressed != NULL_POINTER)
    {
        const widget_vtable_t* vt = widget_registry__get(pressed->type);
        if (vt != NULL_POINTER && vt->on_mouse_drag != NULL_POINTER)
        {
            if (vt->on_mouse_drag(pressed, x, y))
            {
                return;
            }
        }

        // app-region: drag. A press on a drag region (a title bar) moves
        // the nearest absolutely or fixed positioned ancestor by the
        // mouse; the release fires no click.
        if (_scene_input_internal__drag_target == NULL_POINTER && _scene_input_internal__pan_lock_node == NULL_POINTER)
        {
            gui_node_t* handle = NULL_POINTER;
            gui_node_t* target = NULL_POINTER;

            for (handle = pressed; handle != NULL_POINTER; handle = handle->parent)
            {
                if (handle->resolved.app_region_drag == TRUE)
                {
                    for (target = handle; target != NULL_POINTER; target = target->parent)
                    {
                        if (target->resolved.position == GUI_POSITION__ABSOLUTE || target->resolved.position == GUI_POSITION__FIXED)
                        {
                            break;
                        }
                    }
                    if (target != NULL_POINTER)
                    {
                        _scene_input_internal__drag_target = target;
                        _scene_input_internal__drag_last_x = _scene_input_internal__press_x;
                        _scene_input_internal__drag_last_y = _scene_input_internal__press_y;
                    }
                    break;
                }
            }
        }
        if (_scene_input_internal__drag_target != NULL_POINTER)
        {
            _scene_input_internal__drag_target->drag_offset_x += (float)(x - _scene_input_internal__drag_last_x);
            _scene_input_internal__drag_target->drag_offset_y += (float)(y - _scene_input_internal__drag_last_y);
            _scene_input_internal__drag_last_x = x;
            _scene_input_internal__drag_last_y = y;
            _scene_input_internal__pan_drag_active = TRUE;
            return;
        }

        // Touch-pan-scroll fallback. Runs only when the leaf
        // widget didn't claim the drag (no on_mouse_drag, or its
        // hook returned FALSE). Walks up to the nearest overflow-y
        // ancestor with overflowing content; applies dy to the
        // ancestor's scroll_y_target. Locks to a single ancestor
        // for the rest of the press so a long drag stays consistent
        // even if the cursor wanders into a different scrollable
        // subtree.
        if (_scene_input_internal__pan_lock_node == NULL_POINTER)
        {
            int64 dx = x - _scene_input_internal__press_x;
            int64 dy = y - _scene_input_internal__press_y;
            int64 abs_dx = (dx < 0) ? -dx : dx;
            int64 abs_dy = (dy < 0) ? -dy : dy;
            if (abs_dy >= _SCENE_INPUT_INTERNAL__PAN_THRESHOLD_PX && abs_dy > abs_dx)
            {
                _scene_input_internal__pan_lock_node = _scene_input_internal__find_pan_target(pressed);
                if (_scene_input_internal__pan_lock_node != NULL_POINTER)
                {
                    _scene_input_internal__pan_drag_active = TRUE;
                    _scene_input_internal__pan_last_y = _scene_input_internal__press_y;
                    // Fresh pan -- velocity EWMA + sample
                    // timestamp start from 0 so the first motion
                    // event doesn't read a stale dt from a prior
                    // gesture. (scroll_y_velocity itself is
                    // already 0 here -- either tap-to-stop in
                    // touch_down zeroed it, or there was no
                    // prior fling.)
                    _scene_input_internal__pan_lock_node->scroll_pan_last_ms = 0;
                }
            }
        }
        if (_scene_input_internal__pan_lock_node != NULL_POINTER)
        {
            gui_node_t* target = _scene_input_internal__pan_lock_node;
            int64 step = _scene_input_internal__pan_last_y - y;
            int64 now_ms = (int64)scene__frame_time_ms();
            // Compute instantaneous velocity (px/ms, signed) and
            // EWMA into the node's velocity field. Heavy bias on
            // recent samples (0.6 new / 0.4 old) so the value at
            // touch_up reflects the user's last gesture, not the
            // 200-sample average of the whole pan. A pure-snapshot
            // would over-react to a single jittery sample.
            if (target->scroll_pan_last_ms != 0)
            {
                int64 dt = now_ms - target->scroll_pan_last_ms;
                if (dt > 0)
                {
                    float instantaneous = (float)step / (float)dt;
                    target->scroll_y_velocity =
                        0.4f * target->scroll_y_velocity + 0.6f * instantaneous;
                }
            }
            target->scroll_pan_last_ms = now_ms;

            // Rubber band (overscroll-behavior: auto): the part of the
            // step past an edge pulls the content with resistance and
            // the finger holds it there; a step back moves the pulled
            // content first and continues on the track past the edge.
            if (scroll__overscroll_enabled_y(target) == TRUE)
            {
                float max_y = target->content_h - target->bounds.h;
                float remaining = (float)step;
                float position = 0.0f;
                float before = 0.0f;

                if (max_y < 0.0f)
                {
                    max_y = 0.0f;
                }
                target->overscroll_y_held = TRUE;
                target->overscroll_y_fling = FALSE;
                target->overscroll_y_returning = FALSE;
                if (target->overscroll_y != 0.0f)
                {
                    before = target->overscroll_y;
                    scroll__overscroll_push_y(target, remaining);
                    if ((before > 0.0f && target->overscroll_y <= 0.0f) || (before < 0.0f && target->overscroll_y >= 0.0f))
                    {
                        target->overscroll_y = 0.0f;
                    }
                    remaining = 0.0f;
                }
                if (remaining != 0.0f)
                {
                    position = target->scroll_y + remaining;
                    if (position > max_y)
                    {
                        remaining = position - max_y;
                        target->scroll_y = max_y;
                    }
                    else if (position < 0.0f)
                    {
                        remaining = position;
                        target->scroll_y = 0.0f;
                    }
                    else
                    {
                        target->scroll_y = position;
                        remaining = 0.0f;
                    }
                    target->scroll_y_target = target->scroll_y;
                    if (remaining != 0.0f)
                    {
                        scroll__overscroll_push_y(target, remaining);
                    }
                }
                target->scroll_last_active_ms = now_ms;
                _scene_input_internal__pan_last_y = y;
                return;
            }

            target->scroll_y_target += (float)step;
            target->scroll_y         = target->scroll_y_target;
            target->scroll_last_active_ms = now_ms;
            scroll__clamp(target);
            _scene_input_internal__pan_last_y = y;
            return;
        }
    }

    gui_node_t* hit  = scene__hit_test(root, x, y);
    gui_node_t* prev = _scene_input_internal__input.hover;

    if (hit == prev)
    {
        return;
    }

    // Hover-leave: walk the previous-hover chain back to default,
    // skipping any node still in the active-press chain (those
    // remain GUI_NODE_STATE__PRESSED until release).
    gui_node_t* still_pressed = _scene_input_internal__input.pressed;
    if (prev != NULL_POINTER)
    {
        for (gui_node_t* n = prev; n != NULL_POINTER; n = n->parent)
        {
            // If this node is also in the press chain, leave it
            // PRESSED. Cheap "is n an ancestor-or-self of pressed"
            // check via walking up from pressed.
            boole in_press_chain = FALSE;
            for (gui_node_t* p = still_pressed; p != NULL_POINTER; p = p->parent)
            {
                if (p == n)
                {
                    in_press_chain = TRUE;
                    break;
                }
            }
            if (in_press_chain == FALSE)
            {
                n->state = GUI_NODE_STATE__DEFAULT;
            }
        }
    }

    _scene_input_internal__input.hover = hit;

    // Hover-enter: chain set to HOVER, but again skip nodes
    // currently in the press chain.
    if (hit != NULL_POINTER)
    {
        for (gui_node_t* n = hit; n != NULL_POINTER; n = n->parent)
        {
            boole in_press_chain = FALSE;
            for (gui_node_t* p = still_pressed; p != NULL_POINTER; p = p->parent)
            {
                if (p == n)
                {
                    in_press_chain = TRUE;
                    break;
                }
            }
            if (in_press_chain == FALSE)
            {
                n->state = GUI_NODE_STATE__HOVER;
            }
        }
    }
}

/**
 * @brief update the pressed-state machine for a mouse button event. On release-on-same-node, dispatches the bound click handler.
 *
 * @param int64 button -> Button index (0 = left).
 * @param boole down -> TRUE for press, FALSE for release.
 * @param int64 x -> Pixel x.
 * @param int64 y -> Pixel y.
 *
 * @return void
 */
void scene__on_mouse_button(int64 button, boole down, int64 x, int64 y)
{
    _scene_input_internal__pointer_x = x;
    _scene_input_internal__pointer_y = y;
    _scene_input_internal__has_pointer = TRUE;
    gui_node_t* root = scene__root();
    if (root == NULL_POINTER)
    {
        return;
    }

    gui_node_t* hit = scene__hit_test(root, x, y);
    DBG_INPUT log_info("%s %d %s %ld %s %ld %s %ld %s %p %s %d", "[dbg-input] mouse_button down=", (int)down, "btn=", (long)button, "at (", (long)x, ",", (long)y, ") -> hit=", (void*)hit, "type=", hit ? (int)hit->type : -1);

    if (down)
    {
        // Reset pan-scroll tracking on every fresh press. Records
        // the start position so on_mouse_move can compare and
        // commit to pan-scroll once the drag exceeds the threshold.
        _scene_input_internal__press_x         = x;
        _scene_input_internal__press_y         = y;
        _scene_input_internal__pan_last_y      = y;
        _scene_input_internal__pan_lock_node   = NULL_POINTER;
        _scene_input_internal__pan_drag_active = FALSE;
        _scene_input_internal__drag_target     = NULL_POINTER;

        // Tap-to-stop: if any ancestor of the hit-tested node has
        // an in-flight fling (non-zero scroll_y_velocity), the
        // touch acts as a brake and zeroes the velocity. iOS /
        // Android scrollers do the same -- a finger on the screen
        // ends momentum even before any drag motion. Walk up from
        // hit because the scrollable container is usually 2-3
        // ancestors above the actual tapped widget.
        if (hit != NULL_POINTER)
        {
            for (gui_node_t* anc = hit; anc != NULL_POINTER; anc = anc->parent)
            {
                if (anc->scroll_y_velocity != 0.0f)
                {
                    anc->scroll_y_velocity = 0.0f;
                    anc->scroll_pan_last_ms = 0;
                    break;
                }
            }
        }

        if (hit != NULL_POINTER)
        {
            _scene_input_internal__input.pressed = hit;
            _scene_input_internal__input.pressed_button = button;
            _scene_input_internal__set_state_chain(hit, GUI_NODE_STATE__PRESSED);

            const widget_vtable_t* vt = widget_registry__get(hit->type);

            boole preserves = (boole)(vt != NULL_POINTER && (vt->preserves_focus || (vt->flags & GUI_WIDGET_FLAGS__PRESERVES_FOCUS)));
            boole takes     = (boole)(vt != NULL_POINTER && (vt->takes_focus     || (vt->flags & GUI_WIDGET_FLAGS__TAKES_FOCUS)));
            if (preserves)      { /* no-op */ }
            else if (takes)
            {
                scene__set_focus(hit);
            }
            else
            {
                scene__set_focus(NULL_POINTER);
            }

            if (vt != NULL_POINTER && vt->on_mouse_down != NULL_POINTER)
            {
                vt->on_mouse_down(hit, x, y, button);
            }
        }
        else
        {
            scene__set_focus(NULL_POINTER);
        }
        return;
    }

    // mouse up
    gui_node_t* pressed = _scene_input_internal__input.pressed;
    if (pressed != NULL_POINTER && button != _scene_input_internal__input.pressed_button)
    {
        return;
    }
    if (pressed != NULL_POINTER)
    {
        const widget_vtable_t* vt             = widget_registry__get(pressed->type);
        boole                consumes_click = (boole)(vt != NULL_POINTER && (vt->consumes_click || (vt->flags & GUI_WIDGET_FLAGS__CONSUMES_CLICK)));
        if (vt != NULL_POINTER && vt->on_mouse_up != NULL_POINTER)
        {
            vt->on_mouse_up(pressed, x, y, button);
        }
        // A widget callback may destroy the node or cancel this press.
        // Teardown clears the global slot; do not use the old local pointer.
        if (_scene_input_internal__input.pressed != pressed)
        {
            return;
        }

        // Suppress click if the press turned into a pan-scroll. The
        // user's intent was to scroll, not to activate; firing
        // on_click on a wallpaper tile (or any leaf) the moment
        // they release after dragging would be a surprise.
        boole pan_active = _scene_input_internal__pan_drag_active;
        // Rubber band: the finger let go of pulled content, it eases
        // back to the edge (a release on the track flings as before)
        if (pan_active == TRUE && _scene_input_internal__pan_lock_node != NULL_POINTER && _scene_input_internal__pan_lock_node->overscroll_y_held == TRUE)
        {
            scroll__overscroll_release_y(_scene_input_internal__pan_lock_node);
        }
        if (consumes_click == FALSE && hit == pressed && pan_active == FALSE)
        {
            DBG_INPUT log_info("%s %p %s %d", "[dbg-input] dispatch_click on node=", (void*)pressed, "type=", (int)pressed->type);
            scene__dispatch_click(pressed, x, y, button);
        }
        if (_scene_input_internal__input.pressed != pressed)
        {
            return;
        }

        // Release: walk the press chain back. If the mouse is
        // still over the same deepest node, drop the chain into
        // HOVER (mouse is hovering over it post-release). Else
        // drop the chain into DEFAULT.
        if (pressed == hit)
        {
            _scene_input_internal__set_state_chain(pressed, GUI_NODE_STATE__HOVER);
        }
        else
        {
            _scene_input_internal__set_state_chain(pressed, GUI_NODE_STATE__DEFAULT);
        }
        _scene_input_internal__input.pressed = NULL_POINTER;
    }
}

/**
 * @brief deliver a mouse-wheel (or trackpad vertical scroll) event. The scene walks from the node under (x, y) upward to find the nearest ancestor with overflow_y set to scroll or auto AND content larger than its bounds; that ancestor's scroll_y_target is bumped by -delta * line_step (delta is positive when the wheel moves AWAY from the user; we subtract so content scrolls UP in that case, matching every browser + OS convention). If no scrollable ancestor exists the event is silently ignored -- same as a browser. delta is typically +/-1.0 per wheel detent; fractional values come from high-resolution wheels and trackpads and are passed through.
 *
 * @param int64 x -> Pixel x of the wheel event.
 * @param int64 y -> Pixel y of the wheel event.
 * @param float delta -> Normalized wheel delta (positive = scroll up / away from user).
 *
 * @return void
 */
void scene__on_mouse_wheel(int64 x, int64 y, float delta)
{
    gui_node_t* root = scene__root();
    if (root == NULL_POINTER)
    {
        return;
    }

    // one wheel tick moves 40 logical pixels, whatever the display scale
    // (a browser scrolls a fixed number of CSS pixels per tick too)
    const float LINE_STEP_PX = 40.0f * scene__scale();

    gui_node_t* hit = scene__hit_test(root, x, y);
    for (gui_node_t* n = hit; n != NULL_POINTER; n = n->parent)
    {
        const widget_vtable_t* vt = widget_registry__get(n->type);
        if (vt && vt->on_mouse_wheel && vt->on_mouse_wheel(n, x, y, delta)) return;
        gui_overflow_e oy = n->resolved.overflow_y;
        if (oy != GUI_OVERFLOW__SCROLL && oy != GUI_OVERFLOW__AUTO)
        {
            continue;
        }
        if (n->content_h <= n->bounds.h + 0.5f)
        {
            continue;
        }

        // Rubber band (overscroll-behavior: auto): the part of a tick
        // past the edge nudges the content out and it springs back at
        // once; ticks arriving during the spring push it out again.
        if (scroll__overscroll_enabled_y(n) == TRUE && n->overscroll_y_held == FALSE)
        {
            float max_y = n->content_h - n->bounds.h;
            float wanted = n->scroll_y_target - delta * LINE_STEP_PX;
            float excess = 0.0f;

            if (max_y < 0.0f)
            {
                max_y = 0.0f;
            }
            if (wanted > max_y)
            {
                excess = wanted - max_y;
            }
            else if (wanted < 0.0f)
            {
                excess = wanted;
            }
            n->scroll_y_target = wanted;
            scroll__clamp(n);
            if (excess != 0.0f)
            {
                scroll__overscroll_push_y(n, excess);
                scroll__overscroll_release_y(n);
            }
            n->scroll_last_active_ms = (int64)scene__frame_time_ms();
            if (scroll__animated(&n->resolved) == FALSE)
            {
                n->scroll_y = n->scroll_y_target;
            }
            return;
        }
        n->scroll_y_target -= delta * LINE_STEP_PX;
        scroll__clamp(n);
        n->scroll_last_active_ms = (int64)scene__frame_time_ms();
        // scroll-smooth and scroll-end-effect: wobble move scroll_y in the animator
        if (scroll__animated(&n->resolved) == FALSE)
        {
            n->scroll_y = n->scroll_y_target;
        }
        return;
    }
    (void)x; (void)y; (void)delta;
}

/**
 * @brief return the currently pressed node -- the one that received the most recent mouse/touch DOWN and hasn't been released yet. NULL when nothing is pressed. Used by the platform touch layer to ask "should I convert this finger drag into a scroll, or leave it on the pressed widget?" -- see the captures_drag flag on widget_vtable_t.
 *
 * @return gui_node_t* -> void
 */
gui_node_t* scene__pressed(void)
{
    return _scene_input_internal__input.pressed;
}

/**
 * @brief cancel the current press without dispatching `on_click`. Drops the pressed slot to NULL and walks the press chain back to default state. Use when host code decides to take over the touch midway through (custom drag, long-press promotion, modal takeover, etc.) -- the next `mouse_up` sees `pressed == NULL` and skips the normally-scheduled click on the underlying widget. Idempotent: no-op if no node is currently pressed.
 *
 * @return void
 */
void scene__cancel_press(void)
{
    gui_node_t* p = _scene_input_internal__input.pressed;
    if (p == NULL_POINTER)
    {
        return;
    }
    // Walk the press chain back to default state. Mirrors the
    // release branch of scene__on_mouse_button when the up lands
    // off the originally-pressed node -- minus the click dispatch.
    // The next mouse_up sees pressed == NULL and the click is
    // skipped on the underlying widget.
    _scene_input_internal__set_state_chain(p, GUI_NODE_STATE__DEFAULT);
    _scene_input_internal__input.pressed = NULL_POINTER;
}

/**
 * @brief instant scroll from a touch drag. Identical to on_mouse_wheel in that it walks up from the hit node to the nearest scrollable ancestor, but bypasses scroll-smooth lerping: both scroll_y and scroll_y_target are updated together so the container follows the finger 1:1 with no lag. Used by the platform touch handler.
 *
 * @param int64 x -> Finger x in pixels.
 * @param int64 y -> Finger y in pixels.
 * @param float delta_pixels -> Finger travel in pixels since last move. Positive = finger moved DOWN (scroll goes to earlier content; scroll_y decreases).
 *
 * @return void
 */
void scene__on_touch_scroll(int64 x, int64 y, float delta_pixels)
{
    gui_node_t* root = scene__root();
    if (root == NULL_POINTER)
    {
        return;
    }

    gui_node_t* hit = scene__hit_test(root, x, y);
    for (gui_node_t* n = hit; n != NULL_POINTER; n = n->parent)
    {
        gui_overflow_e oy = n->resolved.overflow_y;
        if (oy != GUI_OVERFLOW__SCROLL && oy != GUI_OVERFLOW__AUTO)
        {
            continue;
        }
        if (n->content_h <= n->bounds.h + 0.5f)
        {
            continue;
        }

        n->scroll_y_target -= delta_pixels;
        scroll__clamp(n);
        n->scroll_last_active_ms = (int64)scene__frame_time_ms();
        if (n->resolved.scroll_smooth_ms <= 0.0f)
        {
            n->scroll_y = n->scroll_y_target;
        }
        return;
    }
    (void)x; (void)y; (void)delta_pixels;
}

/**
 * @brief notification that the platform viewport changed size. Currently a no-op (layout re-runs every frame); reserved for dirty-tracking.
 *
 * @param int64 w -> New viewport width.
 * @param int64 h -> New viewport height.
 *
 * @return void
 */
void scene__on_resize(int64 w, int64 h)
{
    (void)w; (void)h;
}

/**
 * @brief set which node receives keyboard input. Pass NULL to clear focus. The previously-focused node is automatically de-focused.
 *
 * @param gui_node_t* node -> Node to focus, or NULL.
 *
 * @return void
 */
void scene__set_focus(gui_node_t* node)
{
    gui_node_t* prev = _scene_input_internal__input.focus;
    if (prev == node)
    {
        return;
    }

    if (prev != NULL_POINTER)
    {
        const widget_vtable_t* pvt = widget_registry__get(prev->type);
        widget_navigation__remember_focus(prev);
        if (pvt != NULL_POINTER && pvt->on_focus_lost != NULL_POINTER)
        {
            pvt->on_focus_lost(prev);
        }
    }
    _scene_input_internal__input.focus = node;
    if (node != NULL_POINTER)
    {
        const widget_vtable_t* nvt = widget_registry__get(node->type);
        if (nvt != NULL_POINTER && nvt->on_focus_gained != NULL_POINTER)
        {
            nvt->on_focus_gained(node);
        }
    }

    // a select of select-style: search takes typed text for its filter, like an input
    boole was_text = prev != NULL_POINTER && (prev->type == GUI_NODE_TYPE__INPUT || prev->type == GUI_NODE_TYPE__TEXTAREA || (prev->type == GUI_NODE_TYPE__SELECT && prev->resolved.select_style == GUI_SELECT_STYLE__SEARCH));
    boole is_text  = node != NULL_POINTER && (node->type == GUI_NODE_TYPE__INPUT || node->type == GUI_NODE_TYPE__TEXTAREA || (node->type == GUI_NODE_TYPE__SELECT && node->resolved.select_style == GUI_SELECT_STYLE__SEARCH));

    // Platform-level text-input notification. The wayland-client
    // backend uses this to fire zwp_text_input_v3.enable/disable so
    // the system compositor can ask its shell to show an out-of-
    // process keyboard. When this hook is installed we DON'T also
    // attach an in-tree widget_keyboard -- that would duplicate the
    // keyboard inside the app's own surface, on top of the shell's.
    if (_scene_input_internal__text_input_focus_hook != NULL_POINTER)
    {
        if      (is_text && was_text == FALSE)
        {
            _scene_input_internal__text_input_focus_hook(TRUE);
        }
        else if (is_text == FALSE && was_text)
        {
            _scene_input_internal__text_input_focus_hook(FALSE);
        }
    }
    else if (scene__virtual_keyboard_enabled())
    {
        if      (is_text && was_text == FALSE)
        {
            scene__show_keyboard();
        }
        else if (is_text == FALSE && was_text)
        {
            scene__hide_keyboard();
        }
    }
}

/**
 * @brief get the currently-focused node, or NULL.
 *
 * @return gui_node_t* -> The focused node, or NULL.
 */
gui_node_t* scene__focus(void)
{
    return _scene_input_internal__input.focus;
}

/**
 * @brief deliver a character typed by the user to the currently-focused node's widget vtable (on_char hook). Called by platform_win32 on WM_CHAR. No-op if no node is focused or if the focused widget doesn't define on_char.
 *
 * @param uint codepoint -> Unicode codepoint (ASCII+Latin-1 subset for now).
 *
 * @return void
 */
void scene__on_char(uint codepoint)
{
    // Redirect hook. Used by meek-shell's IME bridge to forward the
    // codepoint to an external sink (the zwp_text_input_v3 in the
    // foreign app's process) instead of a local widget. Hook returns
    // TRUE if it handled the char; we skip local delivery in that
    // case. FALSE (or hook not set) falls through to the focused
    // widget's on_char vtable.
    if (_scene_input_internal__char_redirect != NULL_POINTER && _scene_input_internal__char_redirect(codepoint))
    {
        return;
    }
    gui_node_t* f = _scene_input_internal__input.focus;
    if (f == NULL_POINTER)
    {
        return;
    }
    const widget_vtable_t* vt = widget_registry__get(f->type);
    if (vt == NULL_POINTER || vt->on_char == NULL_POINTER)
    {
        return;
    }
    vt->on_char(f, codepoint);
}

/**
 * @brief deliver a non-character key event (BACKSPACE, ENTER, arrows, ...) to the focused node's widget. Called by platform_win32 on WM_KEYDOWN / WM_KEYUP.
 *
 * @param int64 vk -> Win32 virtual-key code.
 * @param boole down -> TRUE on press, FALSE on release.
 *
 * @return void
 */
void scene__on_key(int64 vk, boole down)
{
    _scene_input_internal__key_modifier(vk, down);
    gui_node_t* f = _scene_input_internal__input.focus;
    if (f == NULL_POINTER)
    {
        return;
    }
    const widget_vtable_t* vt = widget_registry__get(f->type);
    if (vt == NULL_POINTER || vt->on_key == NULL_POINTER)
    {
        return;
    }
    vt->on_key(f, vk, down);
}

/**
 * @brief route a key / character event to a specific node. Used by the on-screen keyboard to deliver taps to whichever node currently holds focus. Bypasses the normal platform input dispatcher.
 *
 * @param gui_node_t* node -> Target. NULL is a no-op.
 * @param uint codepoint -> Unicode codepoint (ASCII/Latin-1 for now).
 *
 * @return void
 */
void scene__deliver_char(gui_node_t* node, uint codepoint)
{
    // Redirect hook takes priority over node-targeted delivery too.
    // This is the path widget_keyboard actually uses (it calls
    // scene__deliver_char(focus, ch) after a key tap). With the
    // hook set, keyboard taps forward to the IME bridge regardless
    // of the local focus target.
    if (_scene_input_internal__char_redirect != NULL_POINTER && _scene_input_internal__char_redirect(codepoint))
    {
        return;
    }
    if (node == NULL_POINTER)
    {
        return;
    }
    const widget_vtable_t* vt = widget_registry__get(node->type);
    if (vt == NULL_POINTER || vt->on_char == NULL_POINTER)
    {
        return;
    }
    vt->on_char(node, codepoint);
}

/**
 * @brief scene__deliver_key
 *
 * @return void
 */
void scene__deliver_key(gui_node_t* node, int64 vk, boole down)
{
    _scene_input_internal__key_modifier(vk, down);
    if (node == NULL_POINTER)
    {
        return;
    }
    const widget_vtable_t* vt = widget_registry__get(node->type);
    if (vt == NULL_POINTER || vt->on_key == NULL_POINTER)
    {
        return;
    }
    vt->on_key(node, vk, down);
}
