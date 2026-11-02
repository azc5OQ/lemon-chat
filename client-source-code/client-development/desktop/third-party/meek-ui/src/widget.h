#ifndef WIDGET_H
#define WIDGET_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"
#include "memory_types.h"

// widget.h - the per-control vtable.
// each gui_node_type_e has exactly one widget_vtable_t registered for it
// (via widget_registry__register, called from each widget's
// widget_<name>__register function). scene.c, parser_xml.c, and the
// input dispatcher look up the vtable by node type and call into the
// hooks below instead of switching on type.
// to add a new control:
// 1. add an enum value to gui_node_type_e in gui.h (before
// GUI_NODE_TYPE__COUNT).
// 2. create gui/src/widgets/widget_<name>.c that defines a static
// widget_vtable_t, fills in only the hooks it needs, and exposes
// a single widget_<name>__register(void).
// 3. add the extern declaration + the call to that register
// function in widget_registry.c's bootstrap.
// hooks left as NULL fall through to safe defaults (no-op behavior,
// bg-only draw, no input handling).

// Widget capability flags. Future-proofs the vtable: new bits can
// be added without growing the struct. The four legacy boole
// fields below (consumes_click / takes_focus / captures_drag /
// preserves_focus) are kept for backwards compatibility -- scene
// reads BOTH the bit and the boole and treats either-set as TRUE.
// New widgets can use either form; eventually the booles get
// removed once nothing references them.
typedef enum gui_widget_flags_e
{
    GUI_WIDGET_FLAGS__NONE            = 0,
    GUI_WIDGET_FLAGS__CONSUMES_CLICK  = 1 << 0,
    GUI_WIDGET_FLAGS__TAKES_FOCUS     = 1 << 1,
    GUI_WIDGET_FLAGS__CAPTURES_DRAG   = 1 << 2,
    GUI_WIDGET_FLAGS__PRESERVES_FOCUS = 1 << 3,
} gui_widget_flags_e;

// per-control vtable. registered once per gui_node_type_e. scene.c and
// parser_xml.c call through this instead of switching on type.
typedef struct widget_vtable_t
{
    // xml tag name + selector match string. e.g. "Button". required.
    char* type_name;                                                                             // 0x0

    // called once after parser_xml allocates a node of this type, before
    // any attributes are parsed. set per-type defaults here (e.g. slider
    // value range). may be NULL.
    void  (*init_defaults)(gui_node_t* n);                                                         // 0x8

    // called once from scene__node_free, BEFORE the node's children are
    // freed and BEFORE node memory is released. the right place to
    // release any per-node heap / GPU resources the widget allocated
    // during init_defaults or apply_attribute (e.g. widget_image owns a
    // renderer texture). may be NULL.
    void  (*on_destroy)(gui_node_t* n);                                                            // 0x10

    // called by parser_xml for each attribute that isn't a generic one
    // (id, class, on_click, on_change are handled by the parser itself).
    // return TRUE to consume the attribute; FALSE to let the parser log
    // "unknown attribute". may be NULL (equivalent to always returning FALSE).
    boole (*apply_attribute)(gui_node_t* n, char* name, char* value);                              // 0x18

    // compute the node's bounds inside [x, y, avail_w, avail_h] and lay
    // out children if any. dispatch into children via scene__layout_node;
    // use scene__layout_children_as_column for the standard column
    // arrangement. required.
    void  (*layout)(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale);   // 0x20

    // emit one or more renderer__submit_rect calls for this node. NULL
    // falls through to "submit one rect for resolved.bg if has_bg is set".
    // Scene calls this BEFORE recursing into children. So submissions
    // here render UNDER the children. Use emit_draws_post (below) for
    // anything that needs to land ON TOP of children -- scrollbars,
    // focus rings, hover highlights.
    void  (*emit_draws)(gui_node_t* n, float scale);                                               // 0x28

    // Like emit_draws, but called AFTER scene has recursed through this
    // node's children. Submissions here render on top of children.
    // Also the right place to call renderer__pop_scissor when the
    // matching push_scissor was issued in emit_draws -- the push/pop
    // pair brackets the children's draws so they get clipped.
    void  (*emit_draws_post)(gui_node_t* n, float scale);                                          // 0x30

    // input hooks. all may be NULL; scene's default semantics apply.
    // on_mouse_down / on_mouse_up: called when the press / release
    // landed on this node. return value is ignored for now (reserved).
    // on_mouse_drag: called on every mouse_move while this node is
    // the "pressed" node. return TRUE to suppress hover updates
    // (drag-mode widgets); FALSE to let scene update hover normally.
    // on_char: called when a character is typed while this node has
    // focus. codepoint is the Unicode codepoint (we currently
    // forward only the ASCII+Latin-1 subset). Input uses this to
    // append to node->text.
    // on_key: called when a non-character key is pressed while this
    // node has focus. vk is the Win32 virtual-key code (VK_BACK,
    // VK_RETURN, VK_LEFT, etc.). Input uses this for backspace.
    // consumes_click: if TRUE, scene skips the on_click dispatch on
    // mouse_up. used by drag-only widgets like sliders.
    // takes_focus: if TRUE, a mouse-down on this node sets it as the
    // scene's focused node. used by Input to capture keyboard.
    void  (*on_mouse_down)(gui_node_t* n, int64 x, int64 y, int64 button);                         // 0x38
    void  (*on_mouse_up)(gui_node_t* n, int64 x, int64 y, int64 button);                           // 0x40
    boole (*on_mouse_drag)(gui_node_t* n, int64 x, int64 y);                                       // 0x48
    void  (*on_char)(gui_node_t* n, uint codepoint);                                               // 0x50
    void  (*on_key)(gui_node_t* n, int64 vk, boole down);                                          // 0x58
    boole consumes_click;                                                                        // 0x60
    boole takes_focus;                                                                           // 0x61
    // captures_drag = TRUE on widgets that need finger-drag events
    // routed to themselves (canvas painting, colorpicker HSV square
    // drags, etc.). The Android touch state machine normally
    // transitions to SCROLLING after ~16 px of vertical finger
    // movement and redirects the delta to the nearest scrollable
    // ancestor; setting captures_drag pins the drag on this widget
    // so the state machine keeps forwarding it as plain mouse_move.
    // No-op on desktop (mouse drags always reach the pressed node).
    boole captures_drag;                                                                         // 0x62
    // preserves_focus = TRUE on widgets that should NOT disturb the
    // scene's focused node when they're pressed. Used by the
    // on-screen keyboard: tapping one of its keys hits the keyboard
    // node, but the edited widget (the <input> / <textarea>) has to
    // keep focus so the key press can be routed to it via
    // scene__deliver_char. Without this flag, scene would treat the
    // keyboard tap as "user clicked something that doesn't want
    // focus" and clear focus -- which triggers the auto-hide-
    // keyboard gate, so the keyboard would vanish the instant you
    // tapped a key.
    boole preserves_focus;                                                                       // 0x63

    // ===== widget-state auto-alloc ===========================================
    // state_size: when > 0, scene auto-allocates this many bytes via
    // GUI_CALLOC_T(state_size, MM_TYPE__GENERIC) on first access via
    // scene__widget_state(n) and auto-frees them in scene__node_free
    // AFTER the on_destroy hook returns. Lets new widgets skip the
    // boilerplate `_widget_X_internal__state_of(n)` lazy-alloc
    // function + the matching GUI_FREE in on_destroy. on_destroy is
    // still called first so widgets can release GPU textures /
    // sub-allocations stashed inside the state struct before the
    // outer GUI_FREE happens.
    // Existing widgets that do their own GUI_MALLOC + GUI_FREE inside
    // init_defaults / apply_attribute / on_destroy keep working
    // unchanged: they leave state_size at 0 and scene's auto-free
    // is skipped.
    int64 state_size;                                                                            // 0x68

    // ===== additional lifecycle hooks ========================================
    // on_attach     called once after scene__add_child links this
    // node into the tree. Useful for widgets that need
    // to register with a global system (image cache
    // warmup, canvas buffer preallocation, etc.) at a
    // point where the node already has a parent but
    // hasn't yet been laid out or drawn.
    // on_focus_gained / on_focus_lost
    // fired from scene__set_focus on the node that's
    // gaining / losing focus. Lets widgets show / hide
    // carets, replay an attention animation, scroll
    // themselves into view, etc., without polling
    // scene__focus() each frame.
    // All three are optional; NULL means no-op.
    void (*on_attach)(gui_node_t* n, gui_node_t* parent);                                            // 0x70
    void (*on_focus_gained)(gui_node_t* n);                                                        // 0x78
    void (*on_focus_lost)(gui_node_t* n);                                                          // 0x80

    // Capability bitfield. Combined with the legacy individual
    // boole fields above; scene checks both and treats either-set
    // as TRUE. Lets new widgets opt in via a single OR'd literal
    // instead of N separate `.foo = TRUE,` lines, and adding the
    // next capability is a bit, not a struct field.
    gui_widget_flags_e flags;                                                                      // 0x88

    // Hot-reload reconciliation policy for user_data.
    // FALSE (default): the parser's freshly-allocated user_data on
    // the new tree wins. Old's user_data is freed via on_destroy
    // when the old tree gets torn down. Right for widgets whose
    // user_data carries declarative state from the .ui (popup
    // kind / labels, image src / texture).
    // TRUE: the OLD tree's user_data gets transferred to the new
    // tree's matched node. The new node's parser-allocated
    // user_data is freed via on_destroy first. Right for widgets
    // whose user_data carries pure runtime state that the user
    // built up (canvas pixels, colorpicker HSV selection).
    boole preserve_user_data;                                                                    // 0x8C
    // Optional wheel consumer, offered the event before ancestor scrolling.
    boole (*on_mouse_wheel)(gui_node_t* n, int64 x, int64 y, float delta);
} widget_vtable_t;

#endif
