// widget_keyboard.c - on-screen ASCII keyboard.
// QWERTY layout with three pages:
// PAGE_LETTERS  qwerty + shift/backspace/return; shift toggles case
// PAGE_NUMBERS  1234567890 + common punctuation
// PAGE_SYMBOLS  extra symbols ($ # & @ / ...)
// Drawn as a self-contained overlay at the bottom of the viewport
// (40% height). Taps on keys emit scene__on_char() to the currently
// focused node; modifier keys (shift / page / backspace / return)
// never leave the keyboard's own state machine.
// Triggered two ways:
// 1. Host explicitly: scene__show_keyboard() / scene__hide_keyboard().
// 2. Auto: on Android, the platform layer calls scene__show_keyboard
// whenever scene__focus() is an <input> / <textarea>. On Windows,
// auto-show is off unless the host calls
// scene__enable_virtual_keyboard(TRUE).
// Layout ownership: the keyboard inserts itself as a top-level child
// of the scene root when shown, and removes itself when hidden. That
// way the main scene walk handles its input routing and draw call
// without needing a special overlay path.

#include <string.h>

#include "types.h"
#include <stdint.h>

#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "renderer.h"
#include "font.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

typedef enum _keyboard_internal__page_e
{
    _KEYBOARD_INTERNAL__PAGE_LETTERS = 0,
    _KEYBOARD_INTERNAL__PAGE_NUMBERS = 1,
    _KEYBOARD_INTERNAL__PAGE_SYMBOLS = 2,
} _keyboard_internal__page_e;

typedef struct _keyboard_internal__state_t
{
    _keyboard_internal__page_e page;        // 0x0
    boole                    shift;       // 0x4
    // sticky ctrl (like shift); cleared after next char dispatch.
    boole                    ctrl;        // 0x5
    // Last-frame geometry: we store per-key rects during emit_draws
    // so the on_mouse_down hit test can look up which key was tapped
    // without re-computing layout. Max key slots covers the widest
    // row (~13 keys).
    gui_rect_t key_rects[64];               // 0x8
    // char value for each key; 0 for modifiers.
    char     key_chars[64];               // 0x408
    // 0=char, 1=shift, 2=backspace, 3=return, 4=page-num, 5=page-sym, 6=page-abc, 7=space, 8=ctrl.
    int      key_modifier[64];            // 0x448
    int      key_count;                   // 0x548
    // Index of the key currently being pressed (mouse_down → mouse_up
    // window). Used by emit_draws to flash a lighter bg so the user
    // sees which key they hit. -1 means no key pressed.
    // For a quick tap, mouse_down and mouse_up can both land in the
    // same tick BEFORE emit_draws runs, so the highlight would never
    // reach the screen. `pressed_release_countdown` holds the flash
    // visible for a few frames after release: mouse_up sets it to
    // N, emit_draws decrements each frame, the pressed state is
    // considered active while the countdown is > 0.
    int      pressed_index;               // 0x54C
    int      pressed_release_countdown;   // 0x550
    // Long-press / auto-repeat. When a REPEATABLE key (character,
    // backspace, space) is held, emit_draws tracks how many frames
    // the finger has been down. After an initial delay the key
    // starts auto-firing at a faster interval -- same behaviour as
    // a physical keyboard's key repeat. `pressed_is_repeatable` is
    // set on mouse_down based on the key's modifier id; `hold_frames`
    // counts from 0 while held and is reset on mouse_up.
    // The non-repeatable keys (shift, ctrl, page-switcher, return)
    // don't auto-repeat because they either toggle state (shift /
    // ctrl) or have submit semantics (return, page-switch). Return
    // could be held for "multiple newlines" but that's not a
    // conventional phone-keyboard behaviour; exclude.
    boole    pressed_is_repeatable;       // 0x554
    int      pressed_hold_frames;         // 0x558
    // Swipe-down-to-dismiss state. On mouse_down we record the
    // starting pointer Y; on_mouse_drag compares against it. Once
    // the finger has travelled more than _SWIPE_DISMISS_THRESHOLD_PX
    // DOWNWARD (positive dy), we call scene__hide_keyboard and
    // mark the gesture as consumed so the subsequent mouse_up
    // doesn't do anything weird. The on_mouse_down dispatch of the
    // under-finger character still fires before the drag does --
    // accepted cost for this gesture's simplicity; users doing an
    // intentional dismiss-swipe can undo the one stray character.
    int      swipe_start_y;               // 0x55C
    boole    swipe_dismissed;             // 0x560
} _keyboard_internal__state_t;

// Public sizing knobs (set by widget_keyboard__set_height_pct /
// _font_size_pct, defined further down). Declared up here so the
// layout + emit helpers below can read them. Default values match
// the legacy hard-coded constants -- if a host never calls the
// setters, the keyboard looks identical to the pre-knob build.
static int _keyboard_internal__height_pct    = 40;
static int _keyboard_internal__font_size_pct = 100;

// Auto-repeat tuning. 30 frames initial delay ~ 500 ms on a 60 Hz
// shell render loop -- matches a typical keyboard. Repeat interval
// 2 frames ~ 33 ms -- matches a standard wl_keyboard repeat_info of
// 25 keys/sec.
#define _KEYBOARD_INTERNAL__REPEAT_DELAY_FRAMES    30
#define _KEYBOARD_INTERNAL__REPEAT_INTERVAL_FRAMES  2

// Vertical swipe distance (in logical pixels) that triggers
// keyboard dismiss. ~120 px is ~1.5 key rows tall; smaller would
// fire on accidental finger wiggles, larger would feel sluggish.
// User's phrasing was "swipe from where the keyboard starts to
// bottom", i.e. a clear full-length downward drag.
#define _KEYBOARD_INTERNAL__SWIPE_DISMISS_THRESHOLD_PX 120

static _keyboard_internal__state_t* _keyboard_internal__state_of(gui_node_t* n)
{
    if (n->user_data == NULL_POINTER)
    {
        n->user_data = GUI_CALLOC_T(1, sizeof(_keyboard_internal__state_t), MM_TYPE__GENERIC);
        _keyboard_internal__state_t* st = (_keyboard_internal__state_t*)n->user_data;
        // calloc zero-initializes; -1 sentinel for pressed_index.
        if (st != NULL_POINTER)
        {
            st->pressed_index = -1;
        }
    }
    return (_keyboard_internal__state_t*)n->user_data;
}

// Layouts as rows of key descriptors. Each row is a null-terminated
// string; a literal space inside the string is a literal spacer key
// (takes up one slot but produces no character).
static const char* _KEYBOARD_INTERNAL__ROWS_LETTERS[] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};
static const char* _KEYBOARD_INTERNAL__ROWS_NUMBERS[] = {
    "1234567890",
    "-/:;()$&@\"",
    ".,?!'",
};
static const char* _KEYBOARD_INTERNAL__ROWS_SYMBOLS[] = {
    "[]{}#%^*+=",
    "_\\|~<>",
    "`",
};

// Paint + record hit rects for one row. x is the left edge; y the top.
// Returns the height consumed.
static float _keyboard_internal__emit_row(gui_node_t* n, _keyboard_internal__state_t* st, const char* chars, float x, float y, float row_w, float key_h, float gap, int count)
{
    int n_keys = (int)stdlib__strlen(chars);
    // Empty row would divide by zero below and propagate NaN
    // through every subsequent rect (NaN comparisons are false,
    // so the clamp wouldn't fire either). No real row is empty
    // today but we treat it as "render nothing, take no height".
    if (n_keys <= 0)
    {
        (void)count;
        return 0.0f;
    }
    float key_w = (row_w - gap * (float)(n_keys - 1)) / (float)n_keys;
    // Min key width scales with UI density. Previously this was a
    // raw 16.0f -- on a 3.0x Android phone that clamped keys to 16
    // physical pixels (~5.3 logical), unusable. Pull scale from
    // scene__scale() so the min stays at ~16 logical pixels
    // regardless of density.
    float min_key = 16.0f * scene__scale();
    if (key_w < min_key)
    {
        key_w = min_key;
    }

    // Key backgrounds render at 60% opacity so the content behind
    // the keyboard (terminal, focused input field, etc.) stays
    // partially visible through each key. Text stays fully opaque
    // so labels are always readable. Panel bg in emit_draws matches
    // the same 0.60 alpha.
    gui_color_t key_bg;         key_bg.r = 0.18f;         key_bg.g = 0.19f;         key_bg.b = 0.24f;         key_bg.a = 0.60f;
    gui_color_t key_bg_pressed; key_bg_pressed.r = 0.42f; key_bg_pressed.g = 0.46f; key_bg_pressed.b = 0.60f; key_bg_pressed.a = 0.60f;
    gui_color_t key_text;       key_text.r = 0.95f;       key_text.g = 0.96f;       key_text.b = 0.99f;       key_text.a = 1.0f;
    float     op = n->effective_opacity;
    key_bg.a         *= op;
    key_bg_pressed.a *= op;
    key_text.a       *= op;

    // Font size derived from key height: 55% of the visual key
    // height leaves room for top/bottom padding without truncation.
    // This is what makes the keyboard legible across densities --
    // on a 2.0x phone the key is ~80 px tall and we render at ~44pt;
    // on a 1.0x desktop the key is ~40 px and we render at ~22pt.
    float key_font_size = key_h * 0.55f;
    // User font-scale multiplier. 100 = no change, 50 = half, 200
    // = double. Applied after the key-height-derived auto-size so
    // smaller-than-typical keyboards stay readable at 200% and
    // taller keyboards don't end up with absurdly large glyphs at
    // 100%.
    key_font_size = key_font_size * (float)_keyboard_internal__font_size_pct / 100.0f;
    if (key_font_size < 10.0f)
    {
        key_font_size = 10.0f;
    }

    float cursor_x = x;
    for (int i = 0; i < n_keys; i++)
    {
        gui_rect_t r;
        r.x = cursor_x;
        r.y = y;
        r.w = key_w;
        r.h = key_h;
        // Use the pressed color only for the key actually being
        // held. `st->pressed_index` is the slot about to be pushed
        // below (st->key_count is the index of the next-to-write).
        boole is_pressed = (st->pressed_index == st->key_count);
        renderer__submit_rect(r, is_pressed ? key_bg_pressed : key_bg, 6.0f);

        // Letter label -- respects shift on the letters page.
        char ch = chars[i];
        if (st->page == _KEYBOARD_INTERNAL__PAGE_LETTERS && st->shift && ch >= 'a' && ch <= 'z')
        {
            ch = (char)(ch - 'a' + 'A');
        }
        char label[2] = { ch, 0 };
        gui_font_t* f = font__at(NULL_POINTER, key_font_size);
        if (f != NULL_POINTER)
        {
            float tw = font__measure(f, label);
            float ascent = font__ascent(f);
            float line_h = font__line_height(f);
            font__draw(f, r.x + (r.w - tw) * 0.5f, r.y + (r.h - line_h) * 0.5f + ascent, key_text, label);
        }

        // Record for hit test.
        if (st->key_count < (int)(sizeof(st->key_rects) / sizeof(st->key_rects[0])))
        {
            int slot = st->key_count++;
            st->key_rects[slot]    = r;
            st->key_chars[slot]    = ch;
            st->key_modifier[slot] = 0;
        }
        cursor_x += key_w + gap;
    }
    (void)count;
    return key_h;
}

// Paint one modifier button (shift, backspace, space, return, page-switcher).
// Draws with the regular key look but a slightly brighter tint and a
// text label instead of a single char.
static void _keyboard_internal__emit_modifier(gui_node_t* n, _keyboard_internal__state_t* st, int modifier_id, char* label, gui_rect_t r)
{
    // 60% opacity on modifier key bg to match the regular keys +
    // panel in keyboard_emit_draws. Labels stay fully opaque.
    gui_color_t bg; bg.r = 0.12f; bg.g = 0.13f; bg.b = 0.17f; bg.a = 0.60f;
    gui_color_t text; text.r = 0.80f; text.g = 0.83f; text.b = 0.90f; text.a = 1.0f;
    float op = n->effective_opacity;
    bg.a *= op;
    text.a *= op;

    // Shift / ctrl "active" visual (sticky modifier state). Latched
    // modifier keys paint in the pressed-accent color even while not
    // physically held, so the user can tell at a glance whether
    // their next character will be shifted / ctrled.
    if ((modifier_id == 1 && st->shift) || (modifier_id == 8 && st->ctrl))
    {
        bg.r = 0.27f; bg.g = 0.29f; bg.b = 0.37f;
    }

    // Pressed highlight: lighten bg while the finger is down on
    // this slot. st->pressed_index is the slot we're about to push,
    // same trick as the per-char keys above.
    if (st->pressed_index == st->key_count)
    {
        bg.r += 0.12f; bg.g += 0.12f; bg.b += 0.15f;
        if (bg.r > 1.0f) bg.r = 1.0f;
        if (bg.g > 1.0f) bg.g = 1.0f;
        if (bg.b > 1.0f) bg.b = 1.0f;
    }
    renderer__submit_rect(r, bg, 6.0f);

    // Modifier labels are typically short (1-4 chars) and fit in
    // the same height budget as letter keys. Use the same dynamic
    // sizing: 0.40x key height (modifiers carry more text than
    // letters so we shrink slightly).
    float mod_font_size = r.h * 0.40f;
    mod_font_size = mod_font_size * (float)_keyboard_internal__font_size_pct / 100.0f;
    if (mod_font_size < 9.0f)
    {
        mod_font_size = 9.0f;
    }
    gui_font_t* f = font__at(NULL_POINTER, mod_font_size);
    if (f != NULL_POINTER && label != NULL_POINTER)
    {
        float tw = font__measure(f, label);
        float ascent = font__ascent(f);
        float line_h = font__line_height(f);
        font__draw(f, r.x + (r.w - tw) * 0.5f, r.y + (r.h - line_h) * 0.5f + ascent, text, label);
    }
    if (st->key_count < (int)(sizeof(st->key_rects) / sizeof(st->key_rects[0])))
    {
        int slot = st->key_count++;
        st->key_rects[slot]    = r;
        st->key_chars[slot]    = 0;
        st->key_modifier[slot] = modifier_id;
    }
}

static void keyboard_layout(gui_node_t* n, float x, float y, float avail_w, float avail_h, float scale)
{
    (void)x; (void)y; (void)scale;
    // Position ourselves at the bottom 40% of the viewport,
    // regardless of where the parent tried to lay us out. Matches
    // on-screen keyboards' expected position.
    int64 vw = 0, vh = 0;
    scene__viewport(&vw, &vh);
    // Height = user-configurable percent of viewport, clamped via
    // the setter. Legacy hard-coded value was 0.4 (40%).
    float frac = (float)_keyboard_internal__height_pct / 100.0f;
    float h = (float)vh * frac;
    if (h < 160.0f)
    {
        h = 160.0f;
    }
    n->bounds.x = 0.0f;
    n->bounds.y = (float)vh - h;
    n->bounds.w = (avail_w > 0.0f && vw <= 0) ? avail_w : (float)vw;
    n->bounds.h = h;
    if (vw <= 0 && n->bounds.w <= 0.0f)
    {
        n->bounds.w = 640.0f;
    }
    if (avail_h <= 0.0f) { /* silence warning */ }
}

// Forward decl -- used by emit_draws for auto-repeat dispatch.
static void _keyboard_internal__dispatch_slot(gui_node_t* n, _keyboard_internal__state_t* st, int slot, boole is_repeat);

static void keyboard_emit_draws(gui_node_t* n, float scale)
{
    _keyboard_internal__state_t* st = _keyboard_internal__state_of(n);
    if (st == NULL_POINTER)
    {
        return;
    }

    st->key_count = 0;

    // Consume the pressed-release countdown. Started by
    // keyboard_on_mouse_up so quick taps flash the key for a few
    // frames. When it ticks to zero we stop highlighting.
    if (st->pressed_release_countdown > 0)
    {
        st->pressed_release_countdown--;
        if (st->pressed_release_countdown == 0)
        {
            st->pressed_index = -1;
            st->pressed_is_repeatable = FALSE;
            st->pressed_hold_frames = 0;
        }
    }

    // Auto-repeat. Fires only while the finger is actually held on a
    // repeatable key (char / backspace / space). pressed_release_
    // countdown > 0 means the finger has released and we're just
    // flashing the key -- skip repeat in that case.
    if (st->pressed_index >= 0 && st->pressed_release_countdown == 0 && st->pressed_is_repeatable)
    {
        st->pressed_hold_frames++;
        if (st->pressed_hold_frames >= _KEYBOARD_INTERNAL__REPEAT_DELAY_FRAMES)
        {
            int since_delay = st->pressed_hold_frames - _KEYBOARD_INTERNAL__REPEAT_DELAY_FRAMES;
            if ((since_delay % _KEYBOARD_INTERNAL__REPEAT_INTERVAL_FRAMES) == 0)
            {
                // Re-dispatch the held key. The dispatch helper
                // increments key_count internally as it re-emits
                // the bounds -- but emit_draws resets key_count at
                // the top, so the re-emit happens later. Snapshot
                // the slot index now; dispatch uses the OLD
                // key_chars/key_modifier content which was written
                // the previous frame and is still valid.
                _keyboard_internal__dispatch_slot(n, st, st->pressed_index, TRUE);
            }
        }
    }

    // Drain pending image batch BEFORE the keyboard panel rect.
    // Without this, the foreign-buffer image of the underlying
    // app (process-window) would draw AFTER the keyboard's rects
    // in the renderer's batched flush order (rect -> image ->
    // text), covering the panel entirely. The user would then see
    // the app showing through where the keyboard should be opaque.
    renderer__flush_pending_draws();

    // Panel background. Semi-transparent so the content behind the
    // keyboard (foot terminal, text field, etc.) stays visible even
    // when the keyboard is up -- stops the keyboard from fully
    // occluding the app we're typing into.
    gui_color_t panel; panel.r = 0.08f; panel.g = 0.09f; panel.b = 0.12f; panel.a = 0.60f;
    panel.a *= n->effective_opacity;
    renderer__submit_rect(n->bounds, panel, 0.0f);

    // Layout grid: 4 rows (3 letter rows + 1 bottom mod row) filling
    // the widget height with vertical gaps.
    float margin = 8.0f;
    float gap    = 4.0f;
    float rows   = 4.0f;
    float row_h  = (n->bounds.h - margin * 2.0f - gap * (rows - 1.0f)) / rows;
    float row_w  = n->bounds.w - margin * 2.0f;
    float x0     = n->bounds.x + margin;
    float y      = n->bounds.y + margin;

    // Modifier key width: proportional to row width so shift / ctrl
    // / backspace / return don't look tiny on a wide panel. On 1080
    // width that's ~151 px per modifier, enough to hold "SHIFT" and
    // "CTRL" labels without clipping. Minimum 96 logical pixels
    // scaled so it stays tappable on small screens too.
    float mod_w = row_w * 0.14f;
    float mod_w_min = 96.0f * scene__scale();
    if (mod_w < mod_w_min)
    {
        mod_w = mod_w_min;
    }

    const char** letter_rows = _KEYBOARD_INTERNAL__ROWS_LETTERS;
    if      (st->page == _KEYBOARD_INTERNAL__PAGE_NUMBERS) letter_rows = _KEYBOARD_INTERNAL__ROWS_NUMBERS;
    else if (st->page == _KEYBOARD_INTERNAL__PAGE_SYMBOLS) letter_rows = _KEYBOARD_INTERNAL__ROWS_SYMBOLS;

    // Row 1 + 2 : full-width char rows.
    _keyboard_internal__emit_row(n, st, letter_rows[0], x0, y, row_w, row_h, gap, 0);
    y += row_h + gap;
    _keyboard_internal__emit_row(n, st, letter_rows[1], x0, y, row_w, row_h, gap, 0);
    y += row_h + gap;

    // Row 3 : shift (left), letters, backspace (right).
    {
        // Shift (only on letters page; numbers/symbols don't need it).
        if (st->page == _KEYBOARD_INTERNAL__PAGE_LETTERS)
        {
            gui_rect_t r; r.x = x0; r.y = y; r.w = mod_w; r.h = row_h;
            _keyboard_internal__emit_modifier(n, st, 1, "SHIFT", r);
        }
        float letters_x = x0 + (st->page == _KEYBOARD_INTERNAL__PAGE_LETTERS ? mod_w + gap : 0.0f);
        float letters_w = row_w - (st->page == _KEYBOARD_INTERNAL__PAGE_LETTERS ? 2.0f * (mod_w + gap) : (mod_w + gap));
        _keyboard_internal__emit_row(n, st, letter_rows[2], letters_x, y, letters_w, row_h, gap, 0);
        // Backspace always.
        gui_rect_t bs; bs.x = x0 + row_w - mod_w; bs.y = y; bs.w = mod_w; bs.h = row_h;
        _keyboard_internal__emit_modifier(n, st, 2, "\xe2\x8c\xab", bs); // U+232B ERASE TO THE LEFT
    }
    y += row_h + gap;

    // Row 4: [123] [CTRL] [space] [return]. Four slots; three are
    // mod_w wide and space fills the remainder. CTRL is a sticky
    // modifier like SHIFT; meek-shell's char redirect reads the
    // current modifier bitmask from scene__get_active_modifiers()
    // and forwards ctrl as wl_keyboard.modifiers(ctrl_bit) before
    // the next route_keyboard_key. Without this, ctrl-C / ctrl-D /
    // etc. aren't reachable from the on-screen keyboard -- critical
    // for any terminal use.
    {
        char* switcher_label = "123";
        int   switcher_mod   = 4;
        if (st->page == _KEYBOARD_INTERNAL__PAGE_NUMBERS)
        {
            switcher_label = "=\\<";
            switcher_mod = 5;
        }
        else if (st->page == _KEYBOARD_INTERNAL__PAGE_SYMBOLS)
        {
            switcher_label = "ABC";
            switcher_mod = 6;
        }
        gui_rect_t pg; pg.x = x0; pg.y = y; pg.w = mod_w; pg.h = row_h;
        _keyboard_internal__emit_modifier(n, st, switcher_mod, switcher_label, pg);

        gui_rect_t ct; ct.x = x0 + mod_w + gap; ct.y = y; ct.w = mod_w; ct.h = row_h;
        _keyboard_internal__emit_modifier(n, st, 8, "CTRL", ct);

        float space_x = x0 + 2.0f * (mod_w + gap);
        float space_w = row_w - 3.0f * (mod_w + gap);
        gui_rect_t sp; sp.x = space_x; sp.y = y; sp.w = space_w; sp.h = row_h;
        _keyboard_internal__emit_modifier(n, st, 7, "space", sp);

        gui_rect_t rt; rt.x = x0 + row_w - mod_w; rt.y = y; rt.w = mod_w; rt.h = row_h;
        _keyboard_internal__emit_modifier(n, st, 3, "RETURN", rt);
    }
}

// Central dispatch. Called from on_mouse_down for the initial tap AND
// from emit_draws when auto-repeat fires. `is_repeat` distinguishes
// the two: on a repeat, we don't toggle shift off (shift only
// latches for one character, so holding a repeatable key after
// shift-typed isn't meaningful), and we don't re-run page-switch
// state changes.
// Callers guarantee slot is in range and the state is still valid
// (no destruction between the original press and the dispatch).
static void _keyboard_internal__dispatch_slot(gui_node_t* n, _keyboard_internal__state_t* st, int slot, boole is_repeat)
{
    int mod = st->key_modifier[slot];
    gui_node_t* focus = scene__focus();

    // Compose the active modifier bitmask (shift | ctrl) and push
    // it to the scene before dispatching. The char redirect in
    // meek-shell reads it back via scene__get_active_modifiers()
    // and forwards to wl_keyboard.modifiers before the route_
    // keyboard_key -- that's how ctrl-C etc. reach foot's pty.
    // Bit layout matches xkbcommon's default keymap + standard
    // wl_keyboard.modifiers:
    // shift: 1 << 0
    // ctrl:  1 << 2
    uint32_t mask = 0;
    if (st->shift)
    {
        mask |= (1u << 0);
    }
    if (st->ctrl)
    {
        mask |= (1u << 2);
    }
    scene__set_active_modifiers(mask);

    if (mod == 0)
    {
        // Regular character. Apply shift on the letters page.
        char ch = st->key_chars[slot];
        // Auto-release CTRL after one char (ctrl-C etc. is a one-
        // shot combo, not a mode). SHIFT now STAYS LATCHED across
        // characters so the user can type a run of uppercase letters
        // without re-tapping shift between each one; this matches a
        // caps-lock toggle. Tapping SHIFT again clears it. Without
        // this, the old "shift auto-clears after one char" behavior
        // surprised users into thinking the keyboard had no
        // lowercase / uppercase mode -- you'd press SHIFT, type one
        // letter, and instantly the keyboard reverted, so visually
        // it looked like there was no switch.
        if (is_repeat == FALSE)
        {
            if (st->ctrl)
            {
                st->ctrl = FALSE;
            }
        }
        scene__on_char((uint)ch);
        (void)focus;
    }
    else if (mod == 1)
    {
        if (is_repeat == FALSE)
        {
            st->shift = st->shift == FALSE;
        }
    }
    else if (mod == 2)
    {
        // Backspace. Two dispatch modes:
        // * Redirect installed (meek-shell in fullscreen):
        // route as '\b' via scene__on_char so meek-shell's
        // char->keycode map turns it into KEY_BACKSPACE on
        // the wl_keyboard wire.
        // * No redirect (local widget typing): VK_BACK via
        // scene__on_key, which widget_input / widget_
        // textarea listen for in their on_key vtables.
        if (scene__has_char_redirect())
        {
            scene__on_char((uint)0x08);
        }
        else if (focus != NULL_POINTER && focus != n)
        {
            scene__deliver_key(focus, 0x08, TRUE);
            scene__deliver_key(focus, 0x08, FALSE);
        }
    }
    else if (mod == 3)
    {
        // Return. Same two-mode split as backspace.
        if (scene__has_char_redirect())
        {
            scene__on_char((uint)'\n');
        }
        else if (focus != NULL_POINTER && focus != n)
        {
            scene__deliver_key(focus, 0x0D, TRUE);
            scene__deliver_key(focus, 0x0D, FALSE);
        }
    }
    else if (mod == 4 || mod == 5 || mod == 6)
    {
        if (is_repeat)
        {
            return;
        }
        if      (mod == 4)
        {
            st->page = _KEYBOARD_INTERNAL__PAGE_NUMBERS;
        }
        else if (mod == 5)
        {
            st->page = _KEYBOARD_INTERNAL__PAGE_SYMBOLS;
        }
        else
        {
            st->page = _KEYBOARD_INTERNAL__PAGE_LETTERS;
        }
        // Clear the pressed visual on page switch. Otherwise
        // pressed_index still holds the slot index of the old page
        // (the "123" / "ABC" button), and on the new page that
        // same index points at a totally different key whose bg
        // gets painted in the "pressed" accent color for a few
        // frames ("L" briefly highlighted after 123→letters was
        // exactly this bug).
        st->pressed_index = -1;
        st->pressed_release_countdown = 0;
        st->pressed_is_repeatable = FALSE;
        st->pressed_hold_frames = 0;
    }
    else if (mod == 7)
    {
        scene__on_char((uint)' ');
    }
    else if (mod == 8)
    {
        // CTRL latch -- same toggle semantics as SHIFT. Cleared
        // after the next char dispatch via the mod==0 branch above.
        if (is_repeat == FALSE)
        {
            st->ctrl = st->ctrl == FALSE;
        }
    }
}

static void keyboard_on_mouse_down(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)button;
    _keyboard_internal__state_t* st = _keyboard_internal__state_of(n);
    if (st == NULL_POINTER)
    {
        return;
    }
    // Record the starting Y for the swipe-down-to-dismiss gesture.
    // on_mouse_drag compares against this on every move event;
    // crossing the threshold calls scene__hide_keyboard(). The
    // initial key press still dispatches below -- accepted cost
    // (see the swipe_start_y comment in the state struct).
    st->swipe_start_y   = (int)y;
    st->swipe_dismissed = FALSE;
    // Linear scan of recorded key rects. Count is ~30-40 so a
    // scan is cheap.
    for (int i = 0; i < st->key_count; i++)
    {
        gui_rect_t r = st->key_rects[i];
        if ((float)x < r.x || (float)y < r.y || (float)x >= r.x + r.w || (float)y >= r.y + r.h)
        {
            continue;
        }
        // Set pressed_index; DON'T touch countdown here. Countdown
        // is only armed on mouse_up (for the quick-tap flash). While
        // held, pressed_index stays set and countdown stays 0;
        // emit_draws sees countdown==0 and keeps pressed_index live
        // indefinitely.
        st->pressed_index = i;
        st->pressed_hold_frames = 0;
        // Only BACKSPACE (mod 2) auto-repeats on hold -- that's
        // the one key where holding = delete-many-chars-fast is
        // idiomatic. Characters, space, return, shift/ctrl all
        // require a fresh tap per event: a physical keyboard
        // would repeat them but on-screen-keyboard users holding
        // a letter key usually mean "I pressed it, that's all" --
        // accidentally spamming 'h' is more annoying than
        // usefully fast typing. User called this out explicitly.
        int mod = st->key_modifier[i];
        st->pressed_is_repeatable = (mod == 2);

        log_trace("%s %d %s %d %s %d", "widget_keyboard: pressed key slot", i, "mod=", mod, "repeatable=", st->pressed_is_repeatable);
        _keyboard_internal__dispatch_slot(n, st, i, /*is_repeat*/ FALSE);
        return;
    }
}

// Swipe-down-to-dismiss. Fired by scene on every mouse_move while
// the keyboard is the pressed node. Returns TRUE to suppress hover
// updates during a drag -- consistent with other drag-capturing
// widgets (slider, colorpicker).
// We compare the current Y against the mouse_down start Y. If the
// finger has travelled _SWIPE_DISMISS_THRESHOLD_PX DOWNWARD, hide
// the keyboard. `swipe_dismissed` latches so the same gesture
// doesn't re-fire hide on every subsequent move event.
// Upward drags (negative dy) do nothing -- no equivalent "summon
// keyboard" gesture exists here; the keyboard is shown explicitly
// by the host app via scene__show_keyboard() on focus of an input
// field.
static boole keyboard_on_mouse_drag(gui_node_t* n, int64 x, int64 y)
{
    (void)x;
    _keyboard_internal__state_t* st = (_keyboard_internal__state_t*)n->user_data;
    if (st == NULL_POINTER)
    {
        return FALSE;
    }
    if (st->swipe_dismissed)
    {
        return TRUE;
    }

    int64 dy = y - (int64)st->swipe_start_y;
    if (dy >= _KEYBOARD_INTERNAL__SWIPE_DISMISS_THRESHOLD_PX)
    {
        log_info("%s %lld %s", "widget_keyboard: swipe-down dismiss (dy=", (long long)dy, ")");
        st->swipe_dismissed = TRUE;
        // Clear pressed state so the fading countdown from the
        // under-finger key doesn't linger after the keyboard is
        // re-shown next time. pressed_index goes to -1 so next
        // emit_draws on a re-show starts clean.
        st->pressed_index             = -1;
        st->pressed_release_countdown = 0;
        st->pressed_is_repeatable     = FALSE;
        st->pressed_hold_frames       = 0;
        scene__hide_keyboard();
    }
    return TRUE;
}

static void keyboard_on_mouse_up(gui_node_t* n, int64 x, int64 y, int64 button)
{
    (void)x; (void)y; (void)button;
    // Don't clear pressed_index immediately. A quick tap can fire
    // down + up in the same tick, before emit_draws paints. Arm a
    // countdown instead -- emit_draws consumes it over the next
    // few frames, the user sees the flash, and THEN the highlight
    // clears.
    _keyboard_internal__state_t* st = (_keyboard_internal__state_t*)n->user_data;
    if (st != NULL_POINTER)
    {
        // Don't clear pressed_index here -- arm a short countdown
        // that emit_draws consumes. Rationale: while the user's
        // finger is held on a key, down fires but up doesn't, so
        // pressed_index stays live across many frames (the "while
        // held" behaviour the user asked for). For a quick tap
        // where down + up both land in the same tick BEFORE
        // emit_draws runs, clearing synchronously would erase the
        // press-state before anything hit the screen -- hence the
        // countdown: 15 frames (~250 ms) of flash after release,
        // guaranteeing visibility even for tap-tap-tap typing.
        st->pressed_release_countdown = 15;
        // Stop auto-repeat on release. pressed_hold_frames will be
        // re-zeroed when the countdown expires, but flagging non-
        // repeatable up front stops emit_draws from firing one
        // more repeat between here and the countdown starting.
        st->pressed_is_repeatable = FALSE;
        log_trace("%s", "widget_keyboard: released (countdown 15 armed)");
    }
}

static void keyboard_init_defaults(gui_node_t* n)
{
    _keyboard_internal__state_of(n);
}

static void keyboard_on_destroy(gui_node_t* n)
{
    if (n->user_data != NULL_POINTER)
    {
        GUI_FREE(n->user_data);
        n->user_data = NULL_POINTER;
    }
}

static const widget_vtable_t g_keyboard_vtable = {
    .type_name       = "keyboard",
    .init_defaults   = keyboard_init_defaults,
    .layout          = keyboard_layout,
    .emit_draws      = keyboard_emit_draws,
    .on_mouse_down   = keyboard_on_mouse_down,
    .on_mouse_up     = keyboard_on_mouse_up,
    .on_mouse_drag   = keyboard_on_mouse_drag,
    .on_destroy      = keyboard_on_destroy,
    .consumes_click  = TRUE,
    // Tapping a key must not move focus off the text widget being
    // edited -- otherwise the auto-hide gate in scene__set_focus
    // would hide the keyboard the moment you touched it.
    .preserves_focus = TRUE,
    // Pin drag events on the keyboard widget so the swipe-down-to-
    // dismiss gesture survives the touch state machine's transition
    // to SCROLLING after ~16 px of vertical movement. Without this,
    // the platform layer (Android in particular, also any host that
    // adopts a similar slop-based policy) redirects vertical motion
    // away from the keyboard's on_mouse_drag handler the moment the
    // user starts swiping down. No-op on hosts with no slop machine
    // (current Wayland-client backend), but documents the intent and
    // keeps the gesture reliable when ported. Per
    // `meek-compositor/session/bugs_to_investigate.md` #4.
    .captures_drag   = TRUE,
};

void widget_keyboard__register(void)
{
    widget_registry__register(GUI_NODE_TYPE__KEYBOARD, &g_keyboard_vtable);
}

// Public sizing knobs. The keyboard's layout method reads the
// globals declared at the top of this file each frame instead of
// the prior hard-coded constants, so a host (meek-shell) can plumb
// user settings into the on-screen keyboard's height + key-label
// size without forking the widget.
// height_pct: 20..60. 40 == legacy (40% of viewport).
// font_size_pct: 50..200. 100 == legacy (widget auto-computes).
/**
 * @brief on-screen keyboard sizing knobs. Both are clamped at the widget side (height_pct 20..60, font_size_pct 50..200) so a host with a bad config can't break the layout. Default values preserve the legacy behavior (40% tall, 100% font scale) when the host never calls them.
 *
 * @return void
 */
void widget_keyboard__set_height_pct(int pct)
{
    if (pct < 20)
    {
        pct = 20;
    }
    if (pct > 60)
    {
        pct = 60;
    }
    _keyboard_internal__height_pct = pct;
}

/**
 * @brief widget_keyboard__set_font_size_pct
 *
 * @return void
 */
void widget_keyboard__set_font_size_pct(int pct)
{
    if (pct < 50)
    {
        pct = 50;
    }
    if (pct > 200)
    {
        pct = 200;
    }
    _keyboard_internal__font_size_pct = pct;
}

int widget_keyboard__get_height_pct(void)    { return _keyboard_internal__height_pct; }
int widget_keyboard__get_font_size_pct(void) { return _keyboard_internal__font_size_pct; }

// ============================================================================
// scene__* keyboard helpers (declared in scene.h).
// ============================================================================

static gui_node_t* _keyboard_internal__current = NULL_POINTER;
static boole     _keyboard_internal__enabled_default =
#if defined(__ANDROID__) || defined(ANDROID)
    TRUE;
#else
    FALSE;
#endif
static boole _keyboard_internal__enabled_set = FALSE;
static boole _keyboard_internal__enabled     = FALSE;

static boole _keyboard_internal__is_enabled(void)
{
    if (_keyboard_internal__enabled_set == FALSE)
    {
        return _keyboard_internal__enabled_default;
    }
    return _keyboard_internal__enabled;
}

/**
 * @brief opt in/out of automatic keyboard management. When TRUE, scene will show the keyboard the next time an <input> / <textarea> takes focus and hide it when focus leaves. Defaults TRUE on Android, FALSE on Windows. Host apps that want to force-show on Windows (e.g. a kiosk with only touch input) pass TRUE.
 *
 * @param boole enabled
 *
 * @return void
 */
void scene__set_virtual_keyboard_enabled(boole enabled)
{
    _keyboard_internal__enabled     = enabled;
    _keyboard_internal__enabled_set = TRUE;
    // If we just turned it off while a keyboard is visible, hide it.
    if (enabled == FALSE && _keyboard_internal__current != NULL_POINTER)
    {
        scene__hide_keyboard();
    }
}

/**
 * @brief scene__virtual_keyboard_enabled
 *
 * @return boole -> void
 */
boole scene__virtual_keyboard_enabled(void)
{
    return _keyboard_internal__is_enabled();
}

/**
 * @brief on-screen virtual keyboard. Builds a <keyboard> node, attaches it as a direct child of the scene root, and lets it draw + handle input at the bottom 40% of the viewport until dismissed. Pairs with scene__hide_keyboard(). Intended for touch-only targets. On Android the platform layer can auto-show the keyboard when a text-editing widget takes focus if scene__set_virtual_keyboard_enabled was called with TRUE. Windows is opt-in: the host must explicitly call these helpers, or flip the enabled flag on for hardware-keyboard-less setups.
 *
 * @return void
 */
void scene__show_keyboard(void)
{
    if (_keyboard_internal__current != NULL_POINTER)
    {
        return;
    }
    gui_node_t* root = scene__root();
    if (root == NULL_POINTER)
    {
        log_warn("%s", "scene__show_keyboard: no scene root");
        return;
    }
    gui_node_t* kb = scene__node_new(GUI_NODE_TYPE__KEYBOARD);
    if (kb == NULL_POINTER)
    {
        return;
    }
    // Tag with a class so hosts can target the keyboard from CSS
    // (z-index, position, etc.). meek-shell uses .meek-keyboard
    // to lift the keyboard above the folder-view overlay so the
    // keys don't get caught in the overlay's backdrop blur.
    {
        const char* cls = "meek-keyboard";
        int i = 0;
        while (cls[i] != 0 && i + 1 < (int)sizeof(kb->klass))
        {
            kb->klass[i] = cls[i];
            i++;
        }
        kb->klass[i] = 0;
    }
    scene__add_child(root, kb);
    _keyboard_internal__current = kb;
}

/**
 * @brief scene__hide_keyboard
 *
 * @return void
 */
void scene__hide_keyboard(void)
{
    gui_node_t* kb = _keyboard_internal__current;
    _keyboard_internal__current = NULL_POINTER;
    if (kb == NULL_POINTER)
    {
        return;
    }
    // Detach from its parent so the scene walk stops visiting it,
    // then free. Guard against concurrent tree mutations by walking
    // from the parent we recorded at show time.
    gui_node_t* parent = kb->parent;
    if (parent != NULL_POINTER)
    {
        gui_node_t** pp = &parent->first_child;
        while (*pp != NULL_POINTER && *pp != kb)
        {
            pp = &(*pp)->next_sibling;
        }
        if (*pp == kb)
        {
            *pp = kb->next_sibling;
            if (parent->last_child == kb)
            {
                gui_node_t* tail = parent->first_child;
                while (tail != NULL_POINTER && tail->next_sibling != NULL_POINTER)
                {
                    tail = tail->next_sibling;
                }
                parent->last_child = tail;
            }
            parent->child_count--;
        }
    }
    kb->parent       = NULL_POINTER;
    kb->next_sibling = NULL_POINTER;
    scene__node_free(kb);
}

/**
 * @brief is the on-screen keyboard currently shown? Returns TRUE when a GUI_NODE_TYPE__KEYBOARD node is attached to the scene (i.e. between scene__show_keyboard() and scene__hide_keyboard()). Useful for host UI that wants to toggle the keyboard on a button tap.
 *
 * @return boole -> void
 */
boole scene__keyboard_visible(void)
{
    return (_keyboard_internal__current != NULL_POINTER) ? TRUE : FALSE;
}
