// ===========================================================================
// parser_style.c - CSS-shaped parser for the toolkit's .style files.
// ===========================================================================
// WHAT THIS PARSER IS
// -------------------
// A hand-written recursive-descent parser for the CSS-like syntax described
// in session/DESIGN.md. Input is a text file; output is a sequence of
// scene__register_style(selector, &style) calls -- one per rule found.
// scene.c's selector table then applies those rules to every matching
// node during scene__resolve_styles.
// GRAMMAR (the subset we currently accept):
// stylesheet  := rule*
// rule        := selector '{' block '}'
// block       := ( decl | nested_rule )*
// nested_rule := ':' ident '{' block '}'       // pseudo-state nesting
// decl        := ident ':' value ';'
// value       := color | number | size_2d | keyword
// color       := '#' hex{6}
// number      := [0-9]+ ('.' [0-9]+)?
// size_2d     := number 'x' number             // e.g. 200x40
// keyword     := ident                         // e.g. auto, scroll
// Example .style file:
// Window       { bg: #1e1e1e; font_family: Roboto-Regular; font_size: 14; }
// #main        { pad: 24; gap: 8; }
// Button       { bg: #333; radius: 6; pad: 8; :hover { bg: #555; } }
// Button.primary { bg: #0066cc; }
// CSS (Cascading Style Sheets) subset:
// Base declarations are parsed here; motion.c preprocesses extended
// properties, media queries and keyframes; style_resources.c loads inline assets and fonts.
// Descendant selectors (space), compound classes and selector lists
// are supported. Child/sibling combinators (>, +, ~) are not.
// See docs/style-properties.md for the supported properties and units.
// PROPERTIES SUPPORTED (list lives here; add to match gui_style_t):
// bg                <color>         background color
// fg                <color>         foreground / accent (slider thumb etc.)
// color             <color>         text color
// font_family       <ident|string>  font name (matches .ttf filename)
// font_size         <number>        point size
// radius            <number>        corner radius
// pad               <number>        padding (all four sides)
// gap               <number>        spacing between children
// size              <w>x<h>         explicit size (0 = auto on either axis)
// overflow          <keyword>       visible|hidden|scroll|auto (both axes)
// overflow_x        <keyword>       same, horizontal axis only
// overflow_y        <keyword>       same, vertical axis only
// scrollbar_size    <number>        scrollbar thickness in px
// scrollbar_radius  <number>        scrollbar corner radius
// scrollbar_track   <color>         scrollbar channel color
// scrollbar_thumb   <color>         scrollbar draggable thumb color
// ERROR STRATEGY
// --------------
// Same as parser_xml.c: a latched `ok` flag on the lexer. First error
// logs "filename:line: message" and sets ok = FALSE; helpers short
// circuit. At end of parse, ok is the return value of load_styles().
// Unlike parser_xml, we DON'T blow away the partial style set on
// error -- any rules successfully registered before the error are
// already committed. This is fine for hot reload because the live app
// does a scene__clear_styles() before each reparse.
// I/O
// ---
// fs__read_entire_file slurps the file. parser_style never sees a
// FILE*. This means the same parser happily runs against memory-mapped
// files, embedded byte arrays, or test fixtures.

// NOTE: <string.h> deliberately NOT included. strcmp / memcpy / memset
// have all been migrated to the project-local `stdlib__` wrappers in
// clib/stdlib.h below. If you re-introduce a libc string call here,
// migrate it to stdlib__* in the same patch.

#include "types.h"
#include "gui.h"
#include "gradient.h"
#include "motion.h"
#include "color_mix.h"
#include "scene.h"
#include "fs.h"
#include "parser_style.h"
#include "style_resources.h"
#include "clib/memory_manager.h"
#include "clib/stdlib.h"
#include "third_party/log.h"

/**
 * Lexer state for one parse run.
 *
 * Mirrors parser_xml.c's lexer -- same fields, same conventions. One of
 * these is built on the stack at the top of parser_style__load_styles
 * and threaded through every helper.
 */
typedef struct _parser_style_internal__lexer_t
{
    // input buffer (null-terminated copy of the file).
    char* src;        // 0x0
    // length in bytes (excluding the null terminator).
    int64 len;        // 0x8
    // current parse position (0..len).
    int64 pos;        // 0x10
    // current line number for error messages (1-based).
    int64 line;       // 0x18
    // path echoed in error messages; not owned.
    char* filename;   // 0x20
    // latched FALSE once any error has been reported.
    boole ok;         // 0x28
    boole quiet;      // 0x29 value probes return FALSE without logging
} _parser_style_internal__lexer_t;

// ===========================================================================
// Forward declarations.
// ===========================================================================
// Every helper is file-local. Public API is just parser_style__load_styles.
static void  _parser_style_internal__error(_parser_style_internal__lexer_t* lx, char* msg);
static char  _parser_style_internal__peek(_parser_style_internal__lexer_t* lx);
static char  _parser_style_internal__advance(_parser_style_internal__lexer_t* lx);
static boole _parser_style_internal__eof(_parser_style_internal__lexer_t* lx);
static boole _parser_style_internal__decl_is_important(_parser_style_internal__lexer_t* lx);
static void  _parser_style_internal__skip_ws_and_comments(_parser_style_internal__lexer_t* lx);
static boole _parser_style_internal__is_alpha(char c);
static boole _parser_style_internal__is_alnum(char c);
static boole _parser_style_internal__is_digit(char c);
static boole _parser_style_internal__parse_ident(_parser_style_internal__lexer_t* lx, char* out, int64 cap);
static boole _parser_style_internal__parse_selector(_parser_style_internal__lexer_t* lx, char* out, int64 cap);
static boole _parser_style_internal__parse_color(_parser_style_internal__lexer_t* lx, gui_color_t* out);
static boole _parser_style_internal__parse_color_mix(_parser_style_internal__lexer_t* lx, gui_color_t* out);
static boole _parser_style_internal__named_color(const char* name, gui_color_t* out);
static int64 _parser_style_internal__peek_ident(_parser_style_internal__lexer_t* lx, char* out, int64 cap);
static void  _parser_style_internal__advance_n(_parser_style_internal__lexer_t* lx, int64 n);
static boole _parser_style_internal__parse_rgb_fn(_parser_style_internal__lexer_t* lx, gui_color_t* out);
static boole _parser_style_internal__parse_hsl_fn(_parser_style_internal__lexer_t* lx, gui_color_t* out);
static boole _parser_style_internal__is_gradient_fn(const char* ident);
static boole _parser_style_internal__parse_angle(_parser_style_internal__lexer_t* lx, float* degrees, boole percent);
static boole _parser_style_internal__parse_stop_position(_parser_style_internal__lexer_t* lx, boole conic, float* value, boole* percent, float* pixel_offset);
static boole _parser_style_internal__gradient_dir_keyword(const char* ident, gui_gradient_dir_e* out);
static boole _parser_style_internal__parse_gradient_position(_parser_style_internal__lexer_t* lx, gui_gradient_t* out);
static boole _parser_style_internal__parse_gradient_fn(_parser_style_internal__lexer_t* lx, const char* fn, gui_gradient_t* out);
static boole _parser_style_internal__parse_legacy_gradient_fn(_parser_style_internal__lexer_t* lx, gui_gradient_t* out);
static boole _parser_style_internal__parse_url_path(_parser_style_internal__lexer_t* lx, char* buf, int64 cap);
static boole _parser_style_internal__parse_path_value(_parser_style_internal__lexer_t* lx, char* buf, int64 cap);
static boole _parser_style_internal__parse_number(_parser_style_internal__lexer_t* lx, float* out);
static boole _parser_style_internal__parse_size(_parser_style_internal__lexer_t* lx, float* out_w, float* out_h);
static boole _parser_style_internal__parse_box(_parser_style_internal__lexer_t* lx, float* out_t, float* out_r, float* out_b, float* out_l);
static boole _parser_style_internal__parse_align(_parser_style_internal__lexer_t* lx, boole distributions, int* out);
static boole _parser_style_internal__parse_length(_parser_style_internal__lexer_t* lx, float* out_v, boole* out_is_percent);
static boole _parser_style_internal__parse_easing(_parser_style_internal__lexer_t* lx, gui_easing_e* out_kind, float out_params[4]);
static boole _parser_style_internal__parse_checkbox_part(_parser_style_internal__lexer_t* lx, const char* property, gui_checkbox_part_t* part);
static boole _parser_style_internal__parse_decl(_parser_style_internal__lexer_t* lx, gui_style_t* style);
static void  _parser_style_internal__parse_block(_parser_style_internal__lexer_t* lx, char* base_selector);
static void  _parser_style_internal__parse_rule(_parser_style_internal__lexer_t* lx);

// ===========================================================================
// Public API.
// ===========================================================================

// The only entry point. Reads the file, parses every rule, and calls
// scene__register_style for each. Returns TRUE on a clean parse,
// FALSE on I/O failure or any parse error.
/**
 * @brief read the file at the given path (UTF-8) and parse its CSS-shaped style rules. Each parsed rule becomes a scene__register_style call.
 *
 * @param char* path -> Path to the .style file to read.
 *
 * @return boole -> TRUE on success; FALSE on I/O or parse failure. Errors are logged to stderr. Caller is responsible for clearing the style table beforehand if a fresh-state reload is desired (scene__clear_styles).
 */
boole parser_style__load_styles(char* path)
{
    int64 size = 0;
    char* bytes = NULL_POINTER;
    boole result = FALSE;

    bytes = fs__read_entire_file(path, &size);
    if (bytes == NULL_POINTER)
    {
        log_error("%s %s", "failed to read stylesheet", path);
        return FALSE;
    }
    result = parser_style__load_styles_from_memory(bytes, size, path);
    GUI_FREE(bytes);
    return result;
}

/**
 * @brief parses a .style document from an in-memory byte buffer
 *
 * @param const char* bytes -> UTF-8 .style document bytes; the buffer does not need to be null-terminated, exactly len bytes are read
 * @param int64 len -> byte count of the buffer
 * @param char* source_label -> optional filename-like tag echoed in error logs; pass NULL_POINTER to substitute "(memory)"
 *
 * @attention used by hosts that embed the .style as a string literal so no file path exists; skips fs__read_entire_file, otherwise the lexer-then-parse pipeline is identical to parser_style__load_styles
 *
 * @return boole -> TRUE on success, FALSE on parse failure
 */
boole parser_style__load_styles_from_memory(const char* bytes, int64 len, char* source_label)
{
    _parser_style_internal__lexer_t lx;
    char* filtered = NULL_POINTER;
    char* resources = NULL_POINTER;

    if (bytes == NULL_POINTER || len <= 0)
    {
        log_error("%s", "parser_style__load_styles_from_memory: null or empty buffer");
        return FALSE;
    }

    if (!style_resources__prepare(bytes, len, source_label, &resources, &len)) { return FALSE; }
    if (resources != NULL_POINTER) { bytes = resources; }
    filtered = motion__prepare_styles(bytes, len);
    lx.src      = (filtered != NULL_POINTER) ? filtered : (char*)bytes;
    lx.len      = len;
    lx.pos      = 0;
    lx.line     = 1;
    lx.filename = source_label;
    lx.ok       = TRUE;
    lx.quiet    = FALSE;

    if (lx.filename == NULL_POINTER)
    {
        lx.filename = (char*)"(memory)";
    }

    while (lx.ok == TRUE)
    {
        _parser_style_internal__skip_ws_and_comments(&lx);

        if (_parser_style_internal__eof(&lx) == TRUE)
        {
            break;
        }

        _parser_style_internal__parse_rule(&lx);
    }

    GUI_FREE(filtered);
    GUI_FREE(resources);
    return lx.ok;
}

// ===========================================================================
// Lexer primitives -- character-at-a-time helpers.
// ===========================================================================

// Log "filename:line: message" and latch ok=FALSE. Every downstream
// helper bails when ok is FALSE (through a boolean return).
static void _parser_style_internal__error(_parser_style_internal__lexer_t* lx, char* msg)
{
    if (lx->quiet == FALSE)
    {
        log_error("%s %s %lld %s %s", lx->filename, ":", (long long)lx->line, ":", msg);
    }
    lx->ok = FALSE;
}

// Look at the current character without consuming. Returns 0 at EOF
// so callers can treat EOF and NUL uniformly.
// Whether the declaration that starts at the current position ends in
// `!important` before its ';'. A lookahead that consumes nothing; the
// block parser uses it to pick the rule the declaration belongs to,
// and parse_decl swallows the token itself.
static boole _parser_style_internal__decl_is_important(_parser_style_internal__lexer_t* lx)
{
    int64 i   = 0;
    int64 k   = 0;
    char  c   = 0;

    for (i = lx->pos; i < lx->len; i++)
    {
        c = lx->src[i];
        if (c == ';' || c == '}')
        {
            break;
        }
    }

    if (i >= lx->len || lx->src[i] != ';')
    {
        return FALSE;
    }

    k = i - 1;

    while (k >= lx->pos && (lx->src[k] == ' ' || lx->src[k] == '\t' || lx->src[k] == '\r' || lx->src[k] == '\n'))
    {
        k--;
    }

    // "important" is nine characters, preceded by optional space and '!'
    if (k - 8 < lx->pos || stdlib__strncmp(lx->src + k - 8, "important", 9) != 0)
    {
        return FALSE;
    }

    k = k - 9;

    while (k >= lx->pos && (lx->src[k] == ' ' || lx->src[k] == '\t' || lx->src[k] == '\r' || lx->src[k] == '\n'))
    {
        k--;
    }

    return (boole)(k >= lx->pos && lx->src[k] == '!');
}

static char _parser_style_internal__peek(_parser_style_internal__lexer_t* lx)
{
    if (lx->pos >= lx->len)
    {
        return 0;
    }
    return lx->src[lx->pos];
}

// Consume one character, return it, bump line counter on '\n'.
static char _parser_style_internal__advance(_parser_style_internal__lexer_t* lx)
{
    if (lx->pos >= lx->len)
    {
        return 0;
    }
    char c = lx->src[lx->pos++];
    if (c == '\n')
    {
        lx->line++;
    }
    return c;
}

// EOF test. Used as a loop guard in every scanning function.
static boole _parser_style_internal__eof(_parser_style_internal__lexer_t* lx)
{
    return (boole)(lx->pos >= lx->len);
}

// Skip ASCII whitespace and C-style slash-star comments. CSS uses
// the same slash-star comment syntax, so the same scanner works here.
// We do NOT accept // line comments -- real CSS doesn't either.
// The outer for(;;) loop handles the case where a comment is
// immediately followed by more whitespace, which could be followed
// by another comment, etc. We keep consuming until we hit a non-
// whitespace, non-comment character.
static void _parser_style_internal__skip_ws_and_comments(_parser_style_internal__lexer_t* lx)
{
    for (;;)
    {
        // Inner loop: consume plain whitespace.
        while (_parser_style_internal__eof(lx) == FALSE)
        {
            char c = _parser_style_internal__peek(lx);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                _parser_style_internal__advance(lx);
            }
            else
            {
                break;
            }
        }

        // Is there a slash-star here? If so, consume through star-slash and loop
        // back to the whitespace scanner. If not, we're done.
        if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '/' && lx->src[lx->pos + 1] == '*')
        {
            lx->pos += 2;
            boole closed = FALSE;
            while (lx->pos + 1 < lx->len)
            {
                if (lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')
                {
                    lx->pos += 2;
                    closed = TRUE;
                    break;
                }
                _parser_style_internal__advance(lx);
            }
            if (closed == FALSE)
            {
                // Hit EOF without finding `*/`. Without this branch
                // the .style file would silently load with everything
                // after the unclosed comment dropped on the floor and
                // no warning to the dev.
                _parser_style_internal__error(lx, "unterminated /* */ comment");
                return;
            }
            continue;
        }
        break;
    }
}

// Character classes. We don't pull in <ctype.h> because:
// - <ctype.h> is locale-aware, which slows things down and makes
// the build non-portable between musl/glibc/MSVCRT.
// - We only need ASCII anyway (selectors and property names are
// chosen by the engine, not by users).
static boole _parser_style_internal__is_alpha(char c)
{
    return (boole)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}
static boole _parser_style_internal__is_alnum(char c)
{
    // Note '-' is included here -- CSS identifiers allow hyphens
    // (e.g. `font-family`). Our property names actually use underscores
    // (`font_family`), but we accept hyphens too so that a future
    // conversion to CSS-canonical names doesn't break existing files.
    return (boole)(_parser_style_internal__is_alpha(c) || (c >= '0' && c <= '9') || c == '-');
}
static boole _parser_style_internal__is_digit(char c)
{
    return (boole)(c >= '0' && c <= '9');
}

// Read a plain identifier (first char alpha/'_', rest alnum/'_'/'-').
// Used for property names, keyword values (auto/scroll/visible/...),
// and pseudo-state names (hover/pressed/disabled/default).
// Returns FALSE on empty input or overflow, logs the error, latches ok.
static boole _parser_style_internal__parse_ident(_parser_style_internal__lexer_t* lx, char* out, int64 cap)
{
    int64 n = 0;
    boole first = TRUE;
    while (_parser_style_internal__eof(lx) == FALSE)
    {
        char c = _parser_style_internal__peek(lx);
        if (first)
        {
            if (_parser_style_internal__is_alpha(c) == FALSE)
            {
                break;
            }
            first = FALSE;
        }
        else if (_parser_style_internal__is_alnum(c) == FALSE)
        {
            break;
        }
        if (n + 1 >= cap)
        {
            _parser_style_internal__error(lx, "identifier too long");
            return FALSE;
        }
        // normalize `_` to `-` when building the ident. property-name
        // dispatch below uses the CSS-canonical kebab-case spellings
        // (font-family, scroll-smooth, ease-in-out, ...). existing
        // .style files written with snake_case (font_family, ...)
        // still work because the normalization happens before strcmp.
        // affects only IDENTIFIERS read via this function -- property
        // names and keyword values (easing names, visibility / display
        // keywords). font-family VALUES (Roboto-Regular, etc.) are
        // read via a separate bespoke value parser and are unaffected.
        char stored = (c == '_') ? '-' : c;
        out[n++] = stored;
        _parser_style_internal__advance(lx);
    }
    if (n == 0)
    {
        _parser_style_internal__error(lx, "expected identifier");
        return FALSE;
    }
    out[n] = 0;
    return TRUE;
}

// Read a selector token: everything up to the opening '{' or a
// selector-boundary character. Letters, digits, '.', '#', '-', '_',
// ':' are all treated as part of the selector, so "Button.primary",
// "#main", "Button:hover" each come back as one string. scene.c's
// _scene_internal__parse_selector then splits the pieces apart and
// decides how to match.
// Preserve spaces for descendant matching. A comma ends this selector;
// parse_rule reads the next entry in the selector list.
static boole _parser_style_internal__parse_selector(_parser_style_internal__lexer_t* lx, char* out, int64 cap)
{
    int64 n = 0;
    while (_parser_style_internal__eof(lx) == FALSE)
    {
        char c = _parser_style_internal__peek(lx);
        if (c == '{' || c == ',' || c == 0)
        {
            break;
        }
        if (n + 1 >= cap)
        {
            _parser_style_internal__error(lx, "selector too long");
            return FALSE;
        }
        out[n++] = c;
        _parser_style_internal__advance(lx);
    }
    while (n > 0 && out[n - 1] <= ' ')
    {
        n--;
    }
    if (n == 0)
    {
        _parser_style_internal__error(lx, "expected selector");
        return FALSE;
    }
    out[n] = 0;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Colors. Accepted: #rgb, #rgba, #rrggbb, #rrggbbaa; rgb() / rgba() with
// channels 0..255 or percent and an alpha after a comma or a slash (0..1
// or percent); hsl() / hsla(); `transparent`; the basic CSS color names
// (black, white, red, ... orange). CSS muscle memory writes rgba() and
// `white`, and a page translated by hand should not have to convert them.
typedef struct _parser_style_internal__named_color_t
{
    const char* name;   // 0x0
    uint        rgb;    // 0x8
} _parser_style_internal__named_color_t;

static const _parser_style_internal__named_color_t _parser_style_internal__named_colors[] =
{
    { "black",   0x000000 },
    { "white",   0xffffff },
    { "red",     0xff0000 },
    { "lime",    0x00ff00 },
    { "blue",    0x0000ff },
    { "yellow",  0xffff00 },
    { "cyan",    0x00ffff },
    { "aqua",    0x00ffff },
    { "magenta", 0xff00ff },
    { "fuchsia", 0xff00ff },
    { "gray",    0x808080 },
    { "grey",    0x808080 },
    { "silver",  0xc0c0c0 },
    { "maroon",  0x800000 },
    { "olive",   0x808000 },
    { "green",   0x008000 },
    { "purple",  0x800080 },
    { "teal",    0x008080 },
    { "navy",    0x000080 },
    { "orange",  0xffa500 },
};

static boole _parser_style_internal__named_color(const char* name, gui_color_t* out)
{
    int64 i = 0;

    for (i = 0; i < (int64)(sizeof(_parser_style_internal__named_colors) / sizeof(_parser_style_internal__named_colors[0])); i++)
    {
        if (stdlib__strcmp(name, _parser_style_internal__named_colors[i].name) == 0)
        {
            *out = scene__hex(_parser_style_internal__named_colors[i].rgb);
            return TRUE;
        }
    }

    return FALSE;
}

// Reads the identifier at the current position into `out` (normalized
// like parse_ident, `_` becomes `-`) without consuming it and without
// reporting anything: a lookahead, the callers decide by the keyword
// whether to consume it. Returns its length, 0 when there is none; a
// longer identifier than `cap` holds is truncated in `out` but its full
// length is returned.
static int64 _parser_style_internal__peek_ident(_parser_style_internal__lexer_t* lx, char* out, int64 cap)
{
    int64 i = lx->pos;
    int64 n = 0;
    char  c = 0;

    out[0] = 0;

    if (i >= lx->len || _parser_style_internal__is_alpha(lx->src[i]) == FALSE)
    {
        return 0;
    }

    while (i < lx->len && _parser_style_internal__is_alnum(lx->src[i]) == TRUE)
    {
        c = (lx->src[i] == '_') ? '-' : lx->src[i];

        if (n + 1 < cap)
        {
            out[n]     = c;
            out[n + 1] = 0;
        }

        n++;
        i++;
    }

    return n;
}

static void _parser_style_internal__advance_n(_parser_style_internal__lexer_t* lx, int64 n)
{
    int64 i = 0;

    for (i = 0; i < n; i++)
    {
        _parser_style_internal__advance(lx);
    }
}

// rgb( r, g, b [, a] ) / rgb( r g b [/ a] ), the `rgb` ident consumed
static boole _parser_style_internal__parse_rgb_fn(_parser_style_internal__lexer_t* lx, gui_color_t* out)
{
    float v[4];
    boole pct = FALSE;
    int   i   = 0;

    stdlib__null_memory(v, sizeof(v));
    v[3] = 1.0f;

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) != '(')
    {
        _parser_style_internal__error(lx, "expected '(' after rgb");
        return FALSE;
    }

    _parser_style_internal__advance(lx);

    for (i = 0; i < 3; i++)
    {
        _parser_style_internal__skip_ws_and_comments(lx);

        if (_parser_style_internal__parse_length(lx, &v[i], &pct) == FALSE)
        {
            return FALSE;
        }

        v[i] = (pct == TRUE) ? v[i] / 100.0f : v[i] / 255.0f;
        _parser_style_internal__skip_ws_and_comments(lx);

        if (_parser_style_internal__peek(lx) == ',')
        {
            _parser_style_internal__advance(lx);
        }
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == '/')
    {
        _parser_style_internal__advance(lx);
        _parser_style_internal__skip_ws_and_comments(lx);
    }

    if (_parser_style_internal__peek(lx) != ')')
    {
        if (_parser_style_internal__parse_length(lx, &v[3], &pct) == FALSE)
        {
            return FALSE;
        }

        if (pct == TRUE)
        {
            v[3] = v[3] / 100.0f;
        }

        _parser_style_internal__skip_ws_and_comments(lx);
    }

    if (_parser_style_internal__peek(lx) != ')')
    {
        _parser_style_internal__error(lx, "expected ')' to close rgb()");
        return FALSE;
    }

    _parser_style_internal__advance(lx);

    for (i = 0; i < 4; i++)
    {
        if (v[i] < 0.0f)
        {
            v[i] = 0.0f;
        }

        if (v[i] > 1.0f)
        {
            v[i] = 1.0f;
        }
    }

    out->r = v[0];
    out->g = v[1];
    out->b = v[2];
    out->a = v[3];
    return TRUE;
}

// hsl( h, s%, l% [, a] ) with the hue in degrees (deg / turn / rad / grad
// accepted), the `hsl` ident consumed
static boole _parser_style_internal__parse_hsl_fn(_parser_style_internal__lexer_t* lx, gui_color_t* out)
{
    char  unit[8];
    float h   = 0.0f;
    float s   = 0.0f;
    float l   = 0.0f;
    float a   = 1.0f;
    float c   = 0.0f;
    float x   = 0.0f;
    float m   = 0.0f;
    float hh  = 0.0f;
    float r1  = 0.0f;
    float g1  = 0.0f;
    float b1  = 0.0f;
    boole pct = FALSE;
    int64 len = 0;

    stdlib__null_memory(unit, sizeof(unit));

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) != '(')
    {
        _parser_style_internal__error(lx, "expected '(' after hsl");
        return FALSE;
    }

    _parser_style_internal__advance(lx);
    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__parse_number(lx, &h) == FALSE)
    {
        return FALSE;
    }

    len = _parser_style_internal__peek_ident(lx, unit, (int64)sizeof(unit));

    if (len > 0)
    {
        if (stdlib__strcmp(unit, "turn") == 0)
        {
            h = h * 360.0f;
        }
        else if (stdlib__strcmp(unit, "rad") == 0)
        {
            h = h * 180.0f / 3.14159265f;
        }
        else if (stdlib__strcmp(unit, "grad") == 0)
        {
            h = h * 0.9f;
        }
        else if (stdlib__strcmp(unit, "deg") != 0)
        {
            _parser_style_internal__error(lx, "hsl(): unknown hue unit");
            return FALSE;
        }

        _parser_style_internal__advance_n(lx, len);
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == ',')
    {
        _parser_style_internal__advance(lx);
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__parse_length(lx, &s, &pct) == FALSE)
    {
        return FALSE;
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == ',')
    {
        _parser_style_internal__advance(lx);
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__parse_length(lx, &l, &pct) == FALSE)
    {
        return FALSE;
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == ',' || _parser_style_internal__peek(lx) == '/')
    {
        _parser_style_internal__advance(lx);
        _parser_style_internal__skip_ws_and_comments(lx);
    }

    if (_parser_style_internal__peek(lx) != ')')
    {
        if (_parser_style_internal__parse_length(lx, &a, &pct) == FALSE)
        {
            return FALSE;
        }

        if (pct == TRUE)
        {
            a = a / 100.0f;
        }

        _parser_style_internal__skip_ws_and_comments(lx);
    }

    if (_parser_style_internal__peek(lx) != ')')
    {
        _parser_style_internal__error(lx, "expected ')' to close hsl()");
        return FALSE;
    }

    _parser_style_internal__advance(lx);

    // saturation and lightness are percentages whether or not the sign is written
    s = s / 100.0f;
    l = l / 100.0f;
    h = stdlib__fmodf(h, 360.0f);

    if (h < 0.0f)
    {
        h = h + 360.0f;
    }

    if (s < 0.0f)
    {
        s = 0.0f;
    }

    if (s > 1.0f)
    {
        s = 1.0f;
    }

    if (l < 0.0f)
    {
        l = 0.0f;
    }

    if (l > 1.0f)
    {
        l = 1.0f;
    }

    if (a < 0.0f)
    {
        a = 0.0f;
    }

    if (a > 1.0f)
    {
        a = 1.0f;
    }

    c  = (1.0f - stdlib__fabsf(2.0f * l - 1.0f)) * s;
    hh = h / 60.0f;
    x  = c * (1.0f - stdlib__fabsf(stdlib__fmodf(hh, 2.0f) - 1.0f));
    m  = l - c * 0.5f;

    if (hh < 1.0f)
    {
        r1 = c;
        g1 = x;
    }
    else if (hh < 2.0f)
    {
        r1 = x;
        g1 = c;
    }
    else if (hh < 3.0f)
    {
        g1 = c;
        b1 = x;
    }
    else if (hh < 4.0f)
    {
        g1 = x;
        b1 = c;
    }
    else if (hh < 5.0f)
    {
        r1 = x;
        b1 = c;
    }
    else
    {
        r1 = c;
        b1 = x;
    }

    out->r = r1 + m;
    out->g = g1 + m;
    out->b = b1 + m;
    out->a = a;
    return TRUE;
}

static boole _parser_style_internal__parse_color(_parser_style_internal__lexer_t* lx, gui_color_t* out)
{
    char         ident[40];
    int64        len = 0;
    int64        n   = 0;
    int          d   = 0;
    char         c   = 0;
    unsigned int v   = 0;
    unsigned int r   = 0;
    unsigned int g   = 0;
    unsigned int b   = 0;
    unsigned int a   = 255;

    stdlib__null_memory(ident, sizeof(ident));
    stdlib__null_memory(out, sizeof(*out));

    c = _parser_style_internal__peek(lx);

    if (_parser_style_internal__is_alpha(c) == TRUE)
    {
        len = _parser_style_internal__peek_ident(lx, ident, (int64)sizeof(ident));

        if (stdlib__strcmp(ident, "color-mix") == 0)
        {
            _parser_style_internal__advance_n(lx, len);
            return _parser_style_internal__parse_color_mix(lx, out);
        }

        if (stdlib__strcmp(ident, "transparent") == 0)
        {
            _parser_style_internal__advance_n(lx, len);
            return TRUE;
        }

        if (stdlib__strcmp(ident, "rgb") == 0 || stdlib__strcmp(ident, "rgba") == 0)
        {
            _parser_style_internal__advance_n(lx, len);
            return _parser_style_internal__parse_rgb_fn(lx, out);
        }

        if (stdlib__strcmp(ident, "hsl") == 0 || stdlib__strcmp(ident, "hsla") == 0)
        {
            _parser_style_internal__advance_n(lx, len);
            return _parser_style_internal__parse_hsl_fn(lx, out);
        }

        if (_parser_style_internal__named_color(ident, out) == TRUE)
        {
            _parser_style_internal__advance_n(lx, len);
            return TRUE;
        }

        _parser_style_internal__error(lx, "expected a color: #rrggbb, #rrggbbaa, rgb(), rgba(), hsl(), transparent or a basic color name");
        return FALSE;
    }

    if (c != '#')
    {
        _parser_style_internal__error(lx, "expected a color: #rrggbb, #rrggbbaa, rgb(), rgba(), hsl(), transparent or a basic color name");
        return FALSE;
    }

    _parser_style_internal__advance(lx);

    // up to 8 hex digits; 3 / 4 digits double each one (#abc = #aabbcc)
    while (n < 8)
    {
        c = _parser_style_internal__peek(lx);

        if (c >= '0' && c <= '9')
        {
            d = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            d = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            d = c - 'A' + 10;
        }
        else
        {
            break;
        }

        v = (v << 4) | (unsigned int)d;
        _parser_style_internal__advance(lx);
        n++;
    }

    if (n == 3 || n == 4)
    {
        r = ((v >> (4 * (n - 1))) & 0xFu) * 17u;
        g = ((v >> (4 * (n - 2))) & 0xFu) * 17u;
        b = ((v >> (4 * (n - 3))) & 0xFu) * 17u;
        a = (n == 4) ? (v & 0xFu) * 17u : 255u;
    }
    else if (n == 6)
    {
        r = (v >> 16) & 0xFFu;
        g = (v >> 8) & 0xFFu;
        b = v & 0xFFu;
    }
    else if (n == 8)
    {
        r = (v >> 24) & 0xFFu;
        g = (v >> 16) & 0xFFu;
        b = (v >> 8) & 0xFFu;
        a = v & 0xFFu;
    }
    else
    {
        _parser_style_internal__error(lx, "expected hex color (#rgb, #rgba, #rrggbb or #rrggbbaa)");
        return FALSE;
    }

    out->r = (float)r / 255.0f;
    out->g = (float)g / 255.0f;
    out->b = (float)b / 255.0f;
    out->a = (float)a / 255.0f;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Gradients: linear-gradient( [ <angle> | to <side-or-corner> ]? , <stops> )
// and radial-gradient( [ [ circle | ellipse ] || <size> ] [ at <position> ]? , <stops> )
// with <size> = closest-side | farthest-side | closest-corner | farthest-corner
// | <length> | <length-percentage>{2}, a stop = <color> [<position> [<position>]].
// Angles take deg (also bare), grad, rad, turn. Positions are percent or
// pixels. Color hints (a bare position between two colors) are skipped
// with a warning; repeating-* and conic gradients are not accepted.
static boole _parser_style_internal__is_gradient_fn(const char* ident)
{
    const char* name = ident;

    if (stdlib__strncmp(name, "repeating-", 10) == 0)
    {
        name += 10;
    }
    return (boole)(stdlib__strcmp(name, "linear-gradient") == 0 || stdlib__strcmp(name, "radial-gradient") == 0 || stdlib__strcmp(name, "conic-gradient") == 0);
}

/**
 * @brief parse a CSS angle, optionally accepting a percentage of one turn
 *
 * @param _parser_style_internal__lexer_t* lx -> token stream
 * @param float* degrees -> resulting clockwise angle
 * @param boole percent -> accept percentages for conic color stops
 * @return boole -> TRUE for a valid angle
 */
static boole _parser_style_internal__parse_angle(_parser_style_internal__lexer_t* lx, float* degrees, boole percent)
{
    char unit[16];
    int64 len = 0;

    stdlib__null_memory(unit, sizeof(unit));
    if (_parser_style_internal__parse_number(lx, degrees) == FALSE)
    {
        return FALSE;
    }
    if (percent == TRUE && _parser_style_internal__peek(lx) == '%')
    {
        *degrees *= 3.6f;
        _parser_style_internal__advance(lx);
        return TRUE;
    }
    len = _parser_style_internal__peek_ident(lx, unit, sizeof(unit));
    if (len == 0 && *degrees == 0.0f)
    {
        return TRUE;
    }
    if (stdlib__strcmp(unit, "turn") == 0)
    {
        *degrees *= 360.0f;
    }
    else if (stdlib__strcmp(unit, "rad") == 0)
    {
        *degrees *= 180.0f / 3.14159265358979f;
    }
    else if (stdlib__strcmp(unit, "grad") == 0)
    {
        *degrees *= 0.9f;
    }
    else if (stdlib__strcmp(unit, "deg") != 0)
    {
        _parser_style_internal__error(lx, "expected deg, grad, rad, turn, or a permitted percentage");
        return FALSE;
    }
    _parser_style_internal__advance_n(lx, len);
    return TRUE;
}

/**
 * @brief normalize conic stops to percentages, keeping linear and radial lengths
 *
 * @param _parser_style_internal__lexer_t* lx -> token stream
 * @param boole conic -> angle-based stop positions
 * @param float* value -> percentage or logical pixel length
 * @param boole* percent -> TRUE for a percentage
 * @return boole -> TRUE for a valid position
 */
static boole _parser_style_internal__parse_stop_position(_parser_style_internal__lexer_t* lx, boole conic, float* value, boole* percent, float* pixel_offset)
{
    char expression[256];
    int64 start = lx->pos;
    int64 length = 0;
    int depth = 0;
    char c = 0;

    stdlib__null_memory(expression, sizeof(expression));
    *pixel_offset = 0.0f;
    if (conic == FALSE && stdlib__strncmp(lx->src + start, "calc(", 5) == 0)
    {
        do
        {
            c = _parser_style_internal__peek(lx);
            if (c == '(')
            {
                depth++;
            }
            else if (c == ')')
            {
                depth--;
            }
            _parser_style_internal__advance(lx);
        } while (lx->pos < lx->len && (lx->pos < start + 5 || depth > 0));
        length = lx->pos - start;
        if (depth != 0 || length >= (int64)sizeof(expression))
        {
            return FALSE;
        }
        stdlib__memcpy(expression, lx->src + start, length);
        *pixel_offset = motion__number(expression, 0.0f);
        *value = motion__number(expression, 100.0f) - *pixel_offset;
        *percent = TRUE;
        return TRUE;
    }
    if (conic == FALSE)
    {
        return _parser_style_internal__parse_length(lx, value, percent);
    }
    if (_parser_style_internal__parse_angle(lx, value, TRUE) == FALSE)
    {
        return FALSE;
    }
    *value /= 3.6f;
    *percent = TRUE;
    return TRUE;
}

static boole _parser_style_internal__gradient_dir_keyword(const char* ident, gui_gradient_dir_e* out)
{
    if (stdlib__strcmp(ident, "vertical") == 0)
    {
        *out = GUI_GRADIENT_DIR__VERTICAL;
        return TRUE;
    }

    if (stdlib__strcmp(ident, "horizontal") == 0)
    {
        *out = GUI_GRADIENT_DIR__HORIZONTAL;
        return TRUE;
    }

    if (stdlib__strcmp(ident, "diagonal-tl") == 0)
    {
        *out = GUI_GRADIENT_DIR__DIAGONAL_TL;
        return TRUE;
    }

    if (stdlib__strcmp(ident, "diagonal-tr") == 0)
    {
        *out = GUI_GRADIENT_DIR__DIAGONAL_TR;
        return TRUE;
    }

    return FALSE;
}

// the <position> after `at`: up to two keywords (left / center / right,
// top / center / bottom) or lengths; a keyword names its axis, a length
// fills the next free one (x first)
static boole _parser_style_internal__parse_gradient_position(_parser_style_internal__lexer_t* lx, gui_gradient_t* out)
{
    char  ident[16];
    int64 len   = 0;
    int   i     = 0;
    int   axis  = 0;
    int   ax    = 0;
    float value = 0.0f;
    float v     = 0.0f;
    boole pct   = FALSE;
    char  c     = 0;

    stdlib__null_memory(ident, sizeof(ident));

    for (i = 0; i < 2; i++)
    {
        _parser_style_internal__skip_ws_and_comments(lx);
        c   = _parser_style_internal__peek(lx);
        len = _parser_style_internal__peek_ident(lx, ident, (int64)sizeof(ident));

        if (len > 0)
        {
            if (stdlib__strcmp(ident, "left") == 0 || stdlib__strcmp(ident, "top") == 0)
            {
                value = 0.0f;
            }
            else if (stdlib__strcmp(ident, "center") == 0)
            {
                value = 0.5f;
            }
            else if (stdlib__strcmp(ident, "right") == 0 || stdlib__strcmp(ident, "bottom") == 0)
            {
                value = 1.0f;
            }
            else
            {
                break;
            }

            if (stdlib__strcmp(ident, "top") == 0 || stdlib__strcmp(ident, "bottom") == 0)
            {
                ax = 1;
            }
            else if (stdlib__strcmp(ident, "left") == 0 || stdlib__strcmp(ident, "right") == 0)
            {
                ax = 0;
            }
            else
            {
                ax = axis;
            }

            if (ax == 0)
            {
                out->center_x    = value;
                out->center_x_px = FALSE;
            }
            else
            {
                out->center_y    = value;
                out->center_y_px = FALSE;
            }

            axis = (ax == 1 && i == 0) ? 0 : ax + 1;
            _parser_style_internal__advance_n(lx, len);
            continue;
        }

        if (_parser_style_internal__is_digit(c) == TRUE || c == '.' || c == '-')
        {
            if (_parser_style_internal__parse_length(lx, &v, &pct) == FALSE)
            {
                return FALSE;
            }

            if (axis == 0)
            {
                out->center_x    = (pct == TRUE) ? v / 100.0f : v;
                out->center_x_px = (boole)(pct == FALSE);
            }
            else
            {
                out->center_y    = (pct == TRUE) ? v / 100.0f : v;
                out->center_y_px = (boole)(pct == FALSE);
            }

            axis++;
            continue;
        }

        break;
    }

    return TRUE;
}

// the function's ident is consumed, the '(' is next
static boole _parser_style_internal__parse_gradient_fn(_parser_style_internal__lexer_t* lx, const char* fn, gui_gradient_t* out)
{
    char                 ident[40];
    float                v           = 0.0f;
    float                v2          = 0.0f;
    float                pixel_offset = 0.0f;
    boole                pct         = FALSE;
    boole                pct2        = FALSE;
    boole                have_clause = FALSE;
    boole                radial      = FALSE;
    boole                conic       = FALSE;
    const char*          name        = fn;
    int64                len         = 0;
    char                 c           = 0;
    gui_gradient_stop_t* st          = NULL_POINTER;

    stdlib__null_memory(ident, sizeof(ident));
    stdlib__null_memory(out, sizeof(*out));

    out->repeating = (boole)(stdlib__strncmp(name, "repeating-", 10) == 0);
    if (out->repeating == TRUE)
    {
        name += 10;
    }
    radial         = (boole)(stdlib__strcmp(name, "radial-gradient") == 0);
    conic          = (boole)(stdlib__strcmp(name, "conic-gradient") == 0);
    out->kind      = (radial == TRUE) ? GUI_GRADIENT_KIND__RADIAL : GUI_GRADIENT_KIND__LINEAR;
    out->angle_deg = 180.0f;
    out->size      = GUI_GRADIENT_SIZE__FARTHEST_CORNER;
    out->center_x  = 0.5f;
    out->center_y  = 0.5f;

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) != '(')
    {
        _parser_style_internal__error(lx, "expected '(' after the gradient function");
        return FALSE;
    }

    _parser_style_internal__advance(lx);
    _parser_style_internal__skip_ws_and_comments(lx);

    if (conic == TRUE)
    {
        out->kind = GUI_GRADIENT_KIND__CONIC;
        out->angle_deg = 0.0f;
        len = _parser_style_internal__peek_ident(lx, ident, sizeof(ident));
        if (len > 0 && stdlib__strcmp(ident, "from") == 0)
        {
            _parser_style_internal__advance_n(lx, len);
            _parser_style_internal__skip_ws_and_comments(lx);
            if (_parser_style_internal__parse_angle(lx, &out->angle_deg, FALSE) == FALSE)
            {
                return FALSE;
            }
            have_clause = TRUE;
            _parser_style_internal__skip_ws_and_comments(lx);
            len = _parser_style_internal__peek_ident(lx, ident, sizeof(ident));
        }
        if (len > 0 && stdlib__strcmp(ident, "at") == 0)
        {
            _parser_style_internal__advance_n(lx, len);
            if (_parser_style_internal__parse_gradient_position(lx, out) == FALSE)
            {
                return FALSE;
            }
            have_clause = TRUE;
        }
    }
    else if (radial == FALSE)
    {
        c   = _parser_style_internal__peek(lx);
        len = _parser_style_internal__peek_ident(lx, ident, (int64)sizeof(ident));

        if (_parser_style_internal__is_digit(c) == TRUE || c == '-' || c == '.')
        {
            if (_parser_style_internal__parse_number(lx, &v) == FALSE)
            {
                return FALSE;
            }

            len = _parser_style_internal__peek_ident(lx, ident, (int64)sizeof(ident));

            if (len > 0)
            {
                if (stdlib__strcmp(ident, "grad") == 0)
                {
                    v = v * 0.9f;
                }
                else if (stdlib__strcmp(ident, "rad") == 0)
                {
                    v = v * 180.0f / 3.14159265f;
                }
                else if (stdlib__strcmp(ident, "turn") == 0)
                {
                    v = v * 360.0f;
                }
                else if (stdlib__strcmp(ident, "deg") != 0)
                {
                    _parser_style_internal__error(lx, "linear-gradient: unknown angle unit (deg, grad, rad, turn)");
                    return FALSE;
                }

                _parser_style_internal__advance_n(lx, len);
            }

            out->angle_deg = v;
            have_clause    = TRUE;
        }
        else if (len > 0 && stdlib__strcmp(ident, "to") == 0)
        {
            _parser_style_internal__advance_n(lx, len);

            while (TRUE)
            {
                _parser_style_internal__skip_ws_and_comments(lx);
                len = _parser_style_internal__peek_ident(lx, ident, (int64)sizeof(ident));

                if (len <= 0)
                {
                    break;
                }

                if (stdlib__strcmp(ident, "left") == 0)
                {
                    out->corner_x = -1;
                }
                else if (stdlib__strcmp(ident, "right") == 0)
                {
                    out->corner_x = 1;
                }
                else if (stdlib__strcmp(ident, "top") == 0)
                {
                    out->corner_y = -1;
                }
                else if (stdlib__strcmp(ident, "bottom") == 0)
                {
                    out->corner_y = 1;
                }
                else
                {
                    _parser_style_internal__error(lx, "linear-gradient: expected top, bottom, left or right after 'to'");
                    return FALSE;
                }

                _parser_style_internal__advance_n(lx, len);
            }

            if (out->corner_x == 0 && out->corner_y == 0)
            {
                _parser_style_internal__error(lx, "linear-gradient: expected a side or corner after 'to'");
                return FALSE;
            }

            have_clause = TRUE;
        }
    }
    else
    {
        while (TRUE)
        {
            _parser_style_internal__skip_ws_and_comments(lx);
            c   = _parser_style_internal__peek(lx);
            len = _parser_style_internal__peek_ident(lx, ident, (int64)sizeof(ident));

            if (len > 0)
            {
                if (stdlib__strcmp(ident, "circle") == 0)
                {
                    out->circle = TRUE;
                }
                else if (stdlib__strcmp(ident, "ellipse") == 0)
                {
                    out->circle = FALSE;
                }
                else if (stdlib__strcmp(ident, "closest-side") == 0)
                {
                    out->size = GUI_GRADIENT_SIZE__CLOSEST_SIDE;
                }
                else if (stdlib__strcmp(ident, "farthest-side") == 0)
                {
                    out->size = GUI_GRADIENT_SIZE__FARTHEST_SIDE;
                }
                else if (stdlib__strcmp(ident, "closest-corner") == 0)
                {
                    out->size = GUI_GRADIENT_SIZE__CLOSEST_CORNER;
                }
                else if (stdlib__strcmp(ident, "farthest-corner") == 0)
                {
                    out->size = GUI_GRADIENT_SIZE__FARTHEST_CORNER;
                }
                else if (stdlib__strcmp(ident, "at") == 0)
                {
                    _parser_style_internal__advance_n(lx, len);

                    if (_parser_style_internal__parse_gradient_position(lx, out) == FALSE)
                    {
                        return FALSE;
                    }

                    have_clause = TRUE;
                    continue;
                }
                else
                {
                    // a color: the stops start here
                    break;
                }

                _parser_style_internal__advance_n(lx, len);
                have_clause = TRUE;
                continue;
            }

            if (_parser_style_internal__is_digit(c) == TRUE || c == '.')
            {
                // an explicit size: one length is a circle's radius, two are an ellipse's radii
                if (_parser_style_internal__parse_length(lx, &v, &pct) == FALSE)
                {
                    return FALSE;
                }

                out->radius_x   = v;
                out->radius_y   = v;
                out->radius_pct = pct;
                out->size       = GUI_GRADIENT_SIZE__EXPLICIT;
                out->circle     = TRUE;
                _parser_style_internal__skip_ws_and_comments(lx);
                c = _parser_style_internal__peek(lx);

                if (_parser_style_internal__is_digit(c) == TRUE || c == '.')
                {
                    if (_parser_style_internal__parse_length(lx, &v2, &pct2) == FALSE)
                    {
                        return FALSE;
                    }

                    if (pct2 != pct)
                    {
                        _parser_style_internal__error(lx, "radial-gradient: the two radii must both be pixels or both percent");
                        return FALSE;
                    }

                    out->radius_y = v2;
                    out->circle   = FALSE;
                }

                have_clause = TRUE;
                continue;
            }

            break;
        }
    }

    if (have_clause == TRUE)
    {
        _parser_style_internal__skip_ws_and_comments(lx);

        if (_parser_style_internal__peek(lx) != ',')
        {
            _parser_style_internal__error(lx, "expected ',' between the gradient's direction and its colors");
            return FALSE;
        }

        _parser_style_internal__advance(lx);
    }

    // the color stops
    while (TRUE)
    {
        _parser_style_internal__skip_ws_and_comments(lx);
        c = _parser_style_internal__peek(lx);

        if (_parser_style_internal__is_digit(c) == TRUE || c == '.' || c == '-' || stdlib__strncmp(lx->src + lx->pos, "calc(", 5) == 0)
        {
            // a bare position between two colors is a CSS color hint (the
            // midpoint of that transition); not supported, skipped
            if (_parser_style_internal__parse_stop_position(lx, conic, &v, &pct, &pixel_offset) == FALSE)
            {
                return FALSE;
            }

            log_warn("%s %s %lld %s", lx->filename, ":", (long long)lx->line, ": gradient color hints are not supported, the hint is ignored");
        }
        else
        {
            if (out->stop_count >= GUI_GRADIENT_MAX_STOPS)
            {
                _parser_style_internal__error(lx, "gradient: too many color stops (at most 16)");
                return FALSE;
            }

            st = &out->stops[out->stop_count];

            if (_parser_style_internal__parse_color(lx, &st->color) == FALSE)
            {
                return FALSE;
            }

            out->stop_count++;
            _parser_style_internal__skip_ws_and_comments(lx);
            c = _parser_style_internal__peek(lx);

            if (_parser_style_internal__is_digit(c) == TRUE || c == '.' || c == '-' || stdlib__strncmp(lx->src + lx->pos, "calc(", 5) == 0)
            {
                if (_parser_style_internal__parse_stop_position(lx, conic, &v, &pct, &pixel_offset) == FALSE)
                {
                    return FALSE;
                }

                st->pos       = (pct == TRUE) ? v / 100.0f : v;
                st->has_pos   = TRUE;
                st->pos_is_px = (boole)(pct == FALSE);
                st->px_offset = pixel_offset;
                _parser_style_internal__skip_ws_and_comments(lx);
                c = _parser_style_internal__peek(lx);

                if (_parser_style_internal__is_digit(c) == TRUE || c == '.' || c == '-' || stdlib__strncmp(lx->src + lx->pos, "calc(", 5) == 0)
                {
                    // a second position repeats the color as another stop
                    if (out->stop_count >= GUI_GRADIENT_MAX_STOPS)
                    {
                        _parser_style_internal__error(lx, "gradient: too many color stops (at most 16)");
                        return FALSE;
                    }

                    if (_parser_style_internal__parse_stop_position(lx, conic, &v2, &pct2, &pixel_offset) == FALSE)
                    {
                        return FALSE;
                    }

                    out->stops[out->stop_count] = *st;
                    st                          = &out->stops[out->stop_count];
                    st->pos                     = (pct2 == TRUE) ? v2 / 100.0f : v2;
                    st->pos_is_px               = (boole)(pct2 == FALSE);
                    st->px_offset = pixel_offset;
                    out->stop_count++;
                }
            }
        }

        _parser_style_internal__skip_ws_and_comments(lx);

        if (_parser_style_internal__peek(lx) == ',')
        {
            _parser_style_internal__advance(lx);
            continue;
        }

        break;
    }

    if (_parser_style_internal__peek(lx) != ')')
    {
        _parser_style_internal__error(lx, "expected ')' to close the gradient");
        return FALSE;
    }

    _parser_style_internal__advance(lx);

    if (out->stop_count == 0)
    {
        _parser_style_internal__error(lx, "gradient: at least one color is needed");
        return FALSE;
    }

    return TRUE;
}

// the toolkit's two-color form, gradient( <from> [,] <to> [, direction] ),
// the `gradient` ident consumed
/**
 * @brief parse one complete CSS gradient value without registering a style rule
 *
 * @param const char* value -> null-terminated gradient expression
 * @param gui_gradient_t* out -> parsed gradient
 * @return boole -> TRUE if the entire expression is valid
 */
boole parser_style__parse_gradient(const char* value, gui_gradient_t* out)
{
    _parser_style_internal__lexer_t lx;
    char name[40];

    stdlib__null_memory(&lx, sizeof(lx));
    stdlib__null_memory(name, sizeof(name));
    if (value == NULL_POINTER || out == NULL_POINTER)
    {
        return FALSE;
    }
    lx.src = (char*)value;
    lx.len = (int64)stdlib__strlen(value);
    lx.line = 1;
    lx.filename = "gradient value";
    lx.quiet = TRUE;
    lx.ok = TRUE;
    _parser_style_internal__skip_ws_and_comments(&lx);
    if (_parser_style_internal__parse_ident(&lx, name, sizeof(name)) == FALSE || _parser_style_internal__is_gradient_fn(name) == FALSE)
    {
        return FALSE;
    }
    if (_parser_style_internal__parse_gradient_fn(&lx, name, out) == FALSE)
    {
        return FALSE;
    }
    _parser_style_internal__skip_ws_and_comments(&lx);
    return (boole)(lx.ok == TRUE && lx.pos == lx.len);
}

/**
 * @brief parse one complete CSS color value without registering a style rule
 *
 * @param const char* value -> null-terminated color expression
 * @param gui_color_t* out -> straight-alpha sRGB color
 * @return boole -> TRUE if the entire expression is valid
 */
boole parser_style__parse_color(const char* value, gui_color_t* out)
{
    _parser_style_internal__lexer_t lx;

    stdlib__null_memory(&lx, sizeof(lx));
    if (value == NULL_POINTER || out == NULL_POINTER)
    {
        return FALSE;
    }
    lx.src = (char*)value;
    lx.len = (int64)stdlib__strlen(value);
    lx.line = 1;
    lx.filename = "color value";
    lx.quiet = TRUE;
    lx.ok = TRUE;
    if (_parser_style_internal__parse_color(&lx, out) == FALSE)
    {
        return FALSE;
    }
    _parser_style_internal__skip_ws_and_comments(&lx);
    return (boole)(lx.ok == TRUE && lx.pos == lx.len);
}

static boole _parser_style_internal__parse_legacy_gradient_fn(_parser_style_internal__lexer_t* lx, gui_gradient_t* out)
{
    char               ident[40];
    gui_color_t        ca;
    gui_color_t        cb;
    gui_gradient_dir_e dir = GUI_GRADIENT_DIR__VERTICAL;

    stdlib__null_memory(ident, sizeof(ident));
    stdlib__null_memory(&ca, sizeof(ca));
    stdlib__null_memory(&cb, sizeof(cb));

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) != '(')
    {
        _parser_style_internal__error(lx, "gradient: expected '('");
        return FALSE;
    }

    _parser_style_internal__advance(lx);
    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__parse_color(lx, &ca) == FALSE)
    {
        return FALSE;
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == ',')
    {
        _parser_style_internal__advance(lx);
        _parser_style_internal__skip_ws_and_comments(lx);
    }

    if (_parser_style_internal__parse_color(lx, &cb) == FALSE)
    {
        return FALSE;
    }

    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == ',')
    {
        _parser_style_internal__advance(lx);
        _parser_style_internal__skip_ws_and_comments(lx);
    }

    if (_parser_style_internal__is_alpha(_parser_style_internal__peek(lx)) == TRUE)
    {
        if (_parser_style_internal__parse_ident(lx, ident, (int64)sizeof(ident)) == FALSE)
        {
            return FALSE;
        }

        if (_parser_style_internal__gradient_dir_keyword(ident, &dir) == FALSE)
        {
            log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown gradient direction '", ident, "', using vertical");
        }

        _parser_style_internal__skip_ws_and_comments(lx);
    }

    if (_parser_style_internal__peek(lx) != ')')
    {
        _parser_style_internal__error(lx, "gradient: expected ')'");
        return FALSE;
    }

    _parser_style_internal__advance(lx);
    gradient__two_color(out, ca, cb, dir);
    return TRUE;
}

// url( <path> ) with optional quotes, the `url` ident consumed
static boole _parser_style_internal__parse_url_path(_parser_style_internal__lexer_t* lx, char* buf, int64 cap)
{
    int64 n     = 0;
    char  quote = 0;
    char  c     = 0;

    buf[0] = 0;
    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) != '(')
    {
        _parser_style_internal__error(lx, "url: expected '('");
        return FALSE;
    }

    _parser_style_internal__advance(lx);
    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == '"' || _parser_style_internal__peek(lx) == '\'')
    {
        quote = _parser_style_internal__peek(lx);
        _parser_style_internal__advance(lx);
    }

    while (_parser_style_internal__eof(lx) == FALSE)
    {
        c = _parser_style_internal__peek(lx);

        if (quote != 0 && c == quote)
        {
            _parser_style_internal__advance(lx);
            break;
        }

        if (quote == 0 && c == ')')
        {
            break;
        }

        if (c == 0)
        {
            break;
        }

        if (n + 1 >= cap)
        {
            break;
        }

        buf[n++] = c;
        _parser_style_internal__advance(lx);
    }

    // an unquoted path keeps no trailing whitespace
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t'))
    {
        n--;
    }

    buf[n] = 0;
    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) != ')')
    {
        _parser_style_internal__error(lx, "url: expected ')'");
        return FALSE;
    }

    _parser_style_internal__advance(lx);
    return TRUE;
}

// a bare or quoted path value: ends at ';' or whitespace for bare values,
// at the matching quote for quoted ones (the font-family value shape)
static boole _parser_style_internal__parse_path_value(_parser_style_internal__lexer_t* lx, char* buf, int64 cap)
{
    int64 n     = 0;
    char  quote = 0;
    char  c     = 0;

    buf[0] = 0;
    _parser_style_internal__skip_ws_and_comments(lx);

    if (_parser_style_internal__peek(lx) == '"' || _parser_style_internal__peek(lx) == '\'')
    {
        quote = _parser_style_internal__peek(lx);
        _parser_style_internal__advance(lx);
    }

    while (_parser_style_internal__eof(lx) == FALSE)
    {
        c = _parser_style_internal__peek(lx);

        if (quote != 0 && c == quote)
        {
            _parser_style_internal__advance(lx);
            break;
        }

        if (quote == 0 && (c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n'))
        {
            break;
        }

        if (c == 0)
        {
            break;
        }

        if (n + 1 >= cap)
        {
            break;
        }

        buf[n++] = c;
        _parser_style_internal__advance(lx);
    }

    buf[n] = 0;
    return TRUE;
}

// Parse an unsigned decimal number into a float. Accepts "42",
// "3.14", "0.5". No leading '+' or '-', no exponent form (no "1e3").
// We could add those, but zero .style file needs them and keeping the
// grammar narrow shrinks the surface for bugs.
// Pipes through atof() because the digit parser here is just a copy
// loop -- we don't need to reimplement IEEE-754 from scratch for a
// style-file load.
static boole _parser_style_internal__parse_number(_parser_style_internal__lexer_t* lx, float* out)
{
    char  buf[32];
    int64 n = 0;
    boole seen_dot = FALSE;
    // Optional leading '-'. Useful for box-shadow offsets (`box-shadow:
    // -3 -3 8 #000`) and any future property that meaningfully
    // accepts a negative value. A bare '-' with no digits after it
    // gets caught by the n == 0 check below.
    if (_parser_style_internal__peek(lx) == '-')
    {
        buf[n++] = '-';
        _parser_style_internal__advance(lx);
    }
    while (_parser_style_internal__eof(lx) == FALSE)
    {
        char c = _parser_style_internal__peek(lx);
        if (c == '.')
        {
            // Reject a second '.' rather than silently letting atof
            // truncate at the first one (which would parse "3.14.15"
            // as 3.14 with no warning to the dev).
            if (seen_dot)
            {
                _parser_style_internal__error(lx, "number has multiple decimal points");
                return FALSE;
            }
            seen_dot = TRUE;
        }
        else if (_parser_style_internal__is_digit(c) == FALSE)
        {
            break;
        }
        if (n + 1 >= (int64)sizeof(buf))
        {
            _parser_style_internal__error(lx, "number too long");
            return FALSE;
        }
        buf[n++] = c;
        _parser_style_internal__advance(lx);
    }
    if (n == 0)
    {
        _parser_style_internal__error(lx, "expected number");
        return FALSE;
    }
    // A lone "-" passes the n == 0 guard but is not a number.
    // Catch it so atof("-") doesn't silently produce 0.0.
    if (n == 1 && buf[0] == '-')
    {
        _parser_style_internal__error(lx, "'-' with no digits is not a number");
        return FALSE;
    }
    buf[n] = 0;
    *out = (float)stdlib__atof(buf);
    // Optional trailing `px` suffix. CSS muscle memory writes
    // `font-size: 14px;` / `radius: 18px;` / `gap: 12px;` even
    // though px is the only unit our layout speaks. Accepting +
    // discarding the suffix keeps both forms parsing equivalently
    // so a single .style file can mix bare numbers and `Npx`
    // without surprise. Note this lives BELOW atof so the digits
    // are already in `buf`; the px just gets consumed off the
    // input stream.
    if (_parser_style_internal__peek(lx) == 'p')
    {
        _parser_style_internal__advance(lx);
        if (_parser_style_internal__peek(lx) == 'x')
        {
            _parser_style_internal__advance(lx);
        }
        else
        {
            // Lone 'p' that isn't 'px' is malformed -- the spec
            // doesn't define any other p-prefixed unit. Roll back
            // is annoying without a lexer mark, but failing loud
            // here means the dev sees the typo right away rather
            // than getting silently truncated to the bare number.
            _parser_style_internal__error(lx, "expected 'px' suffix or no unit after number");
            return FALSE;
        }
    }
    return TRUE;
}

// Parse "<w>x<h>": e.g. "200x40", "0x120". Used by `size` and any
// future two-axis sized property. We deliberately don't accept
// whitespace around the 'x' -- "200 x 40" is a parse error. This
// keeps the lexer simple and makes the syntax read like coordinates
// ("100x200 pixels").
// Either axis may be 0, meaning "auto" in the layout engine.
static boole _parser_style_internal__parse_size(_parser_style_internal__lexer_t* lx, float* out_w, float* out_h)
{
    if (_parser_style_internal__parse_number(lx, out_w) == FALSE)
    {
        return FALSE;
    }
    char c = _parser_style_internal__peek(lx);
    if (c != 'x' && c != 'X')
    {
        _parser_style_internal__error(lx, "expected 'x' between width and height");
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    if (_parser_style_internal__parse_number(lx, out_h) == FALSE)
    {
        return FALSE;
    }
    return TRUE;
}

// Parse the CSS box shorthand: one to four numbers (px suffix allowed)
// meaning all sides | top-bottom left-right | top left-right bottom |
// top right bottom left. Used by padding and margin.
static boole _parser_style_internal__parse_box(_parser_style_internal__lexer_t* lx, float* out_t, float* out_r, float* out_b, float* out_l)
{
    float vals[4];
    int64 count = 0;
    char  next = 0;

    vals[0] = 0.0f;
    vals[1] = 0.0f;
    vals[2] = 0.0f;
    vals[3] = 0.0f;

    while (count < 4)
    {
        if (_parser_style_internal__parse_number(lx, &vals[count]) == FALSE)
        {
            return FALSE;
        }

        count = count + 1;
        _parser_style_internal__skip_ws_and_comments(lx);
        next = _parser_style_internal__peek(lx);

        if (_parser_style_internal__is_digit(next) == FALSE && next != '-' && next != '.')
        {
            break;
        }
    }

    *out_t = vals[0];
    *out_r = (count >= 2) ? vals[1] : vals[0];
    *out_b = (count >= 3) ? vals[2] : vals[0];
    *out_l = (count == 4) ? vals[3] : *out_r;

    return TRUE;
}

// Parse a flex alignment keyword. The physical names left / right / top /
// bottom are accepted next to the CSS ones so either axis reads naturally.
// With `distributions` the space-* keywords are allowed (justify-content);
// without them the result is a gui_align_e, else a gui_justify_e.
static boole _parser_style_internal__parse_align(_parser_style_internal__lexer_t* lx, boole distributions, int* out)
{
    char ident[40];

    if (_parser_style_internal__parse_ident(lx, ident, (int64)sizeof(ident)) == FALSE)
    {
        return FALSE;
    }

    if (stdlib__strcmp(ident, "flex-start") == 0 || stdlib__strcmp(ident, "start") == 0 || stdlib__strcmp(ident, "left") == 0 || stdlib__strcmp(ident, "top") == 0)
    {
        *out = (distributions == TRUE) ? (int)GUI_JUSTIFY__START : (int)GUI_ALIGN__START;
        return TRUE;
    }

    if (stdlib__strcmp(ident, "center") == 0)
    {
        *out = (distributions == TRUE) ? (int)GUI_JUSTIFY__CENTER : (int)GUI_ALIGN__CENTER;
        return TRUE;
    }

    if (stdlib__strcmp(ident, "flex-end") == 0 || stdlib__strcmp(ident, "end") == 0 || stdlib__strcmp(ident, "right") == 0 || stdlib__strcmp(ident, "bottom") == 0)
    {
        *out = (distributions == TRUE) ? (int)GUI_JUSTIFY__END : (int)GUI_ALIGN__END;
        return TRUE;
    }

    if (distributions == TRUE)
    {
        if (stdlib__strcmp(ident, "space-between") == 0)
        {
            *out = (int)GUI_JUSTIFY__SPACE_BETWEEN;
            return TRUE;
        }

        if (stdlib__strcmp(ident, "space-around") == 0)
        {
            *out = (int)GUI_JUSTIFY__SPACE_AROUND;
            return TRUE;
        }

        if (stdlib__strcmp(ident, "space-evenly") == 0)
        {
            *out = (int)GUI_JUSTIFY__SPACE_EVENLY;
            return TRUE;
        }
    }
    else
    {
        if (stdlib__strcmp(ident, "stretch") == 0 || stdlib__strcmp(ident, "normal") == 0)
        {
            *out = (int)GUI_ALIGN__STRETCH;
            return TRUE;
        }

        if (stdlib__strcmp(ident, "auto") == 0)
        {
            *out = (int)GUI_ALIGN__UNSET;
            return TRUE;
        }
    }

    log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown alignment keyword '", ident, "'");
    return FALSE;
}

// Parse an easing specifier. Writes the enum kind to *out_kind and (for
// parameterised kinds) populates out_params. out_params must point to an
// array of 4 floats; untouched slots are zeroed for deterministic state.
// Accepted forms:
// linear | ease-in | ease-out | ease-in-out
// ease | ease-in-back | ease-out-back | ease-in-out-back | ease-out-bounce
// cubic-bezier(x1, y1, x2, y2)
// spring(stiffness, damping)
// The curve keyword is the same one CSS uses. `cubic-bezier` + `spring`
// require a parenthesised argument list; commas or whitespace separate
// the numbers, and the closing `)` is mandatory. Malformed args emit an
// error via the lexer's error mechanism and return FALSE.
static boole _parser_style_internal__parse_easing(_parser_style_internal__lexer_t* lx, gui_easing_e* out_kind, float out_params[4])
{
    out_params[0] = 0.0f;
    out_params[1] = 0.0f;
    out_params[2] = 0.0f;
    out_params[3] = 0.0f;

    char eas[24];
    if (_parser_style_internal__parse_ident(lx, eas, (int64)sizeof(eas)) == FALSE)
    {
        return FALSE;
    }

    if      (stdlib__strcmp(eas, "linear")           == 0)
    {
        *out_kind = GUI_EASING__LINEAR;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease-in")          == 0)
    {
        *out_kind = GUI_EASING__EASE_IN;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease-out")         == 0)
    {
        *out_kind = GUI_EASING__EASE_OUT;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease-in-out")      == 0)
    {
        *out_kind = GUI_EASING__EASE_IN_OUT;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease")             == 0)
    {
        *out_kind = GUI_EASING__EASE;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease-in-back")     == 0)
    {
        *out_kind = GUI_EASING__EASE_IN_BACK;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease-out-back")    == 0)
    {
        *out_kind = GUI_EASING__EASE_OUT_BACK;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease-in-out-back") == 0)
    {
        *out_kind = GUI_EASING__EASE_IN_OUT_BACK;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "ease-out-bounce")  == 0)
    {
        *out_kind = GUI_EASING__EASE_OUT_BOUNCE;
        return TRUE;
    }
    else if (stdlib__strcmp(eas, "cubic-bezier")     == 0 || stdlib__strcmp(eas, "spring") == 0)
    {
        // Parenthesised argument list. cubic-bezier takes 4 numbers,
        // spring takes 2. Numbers are comma- or whitespace-separated;
        // the parser eats either.
        boole is_bezier = (stdlib__strcmp(eas, "cubic-bezier") == 0);
        int   want      = is_bezier ? 4 : 2;

        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__peek(lx) != '(')
        {
            _parser_style_internal__error(lx, is_bezier ? "cubic-bezier expects '(x1, y1, x2, y2)'" : "spring expects '(stiffness, damping)'");
            return FALSE;
        }
        _parser_style_internal__advance(lx);

        for (int i = 0; i < want; i++)
        {
            _parser_style_internal__skip_ws_and_comments(lx);
            if (_parser_style_internal__parse_number(lx, &out_params[i]) == FALSE)
            {
                return FALSE;
            }
            _parser_style_internal__skip_ws_and_comments(lx);
            if (i < want - 1)
            {
                // Optional comma between args. Accepting ws-only
                // keeps `cubic-bezier(0 0 1 1)` valid too, matching
                // the loose style of the rest of this parser.
                if (_parser_style_internal__peek(lx) == ',')
                {
                    _parser_style_internal__advance(lx);
                }
            }
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__peek(lx) != ')')
        {
            _parser_style_internal__error(lx, "easing: expected ')'");
            return FALSE;
        }
        _parser_style_internal__advance(lx);

        *out_kind = is_bezier ? GUI_EASING__CUBIC_BEZIER : GUI_EASING__SPRING;
        return TRUE;
    }

    _parser_style_internal__error(lx, "easing must be one of: linear, ease-in, ease-out, ease-in-out, ease, ease-in-back, ease-out-back, ease-in-out-back, ease-out-bounce, cubic-bezier(...), spring(...)");
    return FALSE;
}

// ===========================================================================
// Declaration parser -- the big property-name dispatch.
// ===========================================================================
// Parse exactly one "property: value;" declaration. The mapping from
// property NAMES to gui_style_t FIELDS is a chain of strcmp/else-if.
// A table-driven approach (static array of {name, parser_fn, offset})
// would be cleaner, but this form:
// - Keeps each property's value-type visible at the call site,
// - Makes adding a property a single-location edit, and
// - Costs nothing meaningful at runtime (handful of strcmps per decl;
// parse runs once per .style file on load + on hot reload).
// Unknown properties are WARNED about but not fatal: we skip to the
// next ';' and keep going. That way a typo doesn't wreck the whole
// file; the known-good properties in the rest of the block still apply.

/**
 * @brief parses one length value: a number with an optional px suffix (pixels) or a % suffix (percent of the containing block)
 *
 * @param _parser_style_internal__lexer_t* lx -> lexer positioned at the value
 * @param float* out_v -> receives the number
 * @param boole* out_is_percent -> receives TRUE when the value carried a % suffix
 *
 * @return boole -> TRUE when a number was parsed
 */
static boole _parser_style_internal__parse_length(_parser_style_internal__lexer_t* lx, float* out_v, boole* out_is_percent)
{
    char c = 0;

    *out_is_percent = FALSE;

    if (_parser_style_internal__parse_number(lx, out_v) == FALSE)
    {
        return FALSE;
    }

    _parser_style_internal__skip_ws_and_comments(lx);
    c = _parser_style_internal__peek(lx);

    if (c == '%')
    {
        *out_is_percent = TRUE;
        _parser_style_internal__advance(lx);
    }

    return TRUE;
}

/** @brief parses independent checkbox-part geometry, fills and borders
 * @param _parser_style_internal__lexer_t* lx -> value lexer
 * @param const char* property -> part property after the track/thumb prefix
 * @param gui_checkbox_part_t* part -> destination partial style
 * @return boole -> true when the property and value are supported
 */
static boole _parser_style_internal__parse_checkbox_part(_parser_style_internal__lexer_t* lx, const char* property, gui_checkbox_part_t* part)
{
    char word[40] = {0};
    int64 length = 0;
    float value = 0.0f;
    uint flag = 0;
    float* destination = NULL_POINTER;

    if (stdlib__strcmp(property, "background") == 0)
    {
        length = _parser_style_internal__peek_ident(lx, word, sizeof(word));
        part->has_gradient = FALSE;
        if (length > 0 && _parser_style_internal__is_gradient_fn(word))
        {
            _parser_style_internal__advance_n(lx, length);
            if (!_parser_style_internal__parse_gradient_fn(lx, word, &part->gradient))
            {
                return FALSE;
            }
            part->has_gradient = TRUE;
            part->background = part->gradient.stops[0].color;
        }
        else if (!_parser_style_internal__parse_color(lx, &part->background))
        {
            return FALSE;
        }
        part->set |= GUI_CHECKBOX_PART__BACKGROUND;
        return TRUE;
    }
    if (stdlib__strcmp(property, "border-color") == 0)
    {
        length = _parser_style_internal__peek_ident(lx, word, sizeof(word));
        part->has_border_gradient = FALSE;
        if (length > 0 && _parser_style_internal__is_gradient_fn(word))
        {
            _parser_style_internal__advance_n(lx, length);
            if (!_parser_style_internal__parse_gradient_fn(lx, word, &part->border_gradient))
            {
                return FALSE;
            }
            part->has_border_gradient = TRUE;
            part->border_color = part->border_gradient.stops[0].color;
        }
        else if (!_parser_style_internal__parse_color(lx, &part->border_color))
        {
            return FALSE;
        }
        part->set |= GUI_CHECKBOX_PART__BORDER_COLOR;
        return TRUE;
    }
    if (stdlib__strcmp(property, "border-style") == 0)
    {
        if (!_parser_style_internal__parse_ident(lx, word, sizeof(word)))
        {
            return FALSE;
        }
        if (stdlib__strcmp(word, "none") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__NONE;
        }
        else if (stdlib__strcmp(word, "solid") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__SOLID;
        }
        else if (stdlib__strcmp(word, "dashed") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__DASHED;
        }
        else if (stdlib__strcmp(word, "dotted") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__DOTTED;
        }
        else if (stdlib__strcmp(word, "double") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__DOUBLE;
        }
        else if (stdlib__strcmp(word, "groove") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__GROOVE;
        }
        else if (stdlib__strcmp(word, "ridge") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__RIDGE;
        }
        else if (stdlib__strcmp(word, "inset") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__INSET;
        }
        else if (stdlib__strcmp(word, "outset") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__OUTSET;
        }
        else if (stdlib__strcmp(word, "hidden") == 0)
        {
            part->border_style = GUI_BORDER_STYLE__HIDDEN;
        }
        else
        {
            return FALSE;
        }
        part->set |= GUI_CHECKBOX_PART__BORDER_STYLE;
        return TRUE;
    }
    if (stdlib__strcmp(property, "width") == 0)
    {
        destination = &part->width;
        flag = GUI_CHECKBOX_PART__WIDTH;
    }
    else if (stdlib__strcmp(property, "height") == 0)
    {
        destination = &part->height;
        flag = GUI_CHECKBOX_PART__HEIGHT;
    }
    else if (stdlib__strcmp(property, "radius") == 0)
    {
        destination = &part->radius;
        flag = GUI_CHECKBOX_PART__RADIUS;
    }
    else if (stdlib__strcmp(property, "inset") == 0)
    {
        destination = &part->inset;
        flag = GUI_CHECKBOX_PART__INSET;
    }
    else if (stdlib__strcmp(property, "border-width") == 0)
    {
        destination = &part->border_width;
        flag = GUI_CHECKBOX_PART__BORDER_WIDTH;
    }
    if (destination == NULL_POINTER || !_parser_style_internal__parse_number(lx, &value) || value < 0.0f || value > 4096.0f)
    {
        return FALSE;
    }
    *destination = value;
    part->set |= flag;
    return TRUE;
}

static boole _parser_style_internal__parse_decl(_parser_style_internal__lexer_t* lx, gui_style_t* style)
{
    // ----- property name ---------------------------------------------------
    char prop[32];
    if (_parser_style_internal__parse_ident(lx, prop, (int64)sizeof(prop)) == FALSE)
    {
        return FALSE;
    }

    // ----- mandatory ':' ---------------------------------------------------
    // CSS requires ':' between name and value. We enforce it strictly.
    _parser_style_internal__skip_ws_and_comments(lx);
    if (_parser_style_internal__peek(lx) != ':')
    {
        _parser_style_internal__error(lx, "expected ':' after property name");
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    _parser_style_internal__skip_ws_and_comments(lx);

    // ----- value (dispatch on property name) ------------------------------
    // Each branch:
    // 1. Parses a value of the right shape (color, number, size, ident).
    // 2. Writes it into the matching gui_style_t field, setting any
    // has_* flag so the scene's overlay logic can tell "set" from
    // "default".
    boole known = TRUE;
    // Three color properties, one struct field each. Old short names
    // ("bg" / "fg" / "color") are accepted as aliases for the new
    // CSS-shaped names ("background-color" / "accent-color" /
    // "font-color") so existing .style files don't break -- but new
    // code should use the long names. The accent-color name comes
    // from CSS 4 where it controls slider thumbs / checkbox ticks /
    // etc., which is exactly what fg meant in this codebase.
    if (stdlib__strcmp(prop, "background-color") == 0 || stdlib__strcmp(prop, "bg") == 0)
    {
        // Background color. has_background_color differentiates "set
        // to black" from "not set" -- without the flag the resolver
        // couldn't tell whether to overlay.
        gui_color_t c;
        if (_parser_style_internal__parse_color(lx, &c) == FALSE)
        {
            return FALSE;
        }
        style->background_color     = c;
        style->has_background_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "accent-color") == 0 || stdlib__strcmp(prop, "fg") == 0)
    {
        // Active widget part: slider thumb, checkbox tick, radio dot,
        // select chevron + selected-row highlight, image tint. Matches
        // CSS 4's `accent-color` property, which controls the same
        // set of native form-control highlights.
        gui_color_t c;
        if (_parser_style_internal__parse_color(lx, &c) == FALSE)
        {
            return FALSE;
        }
        style->accent_color     = c;
        style->has_accent_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "font-color") == 0 || stdlib__strcmp(prop, "color") == 0)
    {
        // Text color. CSS calls this just `color`; we accept that as
        // an alias but prefer `font-color` because it's
        // unambiguously the text color (as opposed to bg, accent, or
        // any other "color" of a widget).
        gui_color_t c;
        if (_parser_style_internal__parse_color(lx, &c) == FALSE)
        {
            return FALSE;
        }
        style->font_color     = c;
        style->has_font_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "font-family") == 0)
    {
        // Font family name. Accepts EITHER a quoted string
        // (font_family: "Inter";) OR a bare identifier
        // (font_family: Inter;). CSS normally requires quotes for
        // multi-word family names; we don't have multi-word names
        // right now (TTF filenames are the family names), but the
        // quote support is there for future-proofing.
        // The value is everything up to ';' (with optional quotes
        // stripped), copied into style->font_family. Overflow is
        // silently truncated.
        char buf[64];
        int64 n = 0;
        _parser_style_internal__skip_ws_and_comments(lx);

        // Detect and consume an opening quote. We remember which
        // quote char it was so only the matching close ends the
        // value -- this lets a double-quoted value contain ' and
        // vice versa.
        char quote = 0;
        if (_parser_style_internal__peek(lx) == '"' || _parser_style_internal__peek(lx) == '\'')
        {
            quote = _parser_style_internal__peek(lx);
            _parser_style_internal__advance(lx);
        }

        // Read until end-of-value. For quoted values that's the
        // matching quote; for bare values that's ';' or whitespace.
        while (_parser_style_internal__eof(lx) == FALSE)
        {
            char c = _parser_style_internal__peek(lx);
            if (quote != 0 && c == quote)
            {
                _parser_style_internal__advance(lx); // consume closing quote.
                break;
            }
            if (quote == 0 && (c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            {
                break;
            }
            if (c == 0)
            {
                break;
            }
            if (n + 1 >= (int64)sizeof(buf))
            {
                break;
            }
            buf[n++] = c;
            _parser_style_internal__advance(lx);
        }
        buf[n] = 0;

        // Copy into the node style field, truncating if necessary
        // (style->font_family is a fixed-size char array). Truncation
        // is logged so the dev knows their long font name didn't
        // round-trip cleanly -- silent truncation would just produce
        // a font lookup miss + fallback to the first registered
        // family with no obvious cause.
        size_t copy_n = (size_t)n;
        if (copy_n >= sizeof(style->font_family))
        {
            log_warn("%s %s %s %zu %s", "font-family '", buf, "' truncated to", sizeof(style->font_family) - 1, "bytes (max name length)");
            copy_n = sizeof(style->font_family) - 1;
        }
        stdlib__memcpy(style->font_family, buf, copy_n);
        style->font_family[copy_n] = 0;
    }
    else if (stdlib__strcmp(prop, "font-size") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->font_size) == FALSE)
        {
            return FALSE;
        }
        style->font_size_explicit = TRUE;
    }
    else if (stdlib__strcmp(prop, "line-height") == 0)
    {
        // line-height: normal | <number> | <n>px. As in CSS the bare
        // number is a multiplier of the font size and px is absolute;
        // `normal` is the font's own line height and is stored as -1 so
        // it overrides an inherited multiplier.
        float v = 0.0f;
        int64 start = 0;
        _parser_style_internal__skip_ws_and_comments(lx);

        if (_parser_style_internal__peek(lx) == 'n')
        {
            char kw[16];
            if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
            {
                return FALSE;
            }
            if (stdlib__strcmp(kw, "normal") != 0)
            {
                _parser_style_internal__error(lx, "line-height must be normal, a number or <n>px");
                return FALSE;
            }
            style->line_height    = -1.0f;
            style->line_height_px = 0.0f;
        }
        else
        {
            start = lx->pos;
            if (_parser_style_internal__parse_number(lx, &v) == FALSE)
            {
                return FALSE;
            }
            if (v <= 0.0f)
            {
                _parser_style_internal__error(lx, "line-height must be positive");
                return FALSE;
            }
            // parse_number swallows a px suffix; the consumed text tells the two forms apart
            if (lx->pos - start >= 2 && lx->src[lx->pos - 1] == 'x' && lx->src[lx->pos - 2] == 'p')
            {
                style->line_height_px = v;
                style->line_height    = 0.0f;
            }
            else
            {
                style->line_height    = v;
                style->line_height_px = 0.0f;
            }
        }
    }
    else if (stdlib__strcmp(prop, "min-width") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->min_w) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "max-width") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->max_w) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "min-height") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->min_h) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "max-height") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->max_h) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "justify-content") == 0)
    {
        // main-axis alignment with the CSS name: vertical in a column,
        // horizontal in a row; adds space-between / -around / -evenly
        int v = 0;
        if (_parser_style_internal__parse_align(lx, TRUE, &v) == FALSE)
        {
            return FALSE;
        }
        style->justify_content = (gui_justify_e)v;
    }
    else if (stdlib__strcmp(prop, "align-items") == 0)
    {
        // cross-axis alignment of the children with the CSS name
        int v = 0;
        if (_parser_style_internal__parse_align(lx, FALSE, &v) == FALSE)
        {
            return FALSE;
        }
        style->align_items = (gui_align_e)v;
    }
    else if (stdlib__strcmp(prop, "align-self") == 0)
    {
        // a child's own cross-axis alignment, overriding the parent's align-items
        int v = 0;
        if (_parser_style_internal__parse_align(lx, FALSE, &v) == FALSE)
        {
            return FALSE;
        }
        style->align_self = (gui_align_e)v;
    }
    else if (stdlib__strcmp(prop, "text-transform") == 0)
    {
        // text-transform: none | uppercase | lowercase
        char ident[16];
        if (_parser_style_internal__parse_ident(lx, ident, (int64)sizeof(ident)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(ident, "none") == 0)
        {
            style->text_transform = GUI_TEXT_TRANSFORM__NONE;
        }
        else if (stdlib__strcmp(ident, "uppercase") == 0)
        {
            style->text_transform = GUI_TEXT_TRANSFORM__UPPERCASE;
        }
        else if (stdlib__strcmp(ident, "lowercase") == 0)
        {
            style->text_transform = GUI_TEXT_TRANSFORM__LOWERCASE;
        }
        else
        {
            log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown text-transform '", ident, "' (none|uppercase|lowercase)");
        }
    }
    else if (stdlib__strcmp(prop, "text-align") == 0)
    {
        char ident[16];
        if (_parser_style_internal__parse_ident(lx, ident, (int64)sizeof(ident)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(ident, "left") == 0 || stdlib__strcmp(ident, "start") == 0)
        {
            style->text_align = GUI_TEXT_ALIGN__LEFT;
        }
        else if (stdlib__strcmp(ident, "center") == 0)
        {
            style->text_align = GUI_TEXT_ALIGN__CENTER;
        }
        else if (stdlib__strcmp(ident, "right") == 0 || stdlib__strcmp(ident, "end") == 0)
        {
            style->text_align = GUI_TEXT_ALIGN__RIGHT;
        }
        else
        {
            log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown text-align '", ident, "' (left|center|right)");
        }
    }
    else if (stdlib__strcmp(prop, "radius") == 0 || stdlib__strcmp(prop, "border-radius") == 0)
    {
        // `radius` is our canonical name; `border-radius` is an
        // alias for CSS muscle memory. Both set the same field.
        // Alias added after the meek-shell D2 test run produced a
        // cascade of typos from contributors trained on CSS; see
        // session/memory/reference_style_property_names.md.
        if (_parser_style_internal__parse_number(lx, &style->radius) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "padding") == 0 || stdlib__strcmp(prop, "pad") == 0)
    {
        // The CSS shorthand: `padding: 12;` sets all four sides,
        // `padding: 8 16;` top-bottom and left-right, three values
        // top / left-right / bottom, four values top right bottom
        // left. Old name `pad:` kept as alias for .style files
        // predating the rename. Per-side versions (padding-top etc.)
        // are handled just below; they OVERRIDE whatever this shorthand
        // produced if they appear later in the same rule block.
        if (_parser_style_internal__parse_box(lx, &style->pad_t, &style->pad_r, &style->pad_b, &style->pad_l) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "padding-top")    == 0 || stdlib__strcmp(prop, "pad-t") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->pad_t) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "padding-right")  == 0 || stdlib__strcmp(prop, "pad-r") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->pad_r) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "padding-bottom") == 0 || stdlib__strcmp(prop, "pad-b") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->pad_b) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "padding-left")   == 0 || stdlib__strcmp(prop, "pad-l") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->pad_l) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "margin") == 0)
    {
        // CSS-aligned margin: space AROUND this element. Different
        // from gap, which is space BETWEEN siblings inside a flex/
        // grid container. The shorthand takes one to four values with
        // the CSS meaning (see padding); per-side versions below
        // override.
        if (_parser_style_internal__parse_box(lx, &style->margin_t, &style->margin_r, &style->margin_b, &style->margin_l) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "margin-top") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->margin_t) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "margin-right") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->margin_r) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "margin-bottom") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->margin_b) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "margin-left") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->margin_l) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "gap") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->gap) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "size") == 0)
    {
        if (_parser_style_internal__parse_size(lx, &style->size_w, &style->size_h) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "overflow") == 0 || stdlib__strcmp(prop, "overflow-x") == 0 || stdlib__strcmp(prop, "overflow-y") == 0)
    {
        // `overflow` takes one of four keyword values:
        // visible  default, no clip, no scrollbar
        // hidden   clip to bounds (clipping deferred), no scrollbar
        // scroll   always show scrollbar
        // auto     show scrollbar only if content exceeds bounds
        // The shorthand `overflow: <kw>;` writes BOTH axes; the
        // axis-specific forms write just one. This mirrors CSS.
        // Widget-level: currently only <div> respects these fields.
        // See widget_div.c.
        char kw[16];
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        gui_overflow_e mode;
        if      (stdlib__strcmp(kw, "visible") == 0)
        {
            mode = GUI_OVERFLOW__VISIBLE;
        }
        else if (stdlib__strcmp(kw, "hidden")  == 0)
        {
            mode = GUI_OVERFLOW__HIDDEN;
        }
        else if (stdlib__strcmp(kw, "scroll")  == 0)
        {
            mode = GUI_OVERFLOW__SCROLL;
        }
        else if (stdlib__strcmp(kw, "auto")    == 0)
        {
            mode = GUI_OVERFLOW__AUTO;
        }
        else
        {
            _parser_style_internal__error(lx, "overflow expects one of: visible, hidden, scroll, auto");
            return FALSE;
        }
        if (stdlib__strcmp(prop, "overflow-x") == 0)
        {
            style->overflow_x = mode;
        }
        else if (stdlib__strcmp(prop, "overflow-y") == 0)
        {
            style->overflow_y = mode;
        }
        else
        {
            // Shorthand: applies to both axes.
            style->overflow_x = mode;
            style->overflow_y = mode;
        }
    }
    else if (stdlib__strcmp(prop, "scrollbar-size") == 0)
    {
        // Thickness of the scrollbar in logical pixels. A vertical
        // scrollbar is scrollbar_size px wide; a horizontal one is
        // scrollbar_size px tall. 0 (the default) resolves to
        // widget_div's built-in 12px.
        if (_parser_style_internal__parse_number(lx, &style->scrollbar_size) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "scrollbar-radius") == 0)
    {
        // Shared corner radius for track + thumb. 0 inherits the
        // widget's "reasonable default" (roughly half the bar size,
        // so the thumb reads as a pill).
        if (_parser_style_internal__parse_number(lx, &style->scrollbar_radius) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "scrollbar-track") == 0)
    {
        // Track (channel) color. Typically a dim version of the
        // containing div's own bg.
        gui_color_t c;
        if (_parser_style_internal__parse_color(lx, &c) == FALSE)
        {
            return FALSE;
        }
        style->scrollbar_track     = c;
        style->has_scrollbar_track = TRUE;
    }
    else if (stdlib__strcmp(prop, "scrollbar-thumb") == 0)
    {
        // Thumb color. Typically brighter than the track so the
        // draggable element reads clearly.
        gui_color_t c;
        if (_parser_style_internal__parse_color(lx, &c) == FALSE)
        {
            return FALSE;
        }
        style->scrollbar_thumb     = c;
        style->has_scrollbar_thumb = TRUE;
    }
    else if (stdlib__strcmp(prop, "appear") == 0)
    {
        // appear: <duration_ms> [easing]
        // Sets up the "fade in on first show" animation. Duration is a
        // plain number interpreted as milliseconds (CSS uses `ms` /
        // `s` suffixes; we don't bother -- everything is ms here).
        // Easing is an optional second token: linear / ease_in /
        // ease_out / ease_in_out. Defaults to ease_out (the "pop"
        // curve). For separately specifying just the easing without
        // the duration, use the appear_easing property below.
        // Examples:
        // appear: 600;             -> 600 ms, ease_out
        // appear: 250 ease_in;     -> 250 ms, ease_in
        // appear: 800 linear;      -> 800 ms, linear
        if (_parser_style_internal__parse_number(lx, &style->appear_ms) == FALSE)
        {
            return FALSE;
        }
        // Default easing is ease_out unless an explicit easing follows.
        // (Setting it here also means a subsequent `appear_easing` decl
        // can override.)
        style->appear_easing = GUI_EASING__EASE_OUT;

        // Look for an optional easing identifier after the number,
        // separated by whitespace. If the next non-whitespace char is
        // ';' there's no easing -- skip the parse.
        _parser_style_internal__skip_ws_and_comments(lx);
        char nxt = _parser_style_internal__peek(lx);
        if (nxt != ';')
        {
            if (_parser_style_internal__parse_easing(lx, &style->appear_easing, style->appear_easing_params) == FALSE)
            {
                return FALSE;
            }
        }
    }
    else if (stdlib__strcmp(prop, "appear-easing") == 0)
    {
        // Standalone easing setter. Useful when the duration is
        // inherited from a parent rule but you want a different
        // easing on this node. No-op if appear_ms hasn't been set
        // anywhere up the chain (overlay only propagates easing
        // alongside a non-zero appear_ms).
        if (_parser_style_internal__parse_easing(lx, &style->appear_easing, style->appear_easing_params) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "appear-threshold") == 0 || stdlib__strcmp(prop, "disappear-threshold") == 0)
    {
        // appear-threshold / disappear-threshold: <fraction 0..1>
        // A value above 1 is read as a percentage (75 = 0.75).
        float v = 0.0f;
        if (_parser_style_internal__parse_number(lx, &v) == FALSE)
        {
            return FALSE;
        }
        if (v > 1.0f)
        {
            v = v / 100.0f;
        }
        if (v < 0.0f)
        {
            v = 0.0f;
        }
        if (v > 1.0f)
        {
            v = 1.0f;
        }
        if (prop[0] == 'a')
        {
            style->appear_threshold     = v;
            style->has_appear_threshold = TRUE;
        }
        else
        {
            style->disappear_threshold     = v;
            style->has_disappear_threshold = TRUE;
        }
    }
    else if (stdlib__strcmp(prop, "animation") == 0)
    {
        // animation: <duration_ms> [easing]
        // Used INSIDE a per-state pseudo block (typically :appear, also
        // :disappear once that lands) to control how the animator moves
        // from this state's per-property values to the default-state
        // values. The pseudo-state's own :appear / :disappear name
        // already says WHEN the animation runs; this declaration only
        // controls duration + curve.
        // Optional inside :appear -- if omitted, animator falls back to
        // sensible defaults (300 ms ease_out). So the minimum useful
        // appear block is just `:appear { bg: ...; }` with no other
        // metadata.
        // Examples (all valid inside :appear { ... }):
        // animation: 600;             -> 600 ms, ease_out
        // animation: 250 ease_in;     -> 250 ms, ease_in
        // animation: 800 linear;      -> 800 ms, linear
        if (_parser_style_internal__parse_number(lx, &style->animation_duration_ms) == FALSE)
        {
            return FALSE;
        }
        style->animation_easing = GUI_EASING__EASE_OUT; // default if no easing follows.

        _parser_style_internal__skip_ws_and_comments(lx);
        char nxt = _parser_style_internal__peek(lx);
        if (nxt != ';')
        {
            if (_parser_style_internal__parse_easing(lx, &style->animation_easing, style->animation_easing_params) == FALSE)
            {
                return FALSE;
            }
        }
    }
    else if (stdlib__strcmp(prop, "transition") == 0)
    {
        // transition: <prop> <duration_ms> [easing]
        // Generic state-change interpolation. Unlike :appear / :disappear
        // which play a one-shot entrance/exit, `transition` smooths every
        // subsequent change to the named property so hover <-> default,
        // focus <-> default, etc. all animate. Distinct data on the style
        // struct (transition_duration_ms / transition_easing) so it
        // doesn't collide with the :appear declaration.
        // <prop> is currently `all` only. Named-property filtering is a
        // future refinement; for now any animated property lerps when
        // duration > 0.
        // Examples:
        // transition: all 200;                      -> 200 ms ease-out
        // transition: all 300 ease-in-out;
        // transition: all 600 spring(18, 4);
        char pname[16];
        if (_parser_style_internal__parse_ident(lx, pname, (int64)sizeof(pname)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(pname, "all") != 0)
        {
            // Warn but accept -- we store the duration on the style
            // regardless so the author still gets motion, just on all
            // props instead of just the named one. Named filtering will
            // turn the warning into "only this prop animates".
            _parser_style_internal__error(lx, "transition: named properties not yet supported; using 'all'");
            // Not a hard error; fall through and keep parsing.
        }

        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__parse_number(lx, &style->transition_duration_ms) == FALSE)
        {
            return FALSE;
        }
        if (_parser_style_internal__peek(lx) == 'm')
        {
            _parser_style_internal__advance(lx);
            if (_parser_style_internal__peek(lx) != 's') return FALSE;
            _parser_style_internal__advance(lx);
        }
        style->transition_easing = GUI_EASING__EASE_OUT; // default if no easing follows.

        _parser_style_internal__skip_ws_and_comments(lx);
        char nxt = _parser_style_internal__peek(lx);
        if (nxt != ';')
        {
            if (_parser_style_internal__parse_easing(lx, &style->transition_easing, style->transition_easing_params) == FALSE)
            {
                return FALSE;
            }
        }
    }
    else if (stdlib__strcmp(prop, "visibility") == 0)
    {
        // visibility: visible | hidden
        // CSS-style per-node visibility. Hidden nodes still occupy
        // layout space (siblings don't reflow) but emit_draws skips
        // them. See gui_visibility_e in gui.h for the full behaviour
        // spec. Not inherited in this PoC: a hidden parent is the
        // only node that's hidden; children decide for themselves.
        // To hide a subtree, use display: none on the parent.
        char kw[16];
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if      (stdlib__strcmp(kw, "visible") == 0)
        {
            style->visibility = GUI_VISIBILITY__VISIBLE;
        }
        else if (stdlib__strcmp(kw, "hidden")  == 0)
        {
            style->visibility = GUI_VISIBILITY__HIDDEN;
        }
        else
        {
            _parser_style_internal__error(lx, "visibility must be one of: visible, hidden");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "display") == 0)
    {
        // display: block | none
        // CSS-style per-node display. `none` removes the node AND its
        // subtree from BOTH layout and rendering -- siblings collapse
        // into the space it would have taken. `block` is the default
        // "node participates normally" value. HTML's many other display
        // values (inline, flex, grid, ...) collapse into `block` in
        // this PoC because we only have column + row layout.
        char kw[16];
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        style->display_set = TRUE;
        if      (stdlib__strcmp(kw, "block") == 0)
        {
            style->display = GUI_DISPLAY__BLOCK;
        }
        else if (stdlib__strcmp(kw, "none")  == 0)
        {
            style->display = GUI_DISPLAY__NONE;
        }
        else if (stdlib__strcmp(kw, "flex") == 0)
        {
            style->display = GUI_DISPLAY__FLEX;
        }
        else
        {
            _parser_style_internal__error(lx, "display must be one of: block, none, flex");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "flex-direction") == 0 || stdlib__strcmp(prop, "table-layout") == 0 || stdlib__strcmp(prop, "border-collapse") == 0 || stdlib__strcmp(prop, "vertical-align") == 0)
    {
        char keyword[24];
        int value = 0;
        stdlib__null_memory(keyword, sizeof(keyword));
        if (_parser_style_internal__parse_ident(lx, keyword, sizeof(keyword)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(prop, "flex-direction") == 0)
        {
            value = stdlib__strcmp(keyword, "row") == 0 ? 1 : (stdlib__strcmp(keyword, "column") == 0 ? 2 : 0);
            style->flex_direction = value;
        }
        else if (stdlib__strcmp(prop, "table-layout") == 0)
        {
            value = stdlib__strcmp(keyword, "auto") == 0 ? 1 : (stdlib__strcmp(keyword, "fixed") == 0 ? 2 : 0);
            style->table_layout = value;
        }
        else if (stdlib__strcmp(prop, "border-collapse") == 0)
        {
            value = stdlib__strcmp(keyword, "separate") == 0 ? 1 : (stdlib__strcmp(keyword, "collapse") == 0 ? 2 : 0);
            style->border_collapse = value;
        }
        else
        {
            value = stdlib__strcmp(keyword, "top") == 0 ? 1 : (stdlib__strcmp(keyword, "middle") == 0 ? 2 : (stdlib__strcmp(keyword, "bottom") == 0 ? 3 : 0));
            style->table_vertical_align = value;
        }
        if (value == 0)
        {
            _parser_style_internal__error(lx, "unsupported layout keyword");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "border-spacing") == 0)
    {
        float horizontal = 0.0f;
        float vertical = 0.0f;
        if (_parser_style_internal__parse_number(lx, &horizontal) == FALSE)
        {
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        vertical = horizontal;
        if (_parser_style_internal__peek(lx) != ';' && _parser_style_internal__peek(lx) != '}')
        {
            if (_parser_style_internal__parse_number(lx, &vertical) == FALSE) { return FALSE; }
            }
        if (horizontal < 0.0f || vertical < 0.0f) { return FALSE; }
        style->border_spacing_x = horizontal;
        style->border_spacing_y = vertical;
        style->border_spacing_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "position") == 0)
    {
        // position: static | relative | absolute | fixed | sticky
        // Same meaning as in CSS; see gui_position_e in gui.h.
        char kw[16];
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if      (stdlib__strcmp(kw, "static")   == 0)
        {
            style->position = GUI_POSITION__STATIC;
        }
        else if (stdlib__strcmp(kw, "relative") == 0)
        {
            style->position = GUI_POSITION__RELATIVE;
        }
        else if (stdlib__strcmp(kw, "absolute") == 0)
        {
            style->position = GUI_POSITION__ABSOLUTE;
        }
        else if (stdlib__strcmp(kw, "fixed")    == 0)
        {
            style->position = GUI_POSITION__FIXED;
        }
        else if (stdlib__strcmp(kw, "sticky")   == 0)
        {
            style->position = GUI_POSITION__STICKY;
        }
        else
        {
            _parser_style_internal__error(lx, "position must be one of: static, relative, absolute, fixed, sticky");
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "inset") == 0)
    {
        // inset: <all> | <top-bottom> <left-right> | <top> <left-right> <bottom> | <top> <right> <bottom> <left>
        // CSS shorthand for the four edge offsets; each value is px or %.
        float vals[4];
        boole pcts[4];
        int64 count = 0;
        int64 i = 0;
        int64 idx_t = 0;
        int64 idx_r = 0;
        int64 idx_b = 0;
        int64 idx_l = 0;
        char  next = 0;

        for (i = 0; i < 4; i++)
        {
            vals[i] = 0.0f;
            pcts[i] = FALSE;
        }

        while (count < 4)
        {
            if (_parser_style_internal__parse_length(lx, &vals[count], &pcts[count]) == FALSE)
            {
                return FALSE;
            }

            count = count + 1;
            _parser_style_internal__skip_ws_and_comments(lx);
            next = _parser_style_internal__peek(lx);

            if (_parser_style_internal__is_digit(next) == FALSE && next != '-' && next != '.')
            {
                break;
            }
        }

        idx_r = (count >= 2) ? 1 : 0;
        idx_b = (count >= 3) ? 2 : 0;
        idx_l = (count == 4) ? 3 : idx_r;

        style->inset_t     = (pcts[idx_t] == TRUE) ? 0.0f : vals[idx_t];
        style->inset_t_pct = (pcts[idx_t] == TRUE) ? vals[idx_t] : 0.0f;
        style->inset_t_set = TRUE;
        style->inset_r     = (pcts[idx_r] == TRUE) ? 0.0f : vals[idx_r];
        style->inset_r_pct = (pcts[idx_r] == TRUE) ? vals[idx_r] : 0.0f;
        style->inset_r_set = TRUE;
        style->inset_b     = (pcts[idx_b] == TRUE) ? 0.0f : vals[idx_b];
        style->inset_b_pct = (pcts[idx_b] == TRUE) ? vals[idx_b] : 0.0f;
        style->inset_b_set = TRUE;
        style->inset_l     = (pcts[idx_l] == TRUE) ? 0.0f : vals[idx_l];
        style->inset_l_pct = (pcts[idx_l] == TRUE) ? vals[idx_l] : 0.0f;
        style->inset_l_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "top")    == 0 || stdlib__strcmp(prop, "right")  == 0 || stdlib__strcmp(prop, "bottom") == 0 || stdlib__strcmp(prop, "left")   == 0)
    {
        // CSS insets for positioned children. Bare number = px;
        // `<n>%` = percent of parent's content w (left/right) or
        // h (top/bottom). Same px / pct dual-slot pattern as
        // width / height -- whichever slot is set wins; setting
        // px clears pct and vice versa.
        // Read by relative, absolute, fixed and sticky layout; a
        // static element ignores them. Writing an edge also sets its
        // inset_*_set flag so 0 and negative values count as set.
        float v = 0.0f;
        if (_parser_style_internal__parse_number(lx, &v) == FALSE)
        {
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);

        char c = _parser_style_internal__peek(lx);
        boole is_percent = FALSE;
        if (c == '%')
        {
            is_percent = TRUE;
            _parser_style_internal__advance(lx);
        }
        else if (c == 'p')
        {
            _parser_style_internal__advance(lx);
            if (_parser_style_internal__peek(lx) == 'x')
            {
                _parser_style_internal__advance(lx);
            }
        }

        if      (stdlib__strcmp(prop, "top")    == 0)
        {
            if (is_percent)
            {
                style->inset_t_pct = v;
                style->inset_t = 0.0f;
            }
            else
            {
                style->inset_t     = v;
                style->inset_t_pct = 0.0f;
            }
            style->inset_t_set = TRUE;
        }
        else if (stdlib__strcmp(prop, "right")  == 0)
        {
            if (is_percent)
            {
                style->inset_r_pct = v;
                style->inset_r = 0.0f;
            }
            else
            {
                style->inset_r     = v;
                style->inset_r_pct = 0.0f;
            }
            style->inset_r_set = TRUE;
        }
        else if (stdlib__strcmp(prop, "bottom") == 0)
        {
            if (is_percent)
            {
                style->inset_b_pct = v;
                style->inset_b = 0.0f;
            }
            else
            {
                style->inset_b     = v;
                style->inset_b_pct = 0.0f;
            }
            style->inset_b_set = TRUE;
        }
        else
        {
            if (is_percent)
            {
                style->inset_l_pct = v;
                style->inset_l = 0.0f;
            }
            else
            {
                style->inset_l     = v;
                style->inset_l_pct = 0.0f;
            }
            style->inset_l_set = TRUE;
        }
    }
    else if (stdlib__strcmp(prop, "scroll-smooth") == 0)
    {
        // scroll_smooth: <duration_ms>
        // Opt-in smooth scrolling for any scrollable container. Wheel
        // input and thumb drag both set the node's scroll_y_target
        // (desired position); the animator then lerps scroll_y toward
        // target over roughly this many milliseconds (exponential
        // decay, so the tail asymptotes). 0 = instant (default).
        if (_parser_style_internal__parse_number(lx, &style->scroll_smooth_ms) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "scroll-end-effect") == 0)
    {
        // scroll-end-effect: none | wobble [<amount 0..1>]. wobble moves the
        // scrolled content with a spring that passes its target and
        // swings back; the amount is 0.5 when the keyword stands alone.
        char kw[16];
        char next = 0;

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(kw, "none") == 0)
        {
            style->scroll_end_effect = GUI_SCROLL_END_EFFECT__NONE;
            style->scroll_end_effect_amount = 0.0f;
        }
        else if (stdlib__strcmp(kw, "wobble") == 0)
        {
            style->scroll_end_effect = GUI_SCROLL_END_EFFECT__WOBBLE;
            style->scroll_end_effect_amount = 0.5f;
            _parser_style_internal__skip_ws_and_comments(lx);
            next = _parser_style_internal__peek(lx);
            if (_parser_style_internal__is_digit(next) == TRUE || next == '.')
            {
                if (_parser_style_internal__parse_number(lx, &style->scroll_end_effect_amount) == FALSE)
                {
                    return FALSE;
                }
                if (style->scroll_end_effect_amount > 1.0f)
                {
                    style->scroll_end_effect_amount = 1.0f;
                }
            }
        }
        else
        {
            return FALSE;
        }
        style->scroll_end_effect_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "scroll-start-effect") == 0)
    {
        char kw[16] = {0};
        char next = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE) return FALSE;
        if (stdlib__strcmp(kw, "none") == 0)
        {
            style->scroll_start_effect = GUI_SCROLL_START_EFFECT__NONE;
            style->scroll_start_effect_amount = 0.0f;
        }
        else if (stdlib__strcmp(kw, "stretch") == 0)
        {
            style->scroll_start_effect = GUI_SCROLL_START_EFFECT__STRETCH;
            style->scroll_start_effect_amount = 0.6f;
            _parser_style_internal__skip_ws_and_comments(lx);
            next = _parser_style_internal__peek(lx);
            if (_parser_style_internal__is_digit(next) == TRUE || next == '.' || next == '-')
            {
                if (_parser_style_internal__parse_number(lx, &style->scroll_start_effect_amount) == FALSE) return FALSE;
                style->scroll_start_effect_amount = stdlib__fminf(1.0f, stdlib__fmaxf(0.0f, style->scroll_start_effect_amount));
            }
        }
        else return FALSE;
        style->scroll_start_effect_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "overscroll-behavior") == 0 || stdlib__strcmp(prop, "overscroll-behavior-x") == 0 || stdlib__strcmp(prop, "overscroll-behavior-y") == 0)
    {
        // overscroll-behavior: auto | contain | none (the CSS keywords).
        // auto and contain let the content be pulled past its edges and
        // spring back (the rubber band of iOS); none, the default, stops
        // at the edge. The shorthand takes one keyword for both axes.
        // Vertical gestures (pan, fling, wheel) exist today; the x flag
        // waits for horizontal gestures.
        char kw[16];
        boole on = FALSE;

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(kw, "auto") != 0 && stdlib__strcmp(kw, "contain") != 0 && stdlib__strcmp(kw, "none") != 0)
        {
            return FALSE;
        }
        on = (boole)(stdlib__strcmp(kw, "none") != 0);
        if (stdlib__strcmp(prop, "overscroll-behavior-x") == 0)
        {
            style->overscroll_x = on;
        }
        else if (stdlib__strcmp(prop, "overscroll-behavior-y") == 0)
        {
            style->overscroll_y = on;
        }
        else
        {
            style->overscroll_x = on;
            style->overscroll_y = on;
        }
        style->overscroll_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "overscroll-resistance") == 0)
    {
        // overscroll-resistance: <number 0..1>. How much of a movement
        // past the edge the content follows right at the edge (0.55 by
        // default); it diminishes to nothing at overscroll-limit.
        if (_parser_style_internal__parse_number(lx, &style->overscroll_resistance) == FALSE)
        {
            return FALSE;
        }
        if (style->overscroll_resistance < 0.01f)
        {
            style->overscroll_resistance = 0.01f;
        }
        if (style->overscroll_resistance > 1.0f)
        {
            style->overscroll_resistance = 1.0f;
        }
    }
    else if (stdlib__strcmp(prop, "overscroll-limit") == 0)
    {
        // overscroll-limit: <length> | <percentage> of the box (50 % by
        // default). The farthest the content can be pulled past an edge.
        float v = 0.0f;
        boole pct = FALSE;

        if (_parser_style_internal__parse_length(lx, &v, &pct) == FALSE)
        {
            return FALSE;
        }
        style->overscroll_limit_px = (pct == TRUE) ? 0.0f : v;
        style->overscroll_limit_pct = (pct == TRUE) ? v : 0.0f;
    }
    else if (stdlib__strcmp(prop, "overscroll-duration") == 0)
    {
        // overscroll-duration: <milliseconds> of the spring back (350 by
        // default), a plain number like scroll-smooth.
        if (_parser_style_internal__parse_number(lx, &style->overscroll_duration_ms) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "overscroll-timing-function") == 0)
    {
        // overscroll-timing-function: the easing of the spring back, the
        // animation-timing-function vocabulary (ease-out by default;
        // ease-out-back overshoots the edge once, ease-out-bounce bounces,
        // cubic-bezier(x1, y1, x2, y2) for anything else).
        if (_parser_style_internal__parse_easing(lx, &style->overscroll_easing, style->overscroll_easing_params) == FALSE)
        {
            return FALSE;
        }
        style->overscroll_easing_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "appearance") == 0)
    {
        // appearance: auto | none | switch. switch draws a checkbox as a
        // pill with a knob (the toggle of a settings menu).
        char kw[16];

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(kw, "auto") != 0 && stdlib__strcmp(kw, "none") != 0 && stdlib__strcmp(kw, "switch") != 0)
        {
            return FALSE;
        }
        style->switch_look = (boole)(stdlib__strcmp(kw, "switch") == 0);
    }
    else if (stdlib__strncmp(prop, "checkbox-track-", 15) == 0 || stdlib__strncmp(prop, "checkbox-thumb-", 15) == 0)
    {
        gui_checkbox_part_t* part = prop[9] == 't' && prop[10] == 'r' ? &style->checkbox_track : &style->checkbox_thumb;
        if (!_parser_style_internal__parse_checkbox_part(lx, prop + 15, part))
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "checkbox-mark") == 0)
    {
        char word[16] = {0};
        if (!_parser_style_internal__parse_ident(lx, word, sizeof(word)))
        {
            return FALSE;
        }
        if (stdlib__strcmp(word, "check") == 0)
        {
            style->checkbox_mark = GUI_CHECKBOX_MARK__CHECK;
        }
        else if (stdlib__strcmp(word, "cross") == 0)
        {
            style->checkbox_mark = GUI_CHECKBOX_MARK__CROSS;
        }
        else if (stdlib__strcmp(word, "bar") == 0)
        {
            style->checkbox_mark = GUI_CHECKBOX_MARK__BAR;
        }
        else if (stdlib__strcmp(word, "fill") == 0)
        {
            style->checkbox_mark = GUI_CHECKBOX_MARK__FILL;
        }
        else if (stdlib__strcmp(word, "none") == 0)
        {
            style->checkbox_mark = GUI_CHECKBOX_MARK__NONE;
        }
        else
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "checkbox-mark-color") == 0)
    {
        if (!_parser_style_internal__parse_color(lx, &style->checkbox_mark_color))
        {
            return FALSE;
        }
        style->has_checkbox_mark_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "checkbox-mark-width") == 0 || stdlib__strcmp(prop, "checkbox-mark-inset") == 0)
    {
        float value = 0.0f;
        if (!_parser_style_internal__parse_number(lx, &value) || value < 0.0f || value > 4096.0f)
        {
            return FALSE;
        }
        if (stdlib__strcmp(prop, "checkbox-mark-width") == 0)
        {
            style->checkbox_mark_width = value;
            style->has_checkbox_mark_width = TRUE;
        }
        else
        {
            style->checkbox_mark_inset = value;
            style->has_checkbox_mark_inset = TRUE;
        }
    }
    else if (stdlib__strcmp(prop, "switch-track-height") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->switch_track_h) == FALSE || style->switch_track_h < 0.0f)
        {
            return FALSE;
        }
        style->has_switch_track_h = TRUE;
    }
    else if (stdlib__strcmp(prop, "switch-thumb-size") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->switch_thumb_size) == FALSE || style->switch_thumb_size < 0.0f)
        {
            return FALSE;
        }
        style->has_switch_thumb_size = TRUE;
    }
    else if (stdlib__strcmp(prop, "switch-thumb-color") == 0)
    {
        if (_parser_style_internal__parse_color(lx, &style->switch_thumb_color) == FALSE)
        {
            return FALSE;
        }
        style->has_switch_thumb_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "switch-thumb-off-color") == 0)
    {
        if (_parser_style_internal__parse_color(lx, &style->switch_thumb_off_color) == FALSE)
        {
            return FALSE;
        }
        style->has_switch_thumb_off_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "app-region") == 0 || stdlib__strcmp(prop, "-webkit-app-region") == 0)
    {
        // app-region: drag | no-drag (the WebKit / Electron name). A press
        // on a drag region moves its nearest absolutely positioned
        // ancestor: the title bar of a movable panel.
        char kw[16];

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(kw, "drag") != 0 && stdlib__strcmp(kw, "no-drag") != 0)
        {
            return FALSE;
        }
        style->app_region_drag = (boole)(stdlib__strcmp(kw, "drag") == 0);
    }
    else if (stdlib__strcmp(prop, "slider-track-height") == 0)
    {
        // slider-track-height: <px>. The range-input look: a thin track
        // centered in the slider's box (0 = the box itself).
        if (_parser_style_internal__parse_number(lx, &style->slider_track_h) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "slider-thumb-width") == 0)
    {
        // slider-thumb-width: <px>. A thin thumb, the box's height tall
        // (0 = the square or thumb-aspect thumb).
        if (_parser_style_internal__parse_number(lx, &style->slider_thumb_w) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "slider-value") == 0)
    {
        // slider-value: none | drag | always. drag shows the rounded value
        // above the thumb while it is held.
        char kw[16];

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(kw, "none") == 0)
        {
            style->slider_value_mode = 0;
        }
        else if (stdlib__strcmp(kw, "drag") == 0)
        {
            style->slider_value_mode = 1;
        }
        else if (stdlib__strcmp(kw, "always") == 0)
        {
            style->slider_value_mode = 2;
        }
        else
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "slider-fill") == 0)
    {
        // slider-fill: <color> of the track left of the thumb
        if (_parser_style_internal__parse_color(lx, &style->slider_fill) == FALSE)
        {
            return FALSE;
        }
        style->has_slider_fill = TRUE;
    }
    else if (stdlib__strcmp(prop, "slider-value-color") == 0)
    {
        // slider-value-color: <color> of the value the slider shows
        if (_parser_style_internal__parse_color(lx, &style->slider_value_color) == FALSE)
        {
            return FALSE;
        }
        style->has_slider_value_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "slider-value-position") == 0)
    {
        // slider-value-position: above | right | left of the thumb
        char kw[16];

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(kw, "above") == 0)
        {
            style->slider_value_pos = 0;
        }
        else if (stdlib__strcmp(kw, "right") == 0)
        {
            style->slider_value_pos = 1;
        }
        else if (stdlib__strcmp(kw, "left") == 0)
        {
            style->slider_value_pos = 2;
        }
        else
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "slider-decimals") == 0)
    {
        // slider-decimals: how many fraction digits the value shows
        float value = 0.0f;

        if (_parser_style_internal__parse_number(lx, &value) == FALSE)
        {
            return FALSE;
        }
        style->slider_decimals = (int)value;
        if (style->slider_decimals < 0)
        {
            style->slider_decimals = 0;
        }
        if (style->slider_decimals > 4)
        {
            style->slider_decimals = 4;
        }
    }
    else if (stdlib__strcmp(prop, "popup-blur") == 0)
    {
        // popup-blur: <px> the page behind an open list is blurred by
        if (_parser_style_internal__parse_number(lx, &style->popup_blur) == FALSE || style->popup_blur < 0.0f)
        {
            return FALSE;
        }
        style->popup_set |= GUI_POPUP_SET__BLUR;
    }
    else if (stdlib__strcmp(prop, "popup-dim") == 0)
    {
        // popup-dim: <color> laid over the page behind an open list
        if (_parser_style_internal__parse_color(lx, &style->popup_dim) == FALSE)
        {
            return FALSE;
        }
        style->has_popup_dim = TRUE;
    }
    else if (stdlib__strcmp(prop, "popup-align") == 0)
    {
        // popup-align: anchor (under the control) | center (of the window)
        char kw[16];

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        if (stdlib__strcmp(kw, "anchor") != 0 && stdlib__strcmp(kw, "center") != 0)
        {
            return FALSE;
        }
        style->popup_centered = (boole)(stdlib__strcmp(kw, "center") == 0);
        style->popup_set |= GUI_POPUP_SET__ALIGN;
    }
    else if (stdlib__strcmp(prop, "popup-duration") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->popup_duration_ms) || style->popup_duration_ms < 0.0f) return FALSE;
        // Durations accept bare milliseconds or the explicit ms suffix.
        if (_parser_style_internal__peek(lx) == 'm') {
            _parser_style_internal__advance(lx);
            if (_parser_style_internal__peek(lx) != 's') return FALSE;
            _parser_style_internal__advance(lx);
        }
        style->popup_set |= GUI_POPUP_SET__DURATION;
    }
    else if (stdlib__strcmp(prop, "popup-stagger") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->popup_stagger_ms) || style->popup_stagger_ms < 0.0f) return FALSE;
        // Durations accept bare milliseconds or the explicit ms suffix.
        if (_parser_style_internal__peek(lx) == 'm') {
            _parser_style_internal__advance(lx);
            if (_parser_style_internal__peek(lx) != 's') return FALSE;
            _parser_style_internal__advance(lx);
        }
        style->popup_set |= GUI_POPUP_SET__STAGGER;
    }
    else if (stdlib__strcmp(prop, "popup-offset") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->popup_offset) || style->popup_offset < 0.0f) return FALSE;
        style->popup_set |= GUI_POPUP_SET__OFFSET;
    }
    else if (stdlib__strcmp(prop, "popup-gap") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->popup_gap) || style->popup_gap < 0.0f) return FALSE;
        style->popup_set |= GUI_POPUP_SET__GAP;
    }
    else if (stdlib__strcmp(prop, "popup-width") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->popup_width) || style->popup_width < 0.0f) return FALSE;
        style->popup_set |= GUI_POPUP_SET__WIDTH;
    }
    else if (stdlib__strcmp(prop, "option-height") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->option_height) || style->option_height < 0.0f) return FALSE;
        style->popup_set |= GUI_POPUP_SET__ROW_HEIGHT;
    }
    else if (stdlib__strcmp(prop, "option-padding") == 0)
    {
        if (!_parser_style_internal__parse_number(lx, &style->option_padding) || style->option_padding < 0.0f) return FALSE;
        style->popup_set |= GUI_POPUP_SET__PADDING;
    }
    else if (stdlib__strcmp(prop, "popup-placement") == 0)
    {
        char kw[24];
        if (!_parser_style_internal__parse_ident(lx, kw, sizeof(kw))) return FALSE;
        if (stdlib__strcmp(kw, "below") == 0) style->popup_placement = GUI_POPUP_PLACEMENT__BELOW;
        else if (stdlib__strcmp(kw, "above") == 0) style->popup_placement = GUI_POPUP_PLACEMENT__ABOVE;
        else if (stdlib__strcmp(kw, "auto") == 0) style->popup_placement = GUI_POPUP_PLACEMENT__AUTO;
        else return FALSE;
        style->popup_set |= GUI_POPUP_SET__PLACEMENT;
    }
    else if (stdlib__strcmp(prop, "popup-reveal") == 0)
    {
        char kw[24];
        if (!_parser_style_internal__parse_ident(lx, kw, sizeof(kw))) return FALSE;
        if (stdlib__strcmp(kw, "fade") == 0) style->popup_reveal = GUI_POPUP_REVEAL__FADE;
        else if (stdlib__strcmp(kw, "top-down") == 0) style->popup_reveal = GUI_POPUP_REVEAL__TOP_DOWN;
        else if (stdlib__strcmp(kw, "bottom-up") == 0) style->popup_reveal = GUI_POPUP_REVEAL__BOTTOM_UP;
        else return FALSE;
        style->popup_set |= GUI_POPUP_SET__REVEAL;
    }
    else if (stdlib__strcmp(prop, "popup-background") == 0)
    {
        if (!_parser_style_internal__parse_color(lx, &style->popup_background)) return FALSE;
        style->has_popup_background = TRUE;
    }
    else if (stdlib__strcmp(prop, "select-style") == 0)
    {
        // select-style: list | wheel | sheet | page | menu | dialog | segmented | chips | carousel | search
        static const char* const names[10] = { "list", "wheel", "sheet", "page", "menu", "dialog", "segmented", "chips", "carousel", "search" };
        char kw[16];
        int i = 0;
        boole known = FALSE;

        kw[0] = 0;
        if (_parser_style_internal__parse_ident(lx, kw, (int64)sizeof(kw)) == FALSE)
        {
            return FALSE;
        }
        for (i = 0; i < 10; i++)
        {
            if (stdlib__strcmp(kw, names[i]) == 0)
            {
                style->select_style = (gui_select_style_e)i;
                known = TRUE;
            }
        }
        if (known == FALSE)
        {
            return FALSE;
        }
        style->select_style_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "select-rows") == 0)
    {
        // select-rows: <count> of options visible at the same time
        if (_parser_style_internal__parse_number(lx, &style->select_rows) == FALSE || style->select_rows < 0.0f)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "option-separator") == 0)
    {
        // option-separator: <color> of the lines a select style draws between or around its options
        if (_parser_style_internal__parse_color(lx, &style->option_separator) == FALSE)
        {
            return FALSE;
        }
        style->has_option_separator = TRUE;
    }
    else if (stdlib__strcmp(prop, "option-current-background") == 0)
    {
        if (!_parser_style_internal__parse_color(lx, &style->option_current_background)) return FALSE;
        style->has_option_current_background = TRUE;
    }
    else if (stdlib__strcmp(prop, "option-hover-background") == 0)
    {
        if (!_parser_style_internal__parse_color(lx, &style->option_hover_background)) return FALSE;
        style->has_option_hover_background = TRUE;
    }
    else if (stdlib__strcmp(prop, "option-color") == 0)
    {
        if (_parser_style_internal__parse_color(lx, &style->option_color) == FALSE)
        {
            return FALSE;
        }
        style->has_option_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "option-current-color") == 0)
    {
        if (_parser_style_internal__parse_color(lx, &style->option_current_color) == FALSE)
        {
            return FALSE;
        }
        style->has_option_current_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "option-hover-color") == 0)
    {
        if (_parser_style_internal__parse_color(lx, &style->option_hover_color) == FALSE)
        {
            return FALSE;
        }
        style->has_option_hover_color = TRUE;
    }
    else if (stdlib__strcmp(prop, "color-swatch") == 0)
    {
        // color-swatch: <px>. A colour picker draws a square of its
        // current colour of this size to the right of its strip (0, the
        // default, draws none).
        if (_parser_style_internal__parse_number(lx, &style->swatch_size) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "color-swatch-radius") == 0)
    {
        // color-swatch-radius: <px>, the swatch's corner radius
        if (_parser_style_internal__parse_number(lx, &style->swatch_radius) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "color-swatch-gap") == 0)
    {
        // color-swatch-gap: <px> between the strip and the swatch (12)
        if (_parser_style_internal__parse_number(lx, &style->swatch_gap) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "scroll-fade") == 0)
    {
        // scroll_fade: <pixels>
        // soft edge-fade region inside a scrollable container. on the
        // container rule; applies once to each direct child subtree.
        // The render walk uses the child's centre and current layout
        // to fade its opacity near the top and bottom edges. 0 (the
        // default) disables the effect entirely; ~30-60 px usually
        // reads well for body-text scrollers.
        if (_parser_style_internal__parse_number(lx, &style->scroll_fade_px) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "box-shadow") == 0)
    {
        // box-shadow: <dx> <dy> <blur> <color>
        // The four tokens in that order, separated by whitespace.
        float dx = 0.0f;
        float dy = 0.0f;
        float bl = 0.0f;
        gui_color_t sc;
        if (_parser_style_internal__parse_number(lx, &dx) == FALSE)
        {
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__parse_number(lx, &dy) == FALSE)
        {
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__parse_number(lx, &bl) == FALSE)
        {
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__parse_color(lx, &sc) == FALSE)
        {
            return FALSE;
        }
        style->shadow_dx    = dx;
        style->shadow_dy    = dy;
        style->shadow_blur  = bl;
        style->shadow_color = sc;
        style->has_shadow   = TRUE;
    }
    else if (stdlib__strcmp(prop, "opacity") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->opacity) == FALSE)
        {
            return FALSE;
        }
        style->has_opacity = TRUE;
    }
    else if (stdlib__strcmp(prop, "z-index") == 0)
    {
        float v = 0.0f;
        if (_parser_style_internal__parse_number(lx, &v) == FALSE)
        {
            return FALSE;
        }
        style->z_index = (int)v;
    }
    else if (stdlib__strcmp(prop, "blur") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->blur_px) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "background-gradient") == 0 || stdlib__strcmp(prop, "bg-gradient") == 0)
    {
        // background-gradient: linear-gradient(...) | radial-gradient(...)
        //                    | <from-color> <to-color> [vertical|horizontal|diagonal-tl|diagonal-tr]
        // The two-color form is the toolkit's original syntax (`bg-gradient`
        // its old name); gradient__two_color maps it onto the CSS geometry.
        char               fn[40];
        char               ident[32];
        int64              len = 0;
        gui_color_t        ca;
        gui_color_t        cb;
        gui_gradient_dir_e dir = GUI_GRADIENT_DIR__VERTICAL;

        stdlib__null_memory(fn, sizeof(fn));
        stdlib__null_memory(ident, sizeof(ident));
        stdlib__null_memory(&ca, sizeof(ca));
        stdlib__null_memory(&cb, sizeof(cb));
        _parser_style_internal__skip_ws_and_comments(lx);
        len = _parser_style_internal__peek_ident(lx, fn, (int64)sizeof(fn));

        if (len > 0 && _parser_style_internal__is_gradient_fn(fn) == TRUE)
        {
            _parser_style_internal__advance_n(lx, len);

            if (_parser_style_internal__parse_gradient_fn(lx, fn, &style->bg_gradient) == FALSE)
            {
                return FALSE;
            }

            style->has_bg_gradient = TRUE;
        }
        else
        {
            if (_parser_style_internal__parse_color(lx, &ca) == FALSE)
            {
                return FALSE;
            }

            _parser_style_internal__skip_ws_and_comments(lx);

            if (_parser_style_internal__parse_color(lx, &cb) == FALSE)
            {
                return FALSE;
            }

            _parser_style_internal__skip_ws_and_comments(lx);

            if (_parser_style_internal__is_alpha(_parser_style_internal__peek(lx)) == TRUE)
            {
                if (_parser_style_internal__parse_ident(lx, ident, (int64)sizeof(ident)) == FALSE)
                {
                    return FALSE;
                }

                if (_parser_style_internal__gradient_dir_keyword(ident, &dir) == FALSE)
                {
                    log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown background-gradient direction '", ident, "', using vertical");
                }
            }

            gradient__two_color(&style->bg_gradient, ca, cb, dir);
            style->has_bg_gradient = TRUE;
        }
    }
    else if (stdlib__strcmp(prop, "columns") == 0)
    {
        float v = 0.0f;
        if (_parser_style_internal__parse_number(lx, &v) == FALSE)
        {
            return FALSE;
        }
        style->collection_columns = (int)v;
    }
    else if (stdlib__strcmp(prop, "item-width") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->item_width) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "item-height") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->item_height) == FALSE)
        {
            return FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "layout") == 0)
    {
        char ident[16];
        if (_parser_style_internal__parse_ident(lx, ident, sizeof(ident)) == FALSE)
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "grid") == 0)
        {
            style->collection_layout = GUI_COLLECTION_LAYOUT__GRID;
        }
        else if (stdlib__strcmp(ident, "list") == 0)
        {
            style->collection_layout = GUI_COLLECTION_LAYOUT__LIST;
        }
        else if (stdlib__strcmp(ident, "flow") == 0)
        {
            style->collection_layout = GUI_COLLECTION_LAYOUT__FLOW;
        }
        else
        {
            log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown layout '", ident, "' (grid|list|flow)");
        }
    }
    else if (stdlib__strcmp(prop, "width") == 0 || stdlib__strcmp(prop, "height") == 0)
    {
        // CSS-ish dimension. Accepts:
        // width: 200;    (bare number -> absolute pixels, like size_w)
        // width: 200px;  (same, explicit px suffix)
        // width: 100%;   (percent of parent's inner content area)
        float v = 0.0f;
        if (_parser_style_internal__parse_number(lx, &v) == FALSE)
        {
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        // Peek the suffix. `%` sets the percent slot; `px` (or no
        // suffix) sets the pixel slot. Unknown suffixes fall through
        // to pixels with a warning so typos don't break the rest of
        // the block.
        char c = _parser_style_internal__peek(lx);
        boole is_percent = FALSE;
        if (c == '%')
        {
            is_percent = TRUE;
            _parser_style_internal__advance(lx);
        }
        else if (c == 'p')
        {
            // Consume "px" if present; otherwise leave the char for
            // the next token (probably ';').
            _parser_style_internal__advance(lx);
            if (_parser_style_internal__peek(lx) == 'x')
            {
                _parser_style_internal__advance(lx);
            }
        }
        if (stdlib__strcmp(prop, "width") == 0)
        {
            if (is_percent)
            {
                style->width_pct = v;
                style->size_w = 0.0f;
                style->size_w_explicit = FALSE;
            }
            else
            {
                style->size_w    = v;
                style->width_pct = 0.0f;
                style->size_w_explicit = TRUE;
            }
        }
        else
        {
            if (is_percent)
            {
                style->height_pct = v;
                style->size_h = 0.0f;
                style->size_h_explicit = FALSE;
            }
            else
            {
                style->size_h     = v;
                style->height_pct = 0.0f;
                style->size_h_explicit = TRUE;
            }
        }
    }
    else if (stdlib__strcmp(prop, "background") == 0)
    {
        // background: the CSS shorthand, layers separated by commas. A
        // layer is a color, linear-gradient(...) / radial-gradient(...)
        // or url(<path>); the color goes under the others (CSS paints it
        // as the bottom layer). One gradient layer and one image layer are
        // kept: a further gradient is parsed and dropped with a warning.
        // `none` sets nothing.
        char           fn[40];
        char           path[128];
        int64          len       = 0;
        int64          copy_n    = 0;
        boole          grad_seen = FALSE;
        gui_gradient_t dropped;
        gui_color_t    c;

        stdlib__null_memory(fn, sizeof(fn));
        stdlib__null_memory(path, sizeof(path));
        stdlib__null_memory(&dropped, sizeof(dropped));
        stdlib__null_memory(&c, sizeof(c));

        while (TRUE)
        {
            _parser_style_internal__skip_ws_and_comments(lx);
            len = _parser_style_internal__peek_ident(lx, fn, (int64)sizeof(fn));

            if (len > 0 && _parser_style_internal__is_gradient_fn(fn) == TRUE)
            {
                _parser_style_internal__advance_n(lx, len);

                if (grad_seen == TRUE)
                {
                    log_warn("%s %s %lld %s", lx->filename, ":", (long long)lx->line, ": background: only one gradient layer is drawn, the further ones are ignored");

                    if (_parser_style_internal__parse_gradient_fn(lx, fn, &dropped) == FALSE)
                    {
                        return FALSE;
                    }
                }
                else
                {
                    if (_parser_style_internal__parse_gradient_fn(lx, fn, &style->bg_gradient) == FALSE)
                    {
                        return FALSE;
                    }

                    style->has_bg_gradient = TRUE;
                    grad_seen              = TRUE;
                }
            }
            else if (len > 0 && stdlib__strcmp(fn, "url") == 0)
            {
                _parser_style_internal__advance_n(lx, len);

                if (_parser_style_internal__parse_url_path(lx, path, (int64)sizeof(path)) == FALSE)
                {
                    return FALSE;
                }

                copy_n = stdlib__strlen(path);

                if (copy_n >= (int64)sizeof(style->background_image))
                {
                    copy_n = (int64)sizeof(style->background_image) - 1;
                }

                stdlib__memcpy(style->background_image, path, (size_t)copy_n);
                style->background_image[copy_n] = 0;
                style->has_background_image     = TRUE;
            }
            else if (len > 0 && stdlib__strcmp(fn, "none") == 0)
            {
                _parser_style_internal__advance_n(lx, len);
            }
            else
            {
                if (_parser_style_internal__parse_color(lx, &c) == FALSE)
                {
                    return FALSE;
                }

                style->background_color     = c;
                style->has_background_color = TRUE;
            }

            _parser_style_internal__skip_ws_and_comments(lx);

            if (_parser_style_internal__peek(lx) == ',')
            {
                _parser_style_internal__advance(lx);
                continue;
            }

            break;
        }
    }
    else if (stdlib__strcmp(prop, "background-image") == 0)
    {
        // background-image: linear-gradient(...) | radial-gradient(...)
        //                 | url(<path>) | <path-or-string>
        // A gradient goes to the gradient slot (the CSS image layer, painted
        // over background-color); a path keeps the flexible bare / quoted
        // parsing of font-family.
        char  buf[128];
        char  fn[40];
        int64 len    = 0;
        int64 copy_n = 0;

        stdlib__null_memory(buf, sizeof(buf));
        stdlib__null_memory(fn, sizeof(fn));
        _parser_style_internal__skip_ws_and_comments(lx);
        len = _parser_style_internal__peek_ident(lx, fn, (int64)sizeof(fn));

        if (len > 0 && _parser_style_internal__is_gradient_fn(fn) == TRUE)
        {
            _parser_style_internal__advance_n(lx, len);

            if (_parser_style_internal__parse_gradient_fn(lx, fn, &style->bg_gradient) == FALSE)
            {
                return FALSE;
            }

            style->has_bg_gradient = TRUE;
        }
        else
        {
            if (len > 0 && stdlib__strcmp(fn, "url") == 0)
            {
                _parser_style_internal__advance_n(lx, len);

                if (_parser_style_internal__parse_url_path(lx, buf, (int64)sizeof(buf)) == FALSE)
                {
                    return FALSE;
                }
            }
            else if (_parser_style_internal__parse_path_value(lx, buf, (int64)sizeof(buf)) == FALSE)
            {
                return FALSE;
            }

            copy_n = stdlib__strlen(buf);

            if (copy_n >= (int64)sizeof(style->background_image))
            {
                copy_n = (int64)sizeof(style->background_image) - 1;
            }

            stdlib__memcpy(style->background_image, buf, (size_t)copy_n);
            style->background_image[copy_n] = 0;
            style->has_background_image     = TRUE;
        }
    }
    else if (stdlib__strcmp(prop, "background-size") == 0 || stdlib__strcmp(prop, "object-fit") == 0)
    {
        // Shared fit keyword set. `stretch` is accepted as an alias
        // for `fill` because users trained on HTML/CSS tend to say
        // "stretch" colloquially even though CSS spells it `fill`.
        char ident[16];
        if (_parser_style_internal__parse_ident(lx, ident, sizeof(ident)) == FALSE)
        {
            return FALSE;
        }
        gui_image_fit_e fit = GUI_IMAGE_FIT__FILL;
        if      (stdlib__strcmp(ident, "fill")    == 0)
        {
            fit = GUI_IMAGE_FIT__FILL;
        }
        else if (stdlib__strcmp(ident, "stretch") == 0)
        {
            fit = GUI_IMAGE_FIT__FILL;
        }
        else if (stdlib__strcmp(ident, "contain") == 0)
        {
            fit = GUI_IMAGE_FIT__CONTAIN;
        }
        else if (stdlib__strcmp(ident, "cover")   == 0)
        {
            fit = GUI_IMAGE_FIT__COVER;
        }
        else if (stdlib__strcmp(ident, "none")    == 0)
        {
            fit = GUI_IMAGE_FIT__NONE;
        }
        else
        {
            log_warn("%s %s %lld %s %s %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown", prop, "value '", ident, "' (fill|stretch|contain|cover|none)");
        }
        if (stdlib__strcmp(prop, "object-fit") == 0)
        {
            style->object_fit = fit;
        }
        else
        {
            style->background_size = fit;
        }
    }
    else if (stdlib__strcmp(prop, "horizontal-alignment") == 0 || stdlib__strcmp(prop, "halign") == 0)
    {
        // `halign` is a short alias for `horizontal-alignment`.
        // Added for ergonomics -- long property name was tripping
        // meek-shell contributors who'd seen `style->halign` in the
        // C code and reached for the obvious short form.
        char ident[16];
        if (_parser_style_internal__parse_ident(lx, ident, sizeof(ident)) == FALSE)
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "left")   == 0)
        {
            style->halign = GUI_HALIGN__LEFT;
        }
        else if (stdlib__strcmp(ident, "center") == 0)
        {
            style->halign = GUI_HALIGN__CENTER;
        }
        else if (stdlib__strcmp(ident, "right")  == 0)
        {
            style->halign = GUI_HALIGN__RIGHT;
        }
        else
        {
            log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown horizontal-alignment '", ident, "' (left|center|right)");
        }
    }
    else if (stdlib__strcmp(prop, "vertical-alignment") == 0 || stdlib__strcmp(prop, "valign") == 0)
    {
        char ident[16];
        if (_parser_style_internal__parse_ident(lx, ident, sizeof(ident)) == FALSE)
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "top")    == 0)
        {
            style->valign = GUI_VALIGN__TOP;
        }
        else if (stdlib__strcmp(ident, "center") == 0)
        {
            style->valign = GUI_VALIGN__CENTER;
        }
        else if (stdlib__strcmp(ident, "bottom") == 0)
        {
            style->valign = GUI_VALIGN__BOTTOM;
        }
        else
        {
            log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown vertical-alignment '", ident, "' (top|center|bottom)");
        }
    }
    else if (stdlib__strcmp(prop, "border-width") == 0)
    {
        if (_parser_style_internal__parse_number(lx, &style->border_width) == FALSE)
        {
            return FALSE;
        }
        style->border_width_set = TRUE;
    }
    else if (stdlib__strcmp(prop, "border-color") == 0)
    {
        // border-color: <color>
        //             | linear-gradient(...) | radial-gradient(...)   (CSS gradient)
        //             | gradient(<from>, <to> [, direction])        (the toolkit's two-color form)
        // A gradient fills border_gradient and the emitter paints the outer
        // border rect with it; border_color mirrors its first color so a
        // reader without gradient awareness still gets a solid line.
        char  fn[40];
        int64 len = 0;

        stdlib__null_memory(fn, sizeof(fn));
        _parser_style_internal__skip_ws_and_comments(lx);
        len = _parser_style_internal__peek_ident(lx, fn, (int64)sizeof(fn));

        if (len > 0 && (_parser_style_internal__is_gradient_fn(fn) == TRUE || stdlib__strcmp(fn, "gradient") == 0))
        {
            _parser_style_internal__advance_n(lx, len);

            if (stdlib__strcmp(fn, "gradient") == 0)
            {
                if (_parser_style_internal__parse_legacy_gradient_fn(lx, &style->border_gradient) == FALSE)
                {
                    return FALSE;
                }
            }
            else if (_parser_style_internal__parse_gradient_fn(lx, fn, &style->border_gradient) == FALSE)
            {
                return FALSE;
            }

            style->has_border_gradient = TRUE;
            style->border_color        = style->border_gradient.stops[0].color;
            style->has_border_color    = TRUE;
        }
        else
        {
            if (_parser_style_internal__parse_color(lx, &style->border_color) == FALSE)
            {
                return FALSE;
            }

            style->has_border_color = TRUE;
            style->has_border_gradient = FALSE;
        }
    }
    else if (stdlib__strcmp(prop, "border-style") == 0)
    {
        char ident[16];
        if (_parser_style_internal__parse_ident(lx, ident, sizeof(ident)) == FALSE)
        {
            return FALSE;
        }
        if      (stdlib__strcmp(ident, "none")   == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__NONE;
        }
        else if (stdlib__strcmp(ident, "solid")  == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__SOLID;
        }
        else if (stdlib__strcmp(ident, "dashed") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__DASHED;
        }
        else if (stdlib__strcmp(ident, "dotted") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__DOTTED;
        }
        else if (stdlib__strcmp(ident, "double") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__DOUBLE;
        }
        else if (stdlib__strcmp(ident, "groove") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__GROOVE;
        }
        else if (stdlib__strcmp(ident, "ridge") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__RIDGE;
        }
        else if (stdlib__strcmp(ident, "inset") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__INSET;
        }
        else if (stdlib__strcmp(ident, "outset") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__OUTSET;
        }
        else if (stdlib__strcmp(ident, "hidden") == 0)
        {
            style->border_style_set = TRUE;
            style->border_style = GUI_BORDER_STYLE__HIDDEN;
        }
        else
        {
            log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown border-style '", ident, "' (none|hidden|solid|dashed|dotted|double|groove|ridge|inset|outset)");
        }
    }
    else if (stdlib__strcmp(prop, "border") == 0)
    {
        // CSS shorthand: `border: 2px solid #fff;` / `border: solid
        // 1 #ff0;` / etc. Tokens are space-separated; we classify
        // each by its leading character:
        // '#'         -> color
        // digit / '.' -> width (with optional 'px' suffix)
        // letter      -> style keyword (solid|dashed|dotted|none)
        // Order is irrelevant. Missing components leave the matching
        // field at its current value (no implicit zeroing). The
        // typical usage stays canonical "<width> <style> <color>".
        for (int slot = 0; slot < 3; slot++)
        {
            _parser_style_internal__skip_ws_and_comments(lx);
            char c = _parser_style_internal__peek(lx);
            if (c == ';' || c == '}' || c == 0)
            {
                break;
            }

            if (c == '#')
            {
                gui_color_t col;
                if (_parser_style_internal__parse_color(lx, &col) == FALSE)
                {
                    return FALSE;
                }
                style->border_color     = col;
                style->has_border_color = TRUE;
            }
            else if ((c >= '0' && c <= '9') || c == '.')
            {
                if (_parser_style_internal__parse_number(lx, &style->border_width) == FALSE)
                {
                    return FALSE;
                }
                style->border_width_set = TRUE;
                // Accept and discard a trailing "px" so `2px` parses
                // the same as `2`. CSS-natural; matches existing
                // width / height parsing in this file.
                if (_parser_style_internal__peek(lx) == 'p')
                {
                    _parser_style_internal__advance(lx);
                    if (_parser_style_internal__peek(lx) == 'x')
                    {
                        _parser_style_internal__advance(lx);
                    }
                }
            }
            else
            {
                char ident[16];
                if (_parser_style_internal__parse_ident(lx, ident, sizeof(ident)) == FALSE)
                {
                    return FALSE;
                }
                if      (stdlib__strcmp(ident, "none")   == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__NONE;
                }
                else if (stdlib__strcmp(ident, "solid")  == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__SOLID;
                }
                else if (stdlib__strcmp(ident, "dashed") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__DASHED;
                }
                else if (stdlib__strcmp(ident, "dotted") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__DOTTED;
                }
                else if (stdlib__strcmp(ident, "double") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__DOUBLE;
                }
                else if (stdlib__strcmp(ident, "groove") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__GROOVE;
                }
                else if (stdlib__strcmp(ident, "ridge") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__RIDGE;
                }
                else if (stdlib__strcmp(ident, "inset") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__INSET;
                }
                else if (stdlib__strcmp(ident, "outset") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__OUTSET;
                }
                else if (stdlib__strcmp(ident, "hidden") == 0)
                {
                    style->border_style_set = TRUE;
                    style->border_style = GUI_BORDER_STYLE__HIDDEN;
                }
                else
                {
                    log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown border style keyword '", ident, "' in shorthand (none|hidden|solid|dashed|dotted|double|groove|ridge|inset|outset)");
                }
            }
        }
    }
    else
    {
        // Unknown property. Warn, then skip tokens until the next ';'
        // or '}' so the rest of the block still parses. This is the
        // forgiving path -- a typo like `radiuz: 6;` triggers one
        // warning but doesn't invalidate the whole file.
        known = FALSE;
        log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown property '", prop, "' (skipping)");
        while (_parser_style_internal__eof(lx) == FALSE)
        {
            char c = _parser_style_internal__peek(lx);
            if (c == ';' || c == '}')
            {
                break;
            }
            _parser_style_internal__advance(lx);
        }
    }

    // ----- mandatory ';' ---------------------------------------------------
    // Every declaration MUST end with ';' -- no CSS-style "last
    // declaration can omit the semicolon". This keeps the parser
    // deterministic (we can always require ';' before going back to
    // the block loop).
    _parser_style_internal__skip_ws_and_comments(lx);
    // an optional `!important` before the ';': the block parser already
    // decided from a lookahead which rule the declaration goes into,
    // here the token is only consumed
    if (_parser_style_internal__peek(lx) == '!')
    {
        char flag[16];
        stdlib__null_memory(flag, sizeof(flag));
        _parser_style_internal__advance(lx);
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__parse_ident(lx, flag, (int64)sizeof(flag)) == FALSE || stdlib__strcmp(flag, "important") != 0)
        {
            _parser_style_internal__error(lx, "expected 'important' after '!'");
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);
    }
    if (_parser_style_internal__peek(lx) != ';')
    {
        _parser_style_internal__error(lx, "expected ';' at end of declaration");
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    // `known` is (void)'d because it's only used for its side effect
    // (logging the warning); nothing consumes its value. Keeping the
    // variable makes the flow clearer, and gives future code a hook
    // if we ever want to stop processing the block after an unknown
    // property.
    (void)known;
    return TRUE;
}

// ===========================================================================
// Block parser -- declarations + nested pseudo-state rules.
// ===========================================================================

// Parse the body of a `{ ... }` block. A block is a mix of:
// - Declarations (name: value;) that accumulate into one gui_style_t
// which gets registered at block-end under `base_selector`.
// - Nested pseudo-state rules (`:hover { ... }`) that each build
// their own "base:pseudo" selector and register a separate style.
// Nested pseudo-state rules recurse back through this same function
// with the composed selector. This mirrors the CSS "&:hover" nesting
// syntax but without needing the '&' prefix -- ':hover' on its own
// is unambiguously a pseudo-state when it appears inside a block.
// A block that contains ONLY nested rules (no own declarations) does
// NOT register a style for `base_selector` itself -- the has_decls
// flag guards that. This is why you can write:
// Button {
// :hover { bg: #555; }
// :pressed { bg: #222; }
// }
// ...without producing a "Button { }" rule that would write zeros
// over nothing and waste a table slot.
static void _parser_style_internal__parse_block(_parser_style_internal__lexer_t* lx, char* base_selector)
{
    // Require an opening '{'.
    if (_parser_style_internal__peek(lx) != '{')
    {
        _parser_style_internal__error(lx, "expected '{'");
        return;
    }
    _parser_style_internal__advance(lx);

    // Fresh style object for this block's declarations to fill in.
    // zero-initialized so every has_* flag starts FALSE.
    gui_style_t style;
    gui_style_t important;
    boole       has_decls     = FALSE;
    boole       has_important = FALSE;
    stdlib__memset(&style, 0, sizeof(style));
    // declarations ending in `!important` collect in a second style
    // that is registered as a rule the resolver applies after every
    // normal rule, which is what !important means in CSS
    stdlib__memset(&important, 0, sizeof(important));

    // Main block loop. Each iteration decides what kind of element
    // we're looking at based on the first non-whitespace character.
    while (lx->ok)
    {
        _parser_style_internal__skip_ws_and_comments(lx);
        char c = _parser_style_internal__peek(lx);

        // End of block -- consume '}' and exit.
        if (c == '}')
        {
            _parser_style_internal__advance(lx);
            break;
        }
        // Premature EOF. Log and return; the top-level parse loop
        // will observe lx->ok == FALSE and stop.
        if (c == 0)
        {
            _parser_style_internal__error(lx, "unexpected end of file inside block");
            return;
        }

        // ----- Nested pseudo-state rule ----------------------------
        // `:hover { ... }` (or `:pressed`, `:disabled`, `:default`)
        // inside a block means "style the parent selector's nodes
        // when they're in this state". We compose the full selector
        // as "parent:pseudo" and recurse -- parse_block will register
        // a new rule with that selector + its own gui_style_t.
        if (c == ':')
        {
            _parser_style_internal__advance(lx);
            char pseudo[32];
            if (_parser_style_internal__parse_ident(lx, pseudo, (int64)sizeof(pseudo)) == FALSE)
            {
                return;
            }

            // Build "base:pseudo" into a 160-byte buffer. If we
            // overflow (very long id + long pseudo -- unlikely but
            // checked), bail with an error.
            char nested[160];
            int64 written = 0;
            stdlib__strcpy_safe(nested, (int64)sizeof(nested), base_selector);
            stdlib__strcat_safe(nested, (int64)sizeof(nested), ":");
            written = stdlib__strcat_safe(nested, (int64)sizeof(nested), pseudo);
            if (written >= (int64)sizeof(nested))
            {
                _parser_style_internal__error(lx, "nested selector too long");
                return;
            }

            _parser_style_internal__skip_ws_and_comments(lx);
            _parser_style_internal__parse_block(lx, nested); // recurse.
            continue;
        }

        // ----- Ordinary declaration: name: value; ------------------
        if (_parser_style_internal__decl_is_important(lx))
        {
            if (_parser_style_internal__parse_decl(lx, &important) == FALSE)
            {
                return;
            }
            has_important = TRUE;
            continue;
        }
        if (_parser_style_internal__parse_decl(lx, &style) == FALSE)
        {
            return;
        }
        has_decls = TRUE;
    }

    // Register the accumulated style under `base_selector`, but only
    // if the block contained at least one declaration. Blocks that
    // are only-nested-rules don't create a zero rule for their parent.
    // Empty base_selector is defensive: shouldn't happen (parse_rule
    // fails if the selector is empty), but if it did we'd register
    // under "" which would match nothing.
    if (has_decls && base_selector[0] != 0)
    {
        // Border partial-declaration check. The renderer's three-way
        // gate in scene__emit_border requires width > 0 AND style
        // != none AND has_border_color -- partial declarations
        // produce no visible border, which surfaces as "I set
        // border-width: 2px and nothing happened". Logging here
        // catches the common case at parse time so the dev sees
        // the warning instead of debugging silent no-paint.
        boole has_w = (style.border_width  > 0.0f);
        boole has_s = (style.border_style != GUI_BORDER_STYLE__NONE);
        boole has_c = style.has_border_color;
        if ((has_w || has_s || has_c) && (has_w && has_s && has_c) == FALSE)
        {
            log_warn("%s %s %lld %s %s %s %s %s %s %s %s %s", lx->filename, ":", (long long)lx->line, ": '", base_selector, "' has partial border declaration (width=", has_w ? "yes" : "NO", "style=", has_s ? "yes" : "NO", "color=", has_c ? "yes" : "NO", "); border won't paint until all three are set");
        }
        if (scene__add_style(base_selector, &style, FALSE) == 0)
        {
            _parser_style_internal__error(lx, "style registration failed (invalid selector or table full)");
            return;
        }
    }

    if (has_important && base_selector[0] != 0)
    {
        if (scene__add_style(base_selector, &important, TRUE) == 0)
        {
            _parser_style_internal__error(lx, "important style registration failed (invalid selector or table full)");
        }
    }
}

// Parse a top-level rule: `selector [, selector2, ...] { block }`.
// Called from the stylesheet loop in parser_style__load_styles.
// Comma-separated selectors are supported: the block is applied to
// every listed selector, equivalent to copy-pasting the block under
// each one. Implementation: we collect the selector list first, then
// snapshot the lexer position at the opening of the block and
// re-parse the same block once per selector (rewinding in between).
// Re-parsing costs a few microseconds per extra selector and keeps
// parse_block's internals (declaration dispatch, nested pseudo-state
// rules, error handling) unchanged.
// Spaces inside each selector are retained for descendant matching.
#define _PARSER_STYLE_INTERNAL_MAX_SELECTOR_LIST 16

static void _parser_style_internal__parse_rule(_parser_style_internal__lexer_t* lx)
{
    char selectors[_PARSER_STYLE_INTERNAL_MAX_SELECTOR_LIST][160];
    int64 n_selectors = 0;

    // Read first selector (mandatory).
    if (_parser_style_internal__parse_selector(lx, selectors[0], (int64)sizeof(selectors[0])) == FALSE)
    {
        return;
    }
    n_selectors = 1;

    // Optionally read `, selector2, selector3, ...`. Each iteration
    // consumes the comma + surrounding whitespace, then one more
    // selector. Stops when the next non-ws char isn't ','.
    _parser_style_internal__skip_ws_and_comments(lx);
    while (_parser_style_internal__peek(lx) == ',')
    {
        _parser_style_internal__advance(lx); // consume ','
        _parser_style_internal__skip_ws_and_comments(lx);

        if (n_selectors >= _PARSER_STYLE_INTERNAL_MAX_SELECTOR_LIST)
        {
            _parser_style_internal__error(lx, "too many comma-separated selectors");
            return;
        }
        if (_parser_style_internal__parse_selector(lx, selectors[n_selectors], (int64)sizeof(selectors[n_selectors])) == FALSE)
        {
            return;
        }
        n_selectors++;
        _parser_style_internal__skip_ws_and_comments(lx);
    }

    // At this point the lexer is positioned at the opening '{' of
    // the block (or some garbage that parse_block will reject).
    // Snapshot pos + line so we can rewind per selector. `ok` is NOT
    // saved -- if the block fails for one selector, bailing out is
    // the right behavior; don't mask errors by retrying.
    int64 block_pos  = lx->pos;
    int64 block_line = lx->line;

    for (int64 i = 0; i < n_selectors; ++i)
    {
        // Rewind for every iteration (including the first, even
        // though it's a no-op there, for symmetry/clarity).
        lx->pos  = block_pos;
        lx->line = block_line;
        _parser_style_internal__parse_block(lx, selectors[i]);
        if (lx->ok == FALSE)
        {
            return;
        }
    }

    // Lexer is now past the block's closing '}' (from the last
    // iteration). Exactly where the single-selector path would have
    // left it -- the stylesheet loop picks up from here.
}

/**
 * @brief parse two-color CSS color-mix with optional percentages
 * @param _parser_style_internal__lexer_t* lx -> positioned after color-mix
 * @param gui_color_t* out -> mixed color
 * @return boole -> TRUE for supported interpolation spaces and valid weights
 */
static boole _parser_style_internal__parse_color_mix(_parser_style_internal__lexer_t* lx, gui_color_t* out)
{
    gui_color_t colors[2];
    float weights[2];
    char space[32];
    char keyword[8];
    boole percent = FALSE;
    int i = 0;
    char c = 0;

    stdlib__null_memory(colors, sizeof(colors));
    stdlib__null_memory(weights, sizeof(weights));
    stdlib__null_memory(space, sizeof(space));
    stdlib__null_memory(keyword, sizeof(keyword));
    weights[0] = -1.0f;
    weights[1] = -1.0f;
    _parser_style_internal__skip_ws_and_comments(lx);
    if (_parser_style_internal__peek(lx) != '(')
    {
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    _parser_style_internal__skip_ws_and_comments(lx);
    if (_parser_style_internal__parse_ident(lx, keyword, sizeof(keyword)) == FALSE || stdlib__strcmp(keyword, "in") != 0)
    {
        return FALSE;
    }
    _parser_style_internal__skip_ws_and_comments(lx);
    if (_parser_style_internal__parse_ident(lx, space, sizeof(space)) == FALSE)
    {
        return FALSE;
    }
    if (stdlib__strcmp(space, "srgb") != 0 && stdlib__strcmp(space, "srgb-linear") != 0 && stdlib__strcmp(space, "oklab") != 0)
    {
        return FALSE;
    }
    for (i = 0; i < 2; i++)
    {
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__peek(lx) != ',')
        {
            return FALSE;
        }
        _parser_style_internal__advance(lx);
        _parser_style_internal__skip_ws_and_comments(lx);
        if (_parser_style_internal__parse_color(lx, &colors[i]) == FALSE)
        {
            return FALSE;
        }
        _parser_style_internal__skip_ws_and_comments(lx);
        c = _parser_style_internal__peek(lx);
        if ((c >= '0' && c <= '9') || c == '.')
        {
            if (_parser_style_internal__parse_length(lx, &weights[i], &percent) == FALSE || percent == FALSE || weights[i] > 100.0f)
            {
                return FALSE;
            }
        }
    }
    _parser_style_internal__skip_ws_and_comments(lx);
    if (_parser_style_internal__peek(lx) != ')')
    {
        return FALSE;
    }
    _parser_style_internal__advance(lx);
    if (weights[0] < 0.0f && weights[1] < 0.0f)
    {
        weights[0] = 50.0f;
        weights[1] = 50.0f;
    }
    else if (weights[0] < 0.0f)
    {
        weights[0] = 100.0f - weights[1];
    }
    else if (weights[1] < 0.0f)
    {
        weights[1] = 100.0f - weights[0];
    }
    if (weights[0] + weights[1] <= 0.0f)
    {
        return FALSE;
    }
    *out = color_mix__mix(colors[0], colors[1], weights[0] / 100.0f, weights[1] / 100.0f, space);
    return TRUE;
}
