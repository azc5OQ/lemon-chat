#ifndef GUI_H
#define GUI_H

#include "types.h"

// gui.h - common types shared by every module of the toolkit.
// declarations only. per-module .h files (scene.h, renderer.h,
// platform_win32.h, parser_xml.h) declare module-specific apis.

// rgba colour, components in 0..1.
typedef struct gui_color_t
{
    float r;   // 0x0
    float g;   // 0x4
    float b;   // 0x8
    float a;   // 0xC
} gui_color_t;

// 2d float vector, used for points and sizes.
typedef struct gui_vec2_t
{
    float x;   // 0x0
    float y;   // 0x4
} gui_vec2_t;

// axis-aligned rectangle, top-left origin, in pixels.
typedef struct gui_rect_t
{
    float x;   // 0x0
    float y;   // 0x4
    float w;   // 0x8
    float h;   // 0xC
} gui_rect_t;

// scene graph node kinds. parser_xml maps tag names to these.
// more kinds (text, image, stack, grid, input) arrive in later
// iterations.
typedef enum gui_node_type_e
{
    GUI_NODE_TYPE__WINDOW,
    GUI_NODE_TYPE__TABLE,
    GUI_NODE_TYPE__TR,
    GUI_NODE_TYPE__BUTTON,
    GUI_NODE_TYPE__SLIDER,
    GUI_NODE_TYPE__DIV,
    GUI_NODE_TYPE__TEXT,
    GUI_NODE_TYPE__INPUT,
    GUI_NODE_TYPE__CHECKBOX,
    GUI_NODE_TYPE__RADIO,
    GUI_NODE_TYPE__SELECT,
    GUI_NODE_TYPE__OPTION, // child of <select>; carries one option's text + value.
    GUI_NODE_TYPE__IMAGE, // <image src="..."/> -- PNG/JPEG loaded via stb_image.
    GUI_NODE_TYPE__COLLECTION, // <collection> -- grid / list / flow of child nodes.
    GUI_NODE_TYPE__COLOR_PICKER, // <color-picker mode="slider|square"> -- hue strip or HSV square.
    GUI_NODE_TYPE__POPUP, // <popup> -- modal overlay with body + OK / yes-no buttons.
    GUI_NODE_TYPE__TEXTAREA, // <textarea> -- multi-line text input with word-wrap.
    GUI_NODE_TYPE__CANVAS, // <canvas> -- RGBA8 drawing surface (paint-app style).
    GUI_NODE_TYPE__KEYBOARD, // <keyboard> -- on-screen ASCII keyboard for touch displays.
    GUI_NODE_TYPE__PROCESS_WINDOW, // <process-window> -- live texture of another process's rendered buffer (compositor forwards via meek_shell_v1).
    GUI_NODE_TYPE__NAVIGATION, // <navigation> -- page-stack container: shows exactly one child page at a time, push/pop API for drill-in navigation.
    GUI_NODE_TYPE__DATEPICKER, // <datepicker> -- inline month calendar; the selection is node->text as "YYYY-MM-DD", on_change carries ev->date.
    GUI_NODE_TYPE__HEXVIEW, // <hexview> -- memory viewer over a host buffer (widget_hexview__set_buffer); on_change carries the byte offset.
    GUI_NODE_TYPE__FILEPICKER, // <filepicker> -- directory browser (fs__list_dir); on_change carries ev->file.
    GUI_NODE_TYPE__TD,
    GUI_NODE_TYPE__TH,
    GUI_NODE_TYPE__COUNT // sentinel; sizes the widget vtable table. add new node kinds before this line.
} gui_node_type_e;

// per-state style slots. the resolver picks the slot matching n->state
// (default/hover/pressed/disabled) each frame and copies it into
// `resolved`. the APPEAR and DISAPPEAR slots are NOT regular states --
// the resolver never selects them based on n->state. instead they hold
// the STARTING values for an :appear animation (where the node fades
// in from those values to the default-state values) and the FINAL
// values for a :disappear animation (where the node fades to those
// values before being removed). the animator reads them, not the
// resolver.
typedef enum gui_node_state_e
{
    GUI_NODE_STATE__DEFAULT   = 0,
    GUI_NODE_STATE__HOVER     = 1,
    GUI_NODE_STATE__PRESSED   = 2,
    GUI_NODE_STATE__DISABLED  = 3,
    GUI_NODE_STATE__APPEAR    = 4, // starting values for the :appear animation
    GUI_NODE_STATE__DISAPPEAR = 5, // ending values for the :disappear animation (deferred)
    GUI_NODE_STATE__COUNT     = 6
} gui_node_state_e;

// easing functions for animations. matches the named curves CSS
// exposes via cubic-bezier shortcuts. linear is the identity; the
// other three sit at the fast-start, slow-start, and slow-start-and-end
// positions on the speed envelope.
// eased(t) maps a normalized progress t in [0,1] to the eased value
// in [0,1]:
// LINEAR        eased = t                      constant speed.
// EASE_IN       eased = t * t                  starts slow, ends fast.
// EASE_OUT      eased = 1 - (1-t)^2            starts fast, ends slow. natural for "pop in" effects.
// EASE_IN_OUT   smooth at both ends, fast in the middle (cubic).
typedef enum gui_easing_e
{
    GUI_EASING__LINEAR           = 0,
    GUI_EASING__EASE_IN          = 1,
    GUI_EASING__EASE_OUT         = 2, // default for :appear -- the most natural "pop"
    GUI_EASING__EASE_IN_OUT      = 3,

    // CSS-derived keyword presets. `ease` is the default CSS transition
    // timing function (cubic-bezier(0.25, 0.1, 0.25, 1.0)). The *_BACK
    // variants overshoot past the end value and ease back — good for
    // springy panel pops. *_BOUNCE decelerates in diminishing oscillations
    // as if the element were a dropped ball settling on the target.
    GUI_EASING__EASE             = 4,
    GUI_EASING__EASE_IN_BACK     = 5,
    GUI_EASING__EASE_OUT_BACK    = 6,
    GUI_EASING__EASE_IN_OUT_BACK = 7,
    GUI_EASING__EASE_OUT_BOUNCE  = 8,

    // Parameterised curves. Values are read from the matching
    // `_easing_params[4]` slot on the style (appear_easing_params,
    // animation_easing_params, transition_easing_params). The parser
    // populates the slot when these kinds are chosen; the animator
    // reads them at apply time.
    // CUBIC_BEZIER: params = { x1, y1, x2, y2 } — same convention as
    // CSS `cubic-bezier(x1, y1, x2, y2)`. The curve goes (0,0) -> P1
    // -> P2 -> (1,1) with the two control points shaping the arc.
    // SPRING: params = { stiffness, damping, _, _ } — see animator.c
    // for the exponentially-damped-oscillator formula. Higher
    // stiffness = more oscillations in the visible range, higher
    // damping = faster settle. `spring(20, 5)` is a mild 2-3 bounce.
    GUI_EASING__CUBIC_BEZIER     = 9,
    GUI_EASING__SPRING           = 10
} gui_easing_e;

// css-like overflow modes for a single axis. controls both clipping
// (not yet implemented -- see widget_div.c note) and whether a scrollbar
// is drawn.
// GUI_OVERFLOW__VISIBLE  default; content can draw outside the bounds;
// no scrollbar; no clipping.
// GUI_OVERFLOW__HIDDEN   content is clipped to bounds (once renderer
// scissor lands); no scrollbar shown.
// GUI_OVERFLOW__SCROLL   always show a scrollbar for the axis even if
// content fits; clip to bounds.
// GUI_OVERFLOW__AUTO     show a scrollbar only if content exceeds
// bounds on that axis; clip to bounds.
// the two "scroll" modes (SCROLL, AUTO) consume scroll input on the
// containing div and offset children by scroll_x / scroll_y.
typedef enum gui_overflow_e
{
    GUI_OVERFLOW__VISIBLE = 0, // default; no clip, no scrollbar.
    GUI_OVERFLOW__HIDDEN  = 1, // clip (when supported); no scrollbar.
    GUI_OVERFLOW__SCROLL  = 2, // always show bar; clip.
    GUI_OVERFLOW__AUTO    = 3 // show bar only when content exceeds bounds; clip.
} gui_overflow_e;

// css-like "is this element painted?" switch. orthogonal to display:
// a hidden element still reserves layout space, its neighbours stay
// put. Closely mirrors the CSS `visibility` property, but it doesn't
// cascade in our implementation -- each node decides for itself.
// Children of a hidden parent continue to paint based on their own
// resolved.visibility. If you want to hide a subtree entirely, use
// GUI_DISPLAY__NONE on the parent.
// GUI_VISIBILITY__VISIBLE  default; node's emit_draws runs normally.
// GUI_VISIBILITY__HIDDEN   node itself is skipped by emit_draws, but
// layout treats it as fully present (same
// bounds, children in their usual places).
typedef enum gui_visibility_e
{
    GUI_VISIBILITY__VISIBLE = 0, // default; drawn.
    GUI_VISIBILITY__HIDDEN  = 1 // not drawn; still reserves layout space.
} gui_visibility_e;

// css-like "is this element in the layout?" switch. Orthogonal to
// visibility: display: none removes the element AND its subtree from
// both layout and rendering, so siblings collapse into the space it
// would otherwise take.
// Only two values in this PoC; the HTML "block / inline / flex / grid /
// table ..." distinctions collapse into a single "participates in
// layout" vs "doesn't exist" bit because we only have one layout model
// (column + row + fill). Future layouts (grid etc.) could grow this.
// GUI_DISPLAY__BLOCK   default; node participates normally.
// GUI_DISPLAY__NONE    node and its entire subtree are skipped for
// layout and rendering; parent's column/row
// layout advances PAST this child as if it
// weren't there.
typedef enum gui_display_e
{
    GUI_DISPLAY__BLOCK = 0, // default; laid out and drawn.
    GUI_DISPLAY__NONE  = 1, // excluded from both layout and rendering.
    GUI_DISPLAY__FLEX = 2
} gui_display_e;

// CSS positioning model, same names and same rules as the web:
//   static    default; normal column / row flow.
//   relative  flows normally, then the box is shifted by top/left
//             (or bottom/right when those are the ones set) without
//             moving its siblings.
//   absolute  lifted out of the flow; siblings act as if it did not
//             exist. Anchored by top/right/bottom/left to the content
//             rect of the nearest ancestor whose position is not
//             static (the root when there is none). Two opposite edges
//             stretch the box between them.
//   fixed     like absolute but anchored to the viewport, anywhere in
//             the tree, unaffected by scrolling.
//   sticky    flows normally; once the nearest scrolling ancestor
//             scrolls it past a set edge it pins to that edge, but
//             never leaves its parent's content box.
// Positioned elements (anything but static) paint above their static
// siblings, and z-index orders siblings above that, as in CSS.
typedef enum gui_position_e
{
    GUI_POSITION__STATIC   = 0, // default; participates in column/row flow.
    GUI_POSITION__RELATIVE = 1, // flows normally, then shifts by inset_l/t (without affecting siblings).
    GUI_POSITION__ABSOLUTE = 2, // lifted out of flow, anchored to nearest positioned ancestor's content rect via insets.
    GUI_POSITION__FIXED    = 3, // anchored to the viewport via insets, anywhere in the tree, ignores scrolling.
    GUI_POSITION__STICKY   = 4, // flows normally; pins to the nearest scrolling ancestor's visible edge once scrolled past it, within its parent's box.
} gui_position_e;

// Direction keywords of the two-color form (`background-gradient: <from>
// <to> [direction]`, renderer__submit_rect_gradient, hosts that set a
// gradient from C). gradient__two_color turns them into a gui_gradient_t:
// vertical is CSS `to bottom`, horizontal `to right`, diagonal-tl `to
// bottom right`, diagonal-tr `to bottom left`.
typedef enum gui_gradient_dir_e
{
    GUI_GRADIENT_DIR__VERTICAL     = 0, // top -> bottom (default).
    GUI_GRADIENT_DIR__HORIZONTAL   = 1, // left -> right.
    GUI_GRADIENT_DIR__DIAGONAL_TL  = 2, // top-left -> bottom-right.
    GUI_GRADIENT_DIR__DIAGONAL_TR  = 3 // top-right -> bottom-left.
} gui_gradient_dir_e;

// A CSS gradient as the style carries it: `linear-gradient(...)` or
// `radial-gradient(...)` with color stops. src/gradient.c evaluates it
// (CSS Images 3 geometry, premultiplied sRGB stop interpolation) and
// tessellates it for the renderers.
typedef enum gui_gradient_kind_e
{
    GUI_GRADIENT_KIND__LINEAR = 0,
    GUI_GRADIENT_KIND__RADIAL = 1,
    GUI_GRADIENT_KIND__CONIC  = 2
} gui_gradient_kind_e;

// The size of a radial gradient's ending shape (CSS <radial-size>).
typedef enum gui_gradient_size_e
{
    GUI_GRADIENT_SIZE__FARTHEST_CORNER = 0, // default: the shape passes through the corner farthest from the center
    GUI_GRADIENT_SIZE__CLOSEST_SIDE    = 1,
    GUI_GRADIENT_SIZE__FARTHEST_SIDE   = 2,
    GUI_GRADIENT_SIZE__CLOSEST_CORNER  = 3,
    GUI_GRADIENT_SIZE__EXPLICIT        = 4  // radius_x / radius_y
} gui_gradient_size_e;

#define GUI_GRADIENT_MAX_STOPS 16

// One color stop. Without a position the CSS fixup spreads it evenly
// between its positioned neighbours (the first and last default to the
// ends). A position is a fraction of the gradient line (percent / 100)
// or, with pos_is_px, logical pixels along it.
typedef struct gui_gradient_stop_t
{
    gui_color_t color;      // 0x0
    float       pos;        // 0x10
    boole       has_pos;    // 0x14
    boole       pos_is_px;  // 0x15
    float       px_offset; // 0x18, additive pixel component of a calc stop
} gui_gradient_stop_t;

typedef struct gui_gradient_t
{
    gui_gradient_kind_e kind;        // 0x0
    // linear: the CSS angle, 0 = to top, 90 = to right, 180 = to bottom (default).
    float               angle_deg;   // 0x4
    // linear `to <side-or-corner>`: -1 / +1 per axis, 0 when the angle applies;
    // a corner (both set) runs perpendicular to the diagonal of the other two corners.
    int                 corner_x;    // 0x8
    int                 corner_y;    // 0xC
    // radial: a circle (else an ellipse) and how its size is chosen.
    boole               circle;      // 0x10
    gui_gradient_size_e size;        // 0x14
    // radial EXPLICIT size: logical pixels, or percent of the box with radius_pct.
    float               radius_x;    // 0x18
    float               radius_y;    // 0x1C
    boole               radius_pct;  // 0x20
    // radial center: fraction of the box (0.5 = center) or logical pixels with center_*_px.
    float               center_x;    // 0x24
    float               center_y;    // 0x28
    boole               center_x_px; // 0x2C
    boole               center_y_px; // 0x2D
    int                 stop_count;  // 0x30
    gui_gradient_stop_t stops[GUI_GRADIENT_MAX_STOPS]; // 0x34
    boole               repeating;   // 0x1F4
} gui_gradient_t;

// Layout mode for <collection>. GRID: fixed number of columns, rows
// grown to fit; each cell sized to item_size (or auto from child).
// LIST: single-row strip, children placed horizontally. FLOW: wrap
// children onto as many rows as fit in the available width.
typedef enum gui_collection_layout_e
{
    GUI_COLLECTION_LAYOUT__GRID = 0,
    GUI_COLLECTION_LAYOUT__LIST = 1,
    GUI_COLLECTION_LAYOUT__FLOW = 2
} gui_collection_layout_e;

// How an image is fitted inside a container's bounds. Mirrors CSS's
// `object-fit` / `background-size` keyword set.
// FILL      stretch to fully fill bounds, breaking aspect ratio.
// same as the pre-property <image> behavior.
// CONTAIN   scale uniformly until the LONGER side fits, letterbox
// the rest. whole image is always visible.
// COVER     scale uniformly until the SHORTER side fills, crop the
// overflow via renderer scissor. image fills bounds but
// some of it is clipped.
// NONE      draw at the image's natural pixel size, centered inside
// bounds. if the image is bigger than bounds, the excess
// is scissored off.
// Accepted spellings in .style: `fill`, `stretch` (alias for fill),
// `contain`, `cover`, `none`. Default is FILL for <image> (preserves
// the old behavior) and FILL for container background-size too.
typedef enum gui_image_fit_e
{
    GUI_IMAGE_FIT__FILL    = 0,
    GUI_IMAGE_FIT__CONTAIN = 1,
    GUI_IMAGE_FIT__COVER   = 2,
    GUI_IMAGE_FIT__NONE    = 3
} gui_image_fit_e;

// Horizontal alignment of children inside a column/div/window (applies
// per-child within the container's inner width), or of the WHOLE ROW
// of children inside a row container (group-level).
typedef enum gui_halign_e
{
    GUI_HALIGN__LEFT   = 0,
    GUI_HALIGN__CENTER = 1,
    GUI_HALIGN__RIGHT  = 2
} gui_halign_e;

// Vertical alignment. Mirror of gui_halign_e. In a column-style
// container it positions the WHOLE STACK of children within the
// parent's inner height; in a row-style container it applies
// per-child.
typedef enum gui_valign_e
{
    GUI_VALIGN__TOP    = 0,
    GUI_VALIGN__CENTER = 1,
    GUI_VALIGN__BOTTOM = 2
} gui_valign_e;

// CSS flex alignment along a container's main axis (vertical in a column,
// horizontal in a row): justify-content. UNSET falls back to the legacy
// vertical-alignment / horizontal-alignment of the container.
typedef enum gui_justify_e
{
    GUI_JUSTIFY__UNSET         = 0,
    GUI_JUSTIFY__START         = 1,
    GUI_JUSTIFY__CENTER        = 2,
    GUI_JUSTIFY__END           = 3,
    GUI_JUSTIFY__SPACE_BETWEEN = 4, // first child at the start, last at the end, equal space between
    GUI_JUSTIFY__SPACE_AROUND  = 5, // equal space around each child, half of it at the edges
    GUI_JUSTIFY__SPACE_EVENLY  = 6  // equal space between the children and at both edges
} gui_justify_e;

// CSS flex alignment across a container's main axis: align-items on the
// container, align-self on a child. STRETCH behaves as START: containers
// already take the cross size a column or row offers and text hugs its
// glyphs, which is the toolkit's stretch.
typedef enum gui_align_e
{
    GUI_ALIGN__UNSET   = 0,
    GUI_ALIGN__START   = 1,
    GUI_ALIGN__CENTER  = 2,
    GUI_ALIGN__END     = 3,
    GUI_ALIGN__STRETCH = 4
} gui_align_e;

// CSS text-align inside a label's box.
typedef enum gui_text_align_e
{
    GUI_TEXT_ALIGN__LEFT   = 0,
    GUI_TEXT_ALIGN__CENTER = 1,
    GUI_TEXT_ALIGN__RIGHT  = 2
} gui_text_align_e;

// CSS text-transform. Honored by the memory view (the case of its
// digits) so far; labels draw their text as written.
typedef enum gui_text_transform_e
{
    GUI_TEXT_TRANSFORM__NONE      = 0,
    GUI_TEXT_TRANSFORM__UPPERCASE = 1,
    GUI_TEXT_TRANSFORM__LOWERCASE = 2
} gui_text_transform_e;

// visual style for a node. populated by the selector resolver in scene.c.
// zero floats are treated as "not specified" by the overlay logic;
// has_background_color / has_accent_color / has_font_color are the
// explicit flags for color fields.
// Border line style. CSS-shaped subset: solid, dashed, dotted, plus
/** Uniform border styles rendered by the backend-neutral border module. */
typedef enum gui_border_style_e
{
    GUI_BORDER_STYLE__NONE   = 0,
    GUI_BORDER_STYLE__SOLID  = 1,
    GUI_BORDER_STYLE__DASHED = 2,
    GUI_BORDER_STYLE__DOTTED = 3,
    GUI_BORDER_STYLE__DOUBLE = 4,
    GUI_BORDER_STYLE__GROOVE = 5,
    GUI_BORDER_STYLE__RIDGE = 6,
    GUI_BORDER_STYLE__INSET = 7,
    GUI_BORDER_STYLE__OUTSET = 8,
    GUI_BORDER_STYLE__HIDDEN = 9,
} gui_border_style_e;

/** Presence flags allow later rules, including :checked, to reset a dimension or border. */
enum gui_checkbox_part_set_t
{
    GUI_CHECKBOX_PART__WIDTH = 1u << 0,
    GUI_CHECKBOX_PART__HEIGHT = 1u << 1,
    GUI_CHECKBOX_PART__RADIUS = 1u << 2,
    GUI_CHECKBOX_PART__INSET = 1u << 3,
    GUI_CHECKBOX_PART__BACKGROUND = 1u << 4,
    GUI_CHECKBOX_PART__BORDER_WIDTH = 1u << 5,
    GUI_CHECKBOX_PART__BORDER_STYLE = 1u << 6,
    GUI_CHECKBOX_PART__BORDER_COLOR = 1u << 7
};

/** Independent switch track/thumb paint and geometry; no engine-specific fields. */
typedef struct gui_checkbox_part_t
{
    uint set;
    float width;
    float height;
    float radius;
    float inset;
    float border_width;
    gui_border_style_e border_style;
    gui_color_t background;
    gui_color_t border_color;
    gui_gradient_t gradient;
    boole has_gradient;
    gui_gradient_t border_gradient;
    boole has_border_gradient;
} gui_checkbox_part_t;

/** Default preserves the original filled-square mark. */
enum gui_checkbox_mark_t
{
    GUI_CHECKBOX_MARK__DEFAULT = 0,
    GUI_CHECKBOX_MARK__CHECK,
    GUI_CHECKBOX_MARK__CROSS,
    GUI_CHECKBOX_MARK__BAR,
    GUI_CHECKBOX_MARK__FILL,
    GUI_CHECKBOX_MARK__NONE
};

// Select popup properties with meaningful zero values need explicit presence
// bits, so a later rule (or C override) can turn an effect off again.
enum gui_popup_set_e {
    GUI_POPUP_SET__BLUR = 1u << 0, GUI_POPUP_SET__ALIGN = 1u << 1,
    GUI_POPUP_SET__PLACEMENT = 1u << 2, GUI_POPUP_SET__REVEAL = 1u << 3,
    GUI_POPUP_SET__DURATION = 1u << 4, GUI_POPUP_SET__STAGGER = 1u << 5,
    GUI_POPUP_SET__OFFSET = 1u << 6, GUI_POPUP_SET__GAP = 1u << 7,
    GUI_POPUP_SET__WIDTH = 1u << 8, GUI_POPUP_SET__ROW_HEIGHT = 1u << 9,
    GUI_POPUP_SET__PADDING = 1u << 10
};
// scroll-end-effect: how scrolled content moves to its target.
typedef enum gui_scroll_end_effect_e
{
    GUI_SCROLL_END_EFFECT__NONE   = 0,
    GUI_SCROLL_END_EFFECT__WOBBLE = 1
} gui_scroll_end_effect_e;

// A local, non-oscillating deformation at the pointer when a select scrolls.
typedef enum gui_scroll_start_effect_e
{
    GUI_SCROLL_START_EFFECT__NONE = 0,
    GUI_SCROLL_START_EFFECT__STRETCH = 1
} gui_scroll_start_effect_e;
// select-style: the form a select shows its options in (docs/select-styles.md).
// LIST to SEARCH open over the page; SEGMENTED, CHIPS and CAROUSEL draw the
// options inside the control and open nothing.
typedef enum gui_select_style_e
{
    GUI_SELECT_STYLE__LIST      = 0, // a list at the control or in the centre of the viewport (default)
    GUI_SELECT_STYLE__WHEEL     = 1, // picker wheel: the row in the middle is the value, the rows fade toward both ends
    GUI_SELECT_STYLE__SHEET     = 2, // a panel that slides in from the bottom edge
    GUI_SELECT_STYLE__PAGE      = 3, // the whole viewport, with a title bar; a check mark on the current row
    GUI_SELECT_STYLE__MENU      = 4, // a menu at the control, as wide as its longest option
    GUI_SELECT_STYLE__DIALOG    = 5, // a centred panel with radio marks and two buttons
    GUI_SELECT_STYLE__SEGMENTED = 6, // all options side by side in the control
    GUI_SELECT_STYLE__CHIPS     = 7, // a row of pills that scrolls sideways
    GUI_SELECT_STYLE__CAROUSEL  = 8, // a horizontal wheel: the option in the middle is the value
    GUI_SELECT_STYLE__SEARCH    = 9  // a list under a text field that filters it
} gui_select_style_e;
typedef enum gui_popup_placement_e {
    GUI_POPUP_PLACEMENT__BELOW, GUI_POPUP_PLACEMENT__ABOVE, GUI_POPUP_PLACEMENT__AUTO
} gui_popup_placement_e;
typedef enum gui_popup_reveal_e {
    GUI_POPUP_REVEAL__FADE, GUI_POPUP_REVEAL__TOP_DOWN, GUI_POPUP_REVEAL__BOTTOM_UP
} gui_popup_reveal_e;

typedef struct gui_style_t
{
    // container fill. CSS-name: background-color.
    gui_color_t background_color;                 // 0x0
    boole     has_background_color;             // 0x10
    // active widget part: slider thumb, checkbox tick, radio dot, select chevron, image tint. CSS-4-name: accent-color.
    gui_color_t accent_color;                     // 0x14
    boole     has_accent_color;                 // 0x24
    // text color. CSS-name: color, but renamed for unambiguity.
    gui_color_t font_color;                       // 0x28
    boole     has_font_color;                   // 0x38
    // Border. border_width is in logical pixels (multiplied by the
    // global scale at draw time, like every other size field).
    // Border draws when border_width > 0 AND border_style != NONE
    // AND has_border_color is TRUE -- all three must align so a
    // partial declaration (just `border-width: 2;`) doesn't paint
    // an invisible black border by accident.
    float            border_width;              // 0x3C
    gui_color_t        border_color;              // 0x40
    boole            has_border_color;          // 0x50
    // Optional gradient form of border-color: `border-color:
    // linear-gradient(...)` / `radial-gradient(...)` / the old
    // `gradient(<from>, <to>[, direction])`. The emitter draws the outer
    // border rect with renderer__submit_gradient instead of a solid
    // rect. border_color keeps the first color (has_border_color TRUE)
    // so a reader without gradient awareness still gets a sensible line.
    gui_gradient_t     border_gradient;           // 0x54
    boole            has_border_gradient;       // 0x24C
    gui_border_style_e border_style;              // 0x250
    boole border_style_set;
    // corner radius in pixels. 0 = square. uniform for now; per-corner radii arrive in a later iteration.
    float     radius;                           // 0x254
    float     pad_t;                            // 0x258
    float     pad_r;                            // 0x25C
    float     pad_b;                            // 0x260
    float     pad_l;                            // 0x264
    // CSS-aligned margin: space AROUND this element, outside its
    // own bounds. Distinct from `gap` (space BETWEEN siblings inside
    // a flex/grid container) and `pad` (space INSIDE this element,
    // between its border and its children). Currently parsed but
    // NOT YET honored by the layout pass -- wiring widgets to read
    // these offsets is a follow-up.
    float     margin_t;                         // 0x268
    float     margin_r;                         // 0x26C
    float     margin_b;                         // 0x270
    float     margin_l;                         // 0x274
    float     gap;                              // 0x278
    // 0.0f means "auto" in the poc.
    float     size_w;                           // 0x27C
    // 0.0f means "auto" in the poc.
    float     size_h;                           // 0x280
    // size_w_explicit / size_h_explicit / font_size_explicit are
    // "did a style rule write a px-valued width / height / font-size
    // for the rule's state slot". The animator uses them to gate
    // size+font interpolation: a transition only lerps the property
    // when BOTH endpoints are flagged as rule-defined. Stops a
    // transient unset endpoint (which the resolver may emit during
    // bootstrap or class re-evaluation) from corrupting layout.
    boole     size_w_explicit;                  // 0x284
    boole     size_h_explicit;                  // 0x285
    // width/height as a percentage of the PARENT's inner content
    // area (post-pad). 0 means "not set" -- fall back to size_w /
    // size_h. Non-zero values WIN over size_w / size_h so a style
    // with both ends up percent-driven, which is what you'd
    // intuitively expect from CSS. Accepted as `width: 100%;` or
    // `height: 50%;` in .style files. Values >100 are legal (if
    // the parent allows overflow) but uncommon.
    float     width_pct;                        // 0x288
    float     height_pct;                       // 0x28C
    // CSS min-width / min-height in logical pixels, 0 = none. Applied
    // by scene__apply_min_max to every widget's final size.
    float     min_w;                            // 0x290
    float     min_h;                            // 0x294
    // empty = default (first registered font). 64 bytes to fit longer family names like "SourceCodePro-Semibold-Italic".
    char      font_family[64];                  // 0x298
    // 0 = inherit/default.
    float     font_size;                        // 0x2D8
    // see size_w_explicit block above.
    boole     font_size_explicit;               // 0x2DC
    // CSS line-height. line_height is the unitless multiplier of the
    // font size (`line-height: 1.2`), line_height_px an absolute value
    // (`line-height: 18px`). Both zero means normal: the font's rounded
    // ascent + descent + line gap, which is what a browser computes.
    // The parser stores -1 in line_height for an explicit `normal` so a
    // rule can override an inherited multiplier. Inherited like
    // font-size.
    float     line_height;                      // 0x2E0
    float     line_height_px;                   // 0x2E4

    // ===== overflow + scrollbar styling =====================================
    // controls whether the node shows scrollbars and, when it does, their
    // visual appearance. currently only respected by <div> -- other
    // container widgets ignore these fields. the two axes are independent
    // so you can get `overflow-y: auto` behavior without touching x.
    // layout interaction (widget_div.c):
    // when either axis is SCROLL or AUTO, the div lays out its children
    // at (child_x, child_y - scroll_y) and computes content_w/_h from
    // the resulting child extents. the scrollbar thumb's size and
    // position are derived from content_h vs bounds.h.
    // zero fields = use built-in defaults (12px bar, 6px radius, neutral
    // dark track + lighter thumb). this keeps .style files terse for the
    // common case where you just want "working" scrollbars.
    gui_overflow_e overflow_x;                    // 0x2E8
    gui_overflow_e overflow_y;                    // 0x2EC
    // 0 = default (~12px). thickness of the bar.
    float        scrollbar_size;                // 0x2F0
    // 0 = default (~scrollbar_size/2). rounds track + thumb.
    float        scrollbar_radius;              // 0x2F4
    // bar background (the "channel" behind the thumb).
    gui_color_t    scrollbar_track;               // 0x2F8
    boole        has_scrollbar_track;           // 0x308
    // the draggable thumb.
    gui_color_t    scrollbar_thumb;               // 0x30C
    boole        has_scrollbar_thumb;           // 0x31C

    // ===== animation =========================================================
    // Two ways to drive an appear/disappear animation:
    // 1. SHORTCUT (fade-in only):
    // Window { appear: 600 ease_out; }
    // Sets appear_ms / appear_easing on this rule. The animator
    // multiplies every color's alpha by the eased progress factor
    // (everything fades in from invisible). Inherited by descendants
    // like font_family / font_size, so one declaration on Window
    // cascades to the whole tree.
    // 2. PROPER PSEUDO-STATE (per-property control):
    // Button.primary {
    // bg: #6366f1;
    // :appear {
    // bg: #6366f100;             -- starting value (8-digit hex = with alpha)
    // transition: 600 ease_out;  -- duration + easing for THIS state's animation
    // }
    // }
    // The :appear block becomes style[GUI_NODE_STATE__APPEAR]. The
    // transition declaration sets transition_duration_ms +
    // transition_easing on that same slot. The animator
    // interpolates each property listed in :appear FROM its
    // :appear value TO whatever the resolver computed for the
    // node's current state -- so the animation works even if the
    // user is hovering during the fade-in.
    // Both modes are checked on every node; the pseudo-state mode
    // wins when transition_duration_ms is set (so a button with
    // both inherits Window's appear_ms but overrides it with its
    // own per-property pseudo-state animation).
    // Defaults: zero everywhere = no animation. inherited like the
    // other "typography-style" properties.
    float      appear_ms;                       // 0x320
    gui_easing_e appear_easing;                   // 0x324
    // Parameters for GUI_EASING__CUBIC_BEZIER / GUI_EASING__SPRING on the
    // `appear` curve. Zero-initialised; only read when appear_easing is
    // one of the parameterised kinds. See gui_easing_e enum comment for
    // parameter meanings.
    float      appear_easing_params[4];         // 0x328
    // appear-threshold / disappear-threshold: the fraction of a node's
    // box (of its own extent, or of the viewport's when the node is the
    // larger) that has to be inside its nearest clipping or scrolling
    // ancestor for the node to count as in view (:appear plays when it
    // gets there) and below which it counts as leaving (:disappear
    // plays while it is still that visible). 0..1; defaults 0.25 and
    // 0.75 when unset. Inherited, so a window rule sets them page-wide.
    float      appear_threshold;
    boole      has_appear_threshold;
    float      disappear_threshold;
    boole      has_disappear_threshold;

    // Per-state animation spec (lives on the :appear / :disappear /
    // future per-state slot, not the default slot). animation_duration_ms
    // > 0 means "the animator interpolates the per-property values in
    // this state slot to the matching values in the default slot over
    // this many milliseconds, using the given easing curve". Declared
    // in .style as `animation: <ms> [easing];`.
    // If a :appear block sets values but omits the animation declaration,
    // the animator falls back to a sensible default (300 ms ease_out)
    // -- the user just needs `:appear { bg: ...; }` to get a working pop.
    float      animation_duration_ms;           // 0x348
    gui_easing_e animation_easing;                // 0x34C
    float      animation_easing_params[4];      // 0x350

    // Generic property transition (state-change interpolation). Applied
    // by the animator when any animated property changes its resolved
    // value between frames (e.g. hover -> default). Distinct from the
    // :appear / :disappear mechanism above: those play a one-shot
    // entrance/exit, `transition` smooths every subsequent delta.
    // Declared in .style as `transition: <prop> <ms> [easing]`.
    // `<prop>` is currently `all` only; named properties are a future
    // refinement. Zero duration disables the feature (default).
    float      transition_duration_ms;          // 0x360
    gui_easing_e transition_easing;               // 0x364
    float      transition_easing_params[4];     // 0x368

    // ===== visibility + display ============================================
    // Matches CSS semantics one-for-one:
    // visibility: hidden  -> node is painted INVISIBLE but still
    // occupies its layout bounds (siblings
    // don't reflow).
    // display: none       -> node disappears from layout AND paint;
    // siblings collapse into the space it
    // would have taken.
    // Neither cascades (the resolver doesn't walk these from parent
    // to child). A hidden parent with a visible child produces a
    // visible child inside a blank container -- same as CSS when the
    // child explicitly sets `visibility: visible`. To hide a whole
    // subtree, use display: none on the parent (it short-circuits
    // traversal).
    // The animator treats transitions on either property as equivalent
    // to "the node just entered the tree" -- appear_age_ms is reset
    // to 0 on hidden->visible or none->block, so :appear replays.
    gui_visibility_e visibility;                  // 0x378
    gui_display_e    display;                     // 0x37C

    // Positioning. See gui_position_e for the model. The insets are
    // top/right/bottom/left in px (inset_*) or in % of the containing
    // block (inset_*_pct); a rule that writes an edge also sets
    // inset_*_set (at the end of this struct), which is how `top: 0`
    // and negative offsets count as set. Two opposite edges stretch
    // the box between them and any explicit width / height on that
    // axis is ignored. Defaults: position=STATIC, no edge set.
    gui_position_e position;                      // 0x380
    float        inset_t;                       // 0x384
    float        inset_r;                       // 0x388
    float        inset_b;                       // 0x38C
    float        inset_l;                       // 0x390
    float        inset_t_pct;                   // 0x394
    float        inset_r_pct;                   // 0x398
    float        inset_b_pct;                   // 0x39C
    float        inset_l_pct;                   // 0x3A0

    // ===== scrolling =======================================================
    // scroll_smooth_ms: if > 0, wheel / thumb-drag updates on a
    // scrollable container write into scroll_y_target and the animator
    // lerps scroll_y toward scroll_y_target over this time constant
    // (exponential decay, so the first 63 % of the distance is covered
    // in scroll_smooth_ms milliseconds; tail asymptotes toward the
    // target). 0 = instant -- scroll_y follows user input with no
    // animation (same behaviour as before this field existed).
    float scroll_smooth_ms;                     // 0x3A4
    // scroll-end-effect: none | wobble [<amount>]. wobble moves the scrolled
    // content with an underdamped spring instead of the lerp above: it
    // passes its target and swings back. amount is 0..1 (0.5 when the
    // keyword stands alone): 0 does not pass the target, 1 passes it by
    // about 44 % of the step. scroll-smooth gives the spring's response
    // time (160 ms when unset). A select's open list also passes its two
    // edges and lets the rows away from the pointer trail the movement;
    // a scrolling container stops at its edges (overscroll-behavior is
    // the edge effect there). scroll_end_effect_set marks a rule that wrote
    // the property, so none can override wobble.
    gui_scroll_end_effect_e scroll_end_effect;
    float               scroll_end_effect_amount;
    boole               scroll_end_effect_set;
    gui_scroll_start_effect_e scroll_start_effect;
    float               scroll_start_effect_amount;
    boole               scroll_start_effect_set;

    // ===== rubber-band overscroll (opt-in) ====================================
    // overscroll-behavior: auto | contain | none. auto and contain let the
    // content be pulled past its edges by a pan, a fling or a wheel tick
    // and spring back (the rubber band of iOS); none, the default, stops
    // at the edge. The vertical gestures exist today; the x flag waits for
    // horizontal gestures. overscroll_set marks a rule that wrote the flags
    // (the cascade copies them only then, so `none` can override `auto`).
    boole        overscroll_x;
    boole        overscroll_y;
    boole        overscroll_set;
    // overscroll-resistance: how much of a movement past the edge the
    // content follows right at the edge (0 = unset, 0.55 by default); it
    // diminishes linearly to nothing at overscroll-limit.
    float        overscroll_resistance;
    // overscroll-limit: the farthest the content can be pulled past an
    // edge, in logical pixels or as a percentage of the box (0 = unset,
    // 50 % by default).
    float        overscroll_limit_px;
    float        overscroll_limit_pct;
    // overscroll-duration: the spring back in milliseconds (0 = unset,
    // 350 by default) with overscroll-timing-function (ease-out by
    // default; ease-out-back overshoots the edge once, cubic-bezier() for
    // anything else).
    float        overscroll_duration_ms;
    gui_easing_e overscroll_easing;
    float        overscroll_easing_params[4];
    boole        overscroll_easing_set;

    // ===== widget looks (meek extensions) ====================================
    // appearance: switch draws a checkbox as a pill with a knob (accent
    // when checked, the background color when not).
    boole        switch_look;
    gui_checkbox_part_t checkbox_track;
    gui_checkbox_part_t checkbox_thumb;
    int          checkbox_mark;
    float        checkbox_mark_width;
    float        checkbox_mark_inset;
    boole        has_checkbox_mark_width;
    boole        has_checkbox_mark_inset;
    gui_color_t  checkbox_mark_color;
    boole        has_checkbox_mark_color;
    /** Optional switch geometry and thumb colors; zero sizes preserve legacy geometry. */
    float        switch_track_h;
    float        switch_thumb_size;
    boole        has_switch_track_h;
    boole        has_switch_thumb_size;
    gui_color_t  switch_thumb_color;
    gui_color_t  switch_thumb_off_color;
    boole        has_switch_thumb_color;
    boole        has_switch_thumb_off_color;
    // app-region: drag (the WebKit / Electron name): a press on this node
    // moves its nearest absolutely or fixed positioned ancestor (or the
    // node itself when it is one), the title bar of a movable panel.
    boole        app_region_drag;
    // slider-track-height / slider-thumb-width: the range-input look, a
    // thin track centered in the box and a thin thumb (0 = the widget's
    // default shapes); slider-value: none | drag | always draws the
    // rounded value above the thumb.
    float        slider_track_h;
    float        slider_thumb_w;
    int          slider_value_mode;
    // color-swatch / color-swatch-radius / color-swatch-gap: a colour
    // picker draws a square of its current colour of this size to the
    // right of its strip (0 = none), with this corner radius and gap.
    float        swatch_size;
    float        swatch_radius;
    float        swatch_gap;
    // slider-fill: the track left of the thumb (the filled part of a
    // range); slider-value-color and slider-value-position place and
    // colour the value the slider shows; slider-decimals is how many
    // fraction digits it prints (0 = a whole number).
    gui_color_t  slider_fill;
    boole        has_slider_fill;
    gui_color_t  slider_value_color;
    boole        has_slider_value_color;
    int          slider_value_pos; // 0 above, 1 right, 2 left
    int          slider_decimals;
    // the select's open list: the page behind it is blurred by this many
    // pixels and covered by the dim colour, and the options take their
    // colour per state.
    float        popup_blur;
    gui_color_t  popup_dim;
    boole        has_popup_dim;
    gui_color_t  option_color;
    boole        has_option_color;
    gui_color_t  option_current_color;
    boole        has_option_current_color;
    gui_color_t  option_hover_color;
    boole        has_option_hover_color;
    boole        popup_centered;
    uint         popup_set;
    gui_popup_placement_e popup_placement;
    gui_popup_reveal_e popup_reveal;
    float        popup_duration_ms; // per-row duration; 0 is instant
    float        popup_stagger_ms;  // delay between rows in reveal order
    float        popup_offset;      // logical pixels to travel into place
    float        popup_gap;
    float        popup_width;       // 0 follows the select width
    float        option_height;
    float        option_padding;
    gui_color_t  popup_background;
    boole        has_popup_background;
    gui_color_t  option_current_background;
    boole        has_option_current_background;
    gui_color_t  option_hover_background;
    boole        has_option_hover_background;
    // select-style: the form the options are shown in; select_style_set
    // marks a rule that wrote it, so list can override another style.
    gui_select_style_e select_style;
    boole        select_style_set;
    // select-rows: how many options are visible at the same time. wheel
    // and carousel: the rows or items on screen, an odd number from 3
    // (5 when unset). The other styles that open: the most rows shown
    // before the list scrolls (0 = as many as the viewport holds).
    float        select_rows;
    // option-separator: the two lines around the middle row of a wheel,
    // the lines between the segments of a segmented control and under a
    // title bar or a search field.
    gui_color_t  option_separator;
    boole        has_option_separator;

    // ===== box-shadow ========================================================
    // Drawn BEHIND the node's bg rect. Matches the CSS four-value form:
    // box-shadow: <dx> <dy> <blur> <color>;
    // Shadow footprint = bg rect, offset by (dx, dy) in logical pixels,
    // with a `blur`-wide soft edge (via SDF falloff in the fragment
    // shader). Uses the node's radius so rounded rects cast rounded
    // shadows. Not scaled by ui scale for now -- values read in raw px.
    float     shadow_dx;                        // 0x3A8
    float     shadow_dy;                        // 0x3AC
    float     shadow_blur;                      // 0x3B0
    gui_color_t shadow_color;                     // 0x3B4
    boole     has_shadow;                       // 0x3C4

    // ===== opacity ===========================================================
    // Multiplied into every descendant's (and this node's) color alphas
    // at emit-time. Cascades through the tree via a runtime effective
    // opacity accumulator, NOT via style inheritance -- so a parent at
    // 0.5 and a child at 0.5 compose to 0.25, the CSS way.
    // 1.0 = fully opaque (default); 0.0 = invisible (descendants aren't
    // culled though -- they still lay out and hit-test, same as CSS).
    // Stored as `has_opacity` + value because zero has a semantic
    // meaning (fully transparent) distinct from "not set".
    float opacity;                              // 0x3C8
    boole has_opacity;                          // 0x3CC

    // ===== z-index ===========================================================
    // Sibling-only stacking order. At emit time, scene sorts a parent's
    // children by (z_index, original order) and draws them in that
    // sequence. Greater z_index draws on top. Does NOT reorder across
    // ancestor boundaries the way CSS stacking contexts do -- siblings
    // only, which covers the cases people actually hit.
    // Default 0. Negative values are allowed and draw below z = 0.
    int z_index;                                // 0x3D0

    // ===== blur ==============================================================
    // Applies to this node as a visual softening pass. Currently
    // approximated by submitting a translucent darken over the bg
    // (cheap, O(1), no FBO). True separable-Gaussian blur-of-contents
    // is a bigger port across the four renderer backends and sits on
    // the deferred list.
    // 0.0 = no blur (default). Positive = blur radius in logical pixels.
    float blur_px;                              // 0x3D4

    // ===== background gradient ===============================================
    // A CSS gradient painted over `background-color` (CSS layering: the
    // color is the bottom layer, the gradient the image layer above it).
    // Set by `background-gradient`, `background-image: linear-gradient(...)`
    // / `radial-gradient(...)` and the `background` shorthand. Alpha in
    // any stop works (a `transparent` stop fades the layer out).
    gui_gradient_t     bg_gradient;               // 0x3D8
    boole            has_bg_gradient;           // 0x5D0

    // ===== child alignment ==================================================
    // horizontal-alignment: left | center | right   (default left)
    // vertical-alignment:   top  | center | bottom  (default top)
    // Semantics depend on the container's layout direction:
    // <column> / <div> / <window>:
    // horizontal-alignment  -> each child's x inside parent's inner width
    // vertical-alignment    -> the whole stack's y inside parent's inner height
    // <row>:
    // horizontal-alignment  -> the whole row's x inside parent's inner width
    // vertical-alignment    -> each child's y inside parent's inner height
    // Zero values = defaults (left / top), so a style that never sets
    // alignment keeps today's top-left flow.
    gui_halign_e halign;                          // 0x5D4
    gui_valign_e valign;                          // 0x5D8
    // The CSS names for the same two axes. justify_content aligns the
    // children along the container's main axis and adds the space-*
    // distributions; align_items aligns them across it; align_self is
    // a child's own override of align_items. Unset ones fall back to
    // valign / halign, so old style files behave as before.
    gui_justify_e justify_content;                // 0x5DC
    gui_align_e   align_items;                    // 0x5E0
    gui_align_e   align_self;                     // 0x5E4
    // CSS text-align of the lines inside a label's box. Inherited.
    gui_text_align_e text_align;                  // 0x5E8
    // CSS text-transform. Inherited.
    gui_text_transform_e text_transform;
    // CSS max-width / max-height in logical pixels, 0 = none; min_w and
    // min_h above are the min- pair.
    float max_w;                                  // 0x5F0
    float max_h;                                  // 0x5F4

    // ===== image fitting =====================================================
    // background-image: <path>         paints an image as the container's
    // bg (via scene__emit_default_bg).
    // fitted according to background-size.
    // background-size:  fit-keyword    one of fill / stretch / contain /
    // cover / none. default FILL.
    // object-fit:       fit-keyword    <image> widget's own fit behaviour.
    // default FILL (= pre-property stretch).
    char          background_image[128];        // 0x5F8
    boole         has_background_image;         // 0x678
    gui_image_fit_e background_size;              // 0x67C
    gui_image_fit_e object_fit;                   // 0x680

    // ===== collection layout =================================================
    // Only read by widget_collection. Parser accepts `columns: N`,
    // `item-width: N`, `item-height: N`, `layout: grid/list/flow`.
    gui_collection_layout_e collection_layout;    // 0x684
    int                   collection_columns;   // 0x688
    float                 item_width;           // 0x68C
    float                 item_height;          // 0x690

    // scroll_fade_px: width of the top-and-bottom soft-fade region
    // inside a scrollable container. Declared on the container (same
    // rule that carries overflow_y: auto / scroll) and applies to its
    // direct child subtrees: the render walk multiplies opacity by a
    // smooth ramp from 0 at the viewport edge to 1 once the child's
    // centre sits scroll_fade_px inside the visible area. Lets content
    // smoothly appear and disappear as it's scrolled into / out of
    // view instead of getting hard-clipped by the scissor at the
    // viewport edge. 0 = no fade (default; same behaviour as before
    // this field landed).
    float scroll_fade_px;                       // 0x694

    // inset_*_set: TRUE when a style rule wrote top/right/bottom/left,
    // so a zero or negative offset still counts as set. Runtime code
    // that writes inset_* directly may leave these FALSE; a non-zero
    // value counts as set on its own.
    boole inset_t_set;                          // 0x698
    boole inset_r_set;                          // 0x699
    boole inset_b_set;                          // 0x69A
    boole inset_l_set;                          // 0x69B
    // border_width_set: TRUE when a rule wrote border-width (alone or in
    // the border shorthand), so `border-width: 0` in a later rule cancels
    // an earlier border the way it does in CSS instead of reading as unset.
    boole border_width_set;                     // 0x69C
    boole display_set;
    int flex_direction; // 0 unspecified, 1 row, 2 column
    int table_layout; // 0 unspecified, 1 auto, 2 fixed
    int border_collapse; // 0 unspecified, 1 separate, 2 collapse
    float border_spacing_x;
    float border_spacing_y;
    boole border_spacing_set;
    int table_vertical_align; // 0 unspecified, 1 top, 2 middle, 3 bottom
} gui_style_t;

// retained-tree node. first-child / next-sibling representation so that
// tree walks are a simple linked-list traversal.
typedef struct gui_node_t gui_node_t;
struct gui_node_t
{
    gui_node_type_e  type;
    char           id[64];
    char           klass[64];
    // Navigation metadata and transient presentation, owned by the page stack.
    char           navigation_parent[64];
    char           navigation_title[128];
    char           navigation_focus[64];
    float          navigation_x;
    float          navigation_y;
    float          navigation_alpha;
    boole          navigation_effect;
    boole          navigation_inactive;
    boole          navigation_suppress_disappear;
    boole          navigation_chrome;
    gui_style_t      style[GUI_NODE_STATE__COUNT];
    gui_style_t      resolved;
    gui_node_state_e state;
    gui_rect_t       bounds; // bounds in pixels, top-left origin. filled by the layout pass.
    uint           on_click_hash; // fnv-1a hash of on_click_name. 0 = no handler.
    char           on_click_name[64]; // original handler name from the .ui file. used as the GetProcAddress fallback when the registration table misses.
    uint           on_change_hash; // fnv-1a hash of on_change_name. 0 = no handler.
    char           on_change_name[64]; // original handler name from the .ui file (slider on_change attribute).
    float          value; // generic scalar value held by the node. used by Slider; 0 for other kinds.
    float          value_min; // lower bound of `value` (slider only). 0 for other kinds.
    float          value_max; // upper bound of `value` (slider only). 0 for other kinds (range collapses, GUI_NODE_TYPE__SLIDER defaults to 1).
    boole          slider_drag_only; // slider only. when TRUE (parser sets via `drag-only="1"` attr), mouse_down only accepts touches that land on the thumb -- prevents tap-to-snap from anywhere on the track. Used by meek-shell's lock-screen slide-to-unlock so a stray tap on the right side of the lock-screen can't snap-unlock the device.
    boole          slider_commit_on_release; // slider only. when TRUE (parser sets via `commit-on-release="1"` attr), the slider value updates visually during drag but on_change does NOT dispatch on every drag tick -- instead it dispatches exactly ONCE, at mouse_up, with the final value. Used by slide-to-confirm patterns (lock-screen slide-to-unlock) where the user wants to be able to drag back if they change their mind without firing the action mid-drag.
    // slider step: the value snaps to a multiple of it (0 = continuous)
    float          slider_step;
    float          slider_thumb_aspect; // slider only. thumb's width = bounds.h * slider_thumb_aspect. Default 1.0 = square thumb (legacy behaviour). Values > 1 make the thumb rectangular, elongated horizontally -- iOS slide-to-unlock-style handles are ~1.8-2.0. Parsed from the `thumb-aspect="<float>"` attribute in .ui.
    // Owned text: starts in text_inline, grows through scene__reserve_text.
    // Read text directly; replace with scene__set_text. Never assign/free text.
    char*          text;
    char           text_inline[256];
    int64          text_capacity; // allocated bytes INCLUDING the terminator
    int64          text_limit; // permitted UTF-8 bytes EXCLUDING the terminator
    int64          text_len; // current byte length of text (for Input where text mutates via keystrokes).
    gui_node_t*      parent;
    gui_node_t*      first_child;
    gui_node_t*      last_child;
    gui_node_t*      next_sibling;
    int64          child_count;

    // ===== scrollable-container state ========================================
    // populated at layout time for nodes that opt into scrolling via
    // overflow_x / overflow_y. persisted across frames so the scroll
    // position survives layout re-runs (layout is rebuilt every frame
    // in the PoC).
    // scroll_x / scroll_y     current scroll offset in pixels. children
    // are drawn at child_y - scroll_y.
    // content_w / content_h   total extent of children (tight bounding
    // box of what the div "could" show without
    // a scrollbar). measured each layout pass.
    // scroll_drag_axis        which axis (if any) is currently being
    // dragged by the scrollbar thumb. 0 = none,
    // 1 = y-axis, 2 = x-axis (reserved).
    // scroll_drag_mouse_start mouse coordinate (y or x, matching axis)
    // captured at mouse-down on the thumb. used
    // to convert subsequent drag positions into
    // a scroll delta.
    // scroll_drag_scroll_start scroll_x/_y value at the start of the
    // drag; drag delta is added to this to
    // compute the new scroll offset.
    float scroll_x;
    float scroll_y;
    // scroll_{x,y}_target: the desired scroll position. Wheel input and
    // thumb drag both write here; the animator then lerps scroll_{x,y}
    // toward this value over resolved.scroll_smooth_ms. When
    // scroll_smooth_ms is 0, scroll_{x,y} is snapped to
    // scroll_{x,y}_target immediately so the behaviour is identical to
    // pre-smooth-scroll code. Initialized to 0 by calloc, same as the
    // scroll_* pair above.
    float scroll_x_target;
    float scroll_y_target;
    float content_w;
    float content_h;
    int64 scroll_drag_axis; // 0 = not dragging, 1 = y, 2 = x.
    float scroll_drag_mouse_start;
    float scroll_drag_scroll_start;
    // Frame-time (ms) of the last scroll activity on this node:
    // wheel input, thumb drag, touch flick, or active smooth-scroll
    // interpolation. scroll__draw_vbar uses (now - this) to fade the
    // track + thumb to zero alpha after a short idle hold, so the
    // scrollbar disappears the way iOS / Android scrollbars do
    // instead of being permanently on screen.
    int64 scroll_last_active_ms;

    // Scroll inertia / fling-momentum. While the user pans, motion
    // events EWMA-smooth the instantaneous velocity (px/ms, signed)
    // into scroll_y_velocity. The user lifts; the animator picks the
    // residual velocity up each frame, advances scroll_y_target by
    // velocity*dt, and multiplicatively decays the velocity by a
    // friction factor. When |velocity| drops below an epsilon the
    // animator zeroes it and the fling stops. A new touch_down on
    // the same scrollable also zeroes it (tap-to-stop, iOS-style).
    // scroll_pan_last_ms is the frame-time of the most recent motion
    // event during the active pan. The velocity EWMA reads
    // (now - this) for the dt between samples.
    float scroll_y_velocity;
    int64 scroll_pan_last_ms;
    // scroll-end-effect: wobble. The speed of scroll_y on the spring that
    // moves it to scroll_y_target (device pixels per millisecond); zero
    // at rest and for every other scroll-end-effect.
    float scroll_wobble_velocity;

    // Rubber-band overscroll (overscroll-behavior: auto). overscroll_x /
    // overscroll_y: how far the content is pulled past an edge right now
    // (device pixels; negative past the start, positive past the end);
    // layout adds it to scroll_x / scroll_y when placing the children.
    // held: a finger holds it (no spring); fling: a fling that reached
    // the edge is still pushing it; returning: it eases back from
    // overscroll_*_from over overscroll-duration, age counting the ms.
    float overscroll_x;
    float overscroll_y;
    float overscroll_y_from;
    float overscroll_y_age_ms;
    boole overscroll_y_held;
    boole overscroll_y_fling;
    boole overscroll_y_returning;

    // app-region: drag moved this positioned node by the mouse; layout
    // adds the offset to the placement of its inset box.
    float drag_offset_x;
    float drag_offset_y;

    // A widget's visible animation progress (switch, slider label, select).
    // scene__widget_ease provides exponential easing; switches/selects
    // keep their finite timelines privately. widget_anim_ms is the int64 frame
    // time of the last step (0 = never stepped: the first step snaps), and
    // widget_animating keeps the render loop alive while it moves.
    // :appear / :disappear fade of the current frame (1 = none), applied
    // by the render walk to effective_opacity so it survives motion__tick
    float fade_alpha;
    float widget_anim;
    int64 widget_anim_ms;
    boole widget_animating;

    // ===== animator state ===================================================
    // appear_age_ms: milliseconds the node has been in the tree. The
    // animator increments this each frame in animator__tick. When
    // appear_age_ms < resolved.appear_ms, the animator multiplies the
    // node's resolved color alphas by an eased fade factor that ramps
    // from 0 at age=0 to 1 at age=appear_ms. Initialized to 0 in
    // scene__node_new (calloc), which is exactly what we want -- a
    // freshly-created node starts invisible and fades in.
    // prev_visibility / prev_display: the resolved values from the
    // PREVIOUS frame. animator__tick compares the current resolved
    // values against these; when a node transitions out of "hidden"
    // (either direction: GUI_VISIBILITY__HIDDEN -> _VISIBLE or
    // GUI_DISPLAY__NONE -> _BLOCK), appear_age_ms is reset to 0 so the
    // :appear animation plays again. While a node is hidden / none,
    // appear_age_ms is not advanced, so the timer doesn't "use up"
    // the animation while the node is out of view. Initialized to 0
    // (= GUI_VISIBILITY__VISIBLE / GUI_DISPLAY__BLOCK) by calloc so a
    // node that starts hidden doesn't spuriously replay on frame 1.
    float          appear_age_ms;
    gui_visibility_e prev_visibility;
    gui_display_e    prev_display;

    // Transition-snapshot state. Populated by the animator when the
    // `transition:` property is active on the resolved style.
    // transition_age_ms: counts up from 0 each time any tracked
    // property changes its resolved target. Idle when it reaches
    // the transition duration -- no interpolation contribution in
    // that case.
    // transition_from_*: the DISPLAYED value at the moment the most
    // recent change started. Cached so an interpolation that itself
    // gets interrupted by another change produces a smooth chain
    // (new start = current displayed, not the old target).
    // transition_prev_target_*: the resolver's OUTPUT last frame.
    // Compared against this frame's resolver output to detect a
    // "target changed" moment.
    // transition_seen: 0 = fields are uninitialised (first frame,
    // nothing to compare against), 1 = populated. Set to 1 after
    // the first capture pass.
    float     transition_age_ms;
    gui_color_t transition_from_bg;
    gui_color_t transition_from_fg;
    gui_color_t transition_from_text;
    gui_color_t transition_from_border;
    float     transition_from_radius;
    gui_gradient_t transition_from_bg_grad;
    float     transition_from_size_w;
    float     transition_from_size_h;
    float     transition_from_font_size;
    boole     transition_from_size_w_explicit;
    boole     transition_from_size_h_explicit;
    boole     transition_from_font_size_explicit;
    gui_color_t transition_prev_target_bg;
    gui_color_t transition_prev_target_fg;
    gui_color_t transition_prev_target_text;
    gui_color_t transition_prev_target_border;
    float     transition_prev_target_radius;
    gui_gradient_t transition_prev_target_bg_grad;
    float     transition_prev_target_size_w;
    float     transition_prev_target_size_h;
    float     transition_prev_target_font_size;
    boole     transition_prev_target_size_w_explicit;
    boole     transition_prev_target_size_h_explicit;
    boole     transition_prev_target_font_size_explicit;
    boole     transition_seen;

    // disappear_remaining_ms: counts down from the :disappear slot's
    // duration to zero while the node plays its outro. Triggered on the
    // was-shown -> not-shown edge (visibility: visible -> hidden or
    // display: block -> none). While non-zero, the animator overrides
    // the resolved visibility/display back to visible so the node keeps
    // rendering while its values interpolate toward the :disappear
    // slot's override values.
    // Zero-initialised (via calloc) matches the intended idle state:
    // freshly-created nodes that are born hidden don't spuriously play
    // a disappear animation -- only the shown-to-hidden edge sets the
    // counter to the slot's full duration, and each tick it drains
    // back down.
    float     disappear_remaining_ms;

    // ===== generic widget state =============================================
    // is_open: used by widgets that have a "popped open" state distinct
    // from the standard hover/pressed/disabled states. <select> sets
    // this when its dropdown menu is showing; the popup draws via the
    // scene overlay mechanism (see scene__set_overlay). 0 = closed,
    // 1 = open. Other widgets (modal dialogs, accordions, tooltips...)
    // can repurpose the same flag.
    // value (also used by Slider) is the additional payload: for
    // <checkbox> it's 0/1 (unchecked/checked), for <radio> it's 0/1
    // (deselected/selected), for <select> it's the index of the
    // currently selected <option> child.
    boole is_open;

    // Per-widget opaque pointer. Holds widget-specific state that
    // doesn't fit any of the named fields above (e.g. widget_image
    // stashes its decoded texture + dimensions here). Allocated by
    // the widget's init_defaults / apply_attribute hook; released by
    // its on_destroy hook so scene__node_free doesn't leak.
    void* user_data;

    // Effective opacity (runtime-only). scene__emit_draws writes
    // this before dispatching to the widget's emit_draws so the
    // widget can multiply it into every color alpha it submits.
    // Computed as parent.effective_opacity * resolved.opacity (or
    // 1.0 when resolved.has_opacity is FALSE). Not a style field --
    // scene owns it.
    float effective_opacity;

    // Host-side color overrides. Set via
    // scene__set_background_color_override / scene__set_font_color_override
    // etc. from handler code; scene applies them AFTER rule overlay
    // + animator each frame, so they survive style re-resolve
    // without the host having to re-register rules or mutate
    // style[] (which the resolver's memset would clobber). Typical
    // use: theme toggles, quick swatch previews, editor drag-to-
    // recolor. Clear with has_*_override = FALSE.
    gui_color_t background_color_override;
    boole     has_background_color_override;
    gui_color_t font_color_override;
    boole     has_font_color_override;
    char      font_family_override[64];
    float     font_size_override;
    // Background-image override. Same applied-after-rules story as
    // the color overrides above, but for the bg-image filename.
    // 128 bytes matches gui_style_t.background_image[]. Set by hosts
    // that swap the wallpaper at runtime (meek-shell's settings
    // app). The resolver mirrors the value into n->resolved.background_image
    // and flips has_background_image so the rendering path picks it
    // up.
    char  background_image_override[128];
    boole has_background_image_override;
    // input only: type="password" draws every character as a bullet.
    boole input_password;
    // animator: whether the node's box intersected its nearest
    // clipping / scrolling ancestor at the last tick, and whether that
    // has been evaluated at all. A node that comes into view replays
    // :appear; its timer is frozen while it is out of view.
    boole was_in_view;
    boole view_tracked;
    // set when the node left the view by scrolling (not by hiding); it
    // then holds its :disappear end values until it comes back
    boole scrolled_out;
    // the largest visible fraction seen since the node came into view;
    // :disappear starts only after it reached the disappear threshold
    float view_peak;
    // paint_bounds: the device-pixel rect every draw of this node and its
    // descendants stays inside, computed after layout each frame (own
    // bounds plus the effect margin, unioned with the children). The
    // paint walk skips a subtree whose paint_bounds miss the current clip.
    // paint_bounded is FALSE when the node or a descendant carries a
    // transform or a navigation effect: their pixels can land anywhere,
    // so such a subtree is never skipped.
    gui_rect_t     paint_bounds;
    boole          paint_bounded;
    // the largest backdrop-filter blur (device pixels of sigma) on this
    // node or any descendant, from the same post-layout pass: an effect
    // group cut to its visible rect keeps that many pixels of margin so a
    // descendant's backdrop blur reads real content at the cut edge
    float          subtree_backdrop_px;
    // per-frame layout memo for labels (scene__layout_node): the frame
    // and the hash of every input the text layout reads, with the size
    // it produced. The flows lay a label out many times per frame with
    // the same inputs; later calls only move the box.
    uint64         layout_memo_frame;
    uint64         layout_memo_key;
    float          layout_memo_w;
    float          layout_memo_h;
};

// event kinds delivered to handlers. extends as new event types are added.
typedef enum gui_event_type_e
{
    GUI_EVENT_TYPE__CLICK,
    GUI_EVENT_TYPE__CHANGE
} gui_event_type_e;

// one event delivered to a handler. the anonymous union body holds
// payload that varies by type. ev.mouse.* for click events,
// ev.change.scalar for slider on_change.
typedef struct gui_event_t
{
    gui_event_type_e type;
    gui_node_t*      sender;
    union
    {
        struct
        {
            int64 x;
            int64 y;
            int64 button;
        } mouse;
        struct
        {
            float scalar;
        } change;
        // change.color: RGBA 0..1 payload for colorpicker on_change events.
        // Distinct from `change.scalar` so a single handler can read either
        // without guessing; the event type + sender's node type disambiguate.
        struct
        {
            gui_color_t value;
        } color;
        // popup dismiss payload. `confirmed` = TRUE when the user clicked
        // OK / Yes, FALSE for Cancel / No. For type="option-select" the
        // popup also writes the selected option index into `index`.
        struct
        {
            boole confirmed;
            int64 index;
        } popup;
        // datepicker on_change: the selected date.
        struct
        {
            int64 year;
            int64 month;
            int64 day;
        } date;
        // filepicker on_change: the full path of the entry the user
        // activated (valid during the call; copy it to keep it) and
        // whether it is a directory (entered) or a file (picked).
        struct
        {
            const char* path;
            boole       is_dir;
        } file;
    };
} gui_event_t;

typedef void (*gui_handler_fn)(gui_event_t* ev);

#endif
