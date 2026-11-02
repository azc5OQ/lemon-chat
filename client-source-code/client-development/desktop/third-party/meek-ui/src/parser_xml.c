// ===========================================================================
// parser_xml.c - minimal XML parser for the toolkit's .ui files.
// ===========================================================================
// WHAT THIS PARSER IS
// -------------------
// A hand-written recursive-descent parser that reads a tiny subset of XML
// and builds a tree of gui_node_t objects. Each XML tag becomes one node;
// attributes get applied to the node either by the parser itself (for
// generic attributes like id / class / on_click) or by the node's widget
// vtable (for widget-specific knobs like a slider's min/max/value).
// The subset we accept is deliberately small (see session/DESIGN.md):
// - Tags with attributes, optionally self-closing:
// <Button text="OK" on_click="do_it" />
// <Column id="main"> ...children... </Column>
// - Children: nested tags and optional text (text is currently dropped;
// Text content lives on the <Text> widget via the text="" attribute).
// - XML comments <!-- ... -->
// - Exactly four entity references: &lt; &gt; &amp; &quot;
// Explicitly NOT supported (intentionally, to keep parsing trivial):
// - XML namespaces              (x:foo, xmlns=...)
// - DOCTYPE declarations        (<!DOCTYPE ...>)
// - Processing instructions     (<?xml ... ?>)
// - CDATA sections              (<![CDATA[...]]>)
// - Numeric character refs      (&#65; / &#xA0;)
// WHY HAND-WRITTEN
// ----------------
// The grammar is so small that pulling in a library (expat, libxml2) is
// more code than the parser itself. A hand-written recursive-descent
// parser here is also trivially portable (no malloc beyond the node
// allocations, no globals, no third-party headers) and keeps the dev
// feedback loop tight -- we can evolve the grammar as features land.
// ERROR STRATEGY
// --------------
// We use a latched "ok" flag instead of setjmp/longjmp or manual error
// propagation at every call site. The first error logs a message
// ("filename:line: message"), flips lx.ok to FALSE, and all downstream
// functions become no-ops because they check lx.ok (indirectly, via the
// helpers they call). At the top level, if ok is FALSE after parsing,
// we free whatever partial tree was built and return NULL.
// I/O
// ---
// fs__read_entire_file handles the platform-specific file-read (CreateFileW
// on Windows, open/read on POSIX). parser_xml never sees a FILE* or
// a syscall -- it works on a null-terminated char buffer the caller has
// already slurped into memory. This keeps the parser equally at home
// in memory-mapped contexts (hot reload, APK assets, test fixtures).

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "scene.h"
#include "fs.h"
#include "parser_xml.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

/**
 * Lexer state for one parse run.
 *
 * All public-api entry points build one of these on the stack and pass
 * its address to every helper. It holds the input buffer, where we are
 * inside it, the line number (for error messages), the filename (also
 * for error messages), and an "ok" flag that latches FALSE on the
 * first error so downstream code can short-circuit.
 */
typedef struct _parser_xml_internal__lexer_t
{
    // input buffer (null-terminated copy of the file).
    char* src;        // 0x0
    // length in bytes (excluding the null terminator).
    int64 len;        // 0x8
    // current parse position (0..len).
    int64 pos;        // 0x10
    // current line number, 1-based, for error messages.
    int64 line;       // 0x18
    // path echoed in error messages; not owned.
    char* filename;   // 0x20
    // latched FALSE as soon as any error is reported.
    boole ok;         // 0x28
} _parser_xml_internal__lexer_t;

// ===========================================================================
// Forward declarations.
// ===========================================================================
// Every helper is file-local (static, double-underscore prefix per the
// project's C style guide). The public api exposes exactly one function
// (parser_xml__load_ui); everything else is internal machinery.
static void          _parser_xml_internal__error(_parser_xml_internal__lexer_t* lx, char* msg);
static char          _parser_xml_internal__peek(_parser_xml_internal__lexer_t* lx);
static char          _parser_xml_internal__advance(_parser_xml_internal__lexer_t* lx);
static boole         _parser_xml_internal__eof(_parser_xml_internal__lexer_t* lx);
static void          _parser_xml_internal__skip_whitespace(_parser_xml_internal__lexer_t* lx);
static boole         _parser_xml_internal__starts_with(_parser_xml_internal__lexer_t* lx, char* s);
static boole         _parser_xml_internal__skip_comment(_parser_xml_internal__lexer_t* lx);
static boole         _parser_xml_internal__is_name_char(char c, boole first);
static boole         _parser_xml_internal__parse_name(_parser_xml_internal__lexer_t* lx, char* out, int64 cap);
static boole         _parser_xml_internal__parse_attr_value(_parser_xml_internal__lexer_t* lx, char* out, int64 cap, int64* length);
static gui_node_t*     _parser_xml_internal__parse_element(_parser_xml_internal__lexer_t* lx);
static boole         _parser_xml_internal__validate_id(char* value, _parser_xml_internal__lexer_t* lx);
static boole         _parser_xml_internal__validate_class(char* value, _parser_xml_internal__lexer_t* lx);

// ===========================================================================
// Public API.
// ===========================================================================

// The only entry point. Reads the file, parses, returns the tree root.
// Caller owns the returned tree and must call scene__node_free on it
// when done. On any failure (I/O, parse error) logs to stderr via log.c
// and returns NULL after cleaning up any partial tree.
// Directory portion of the most recently loaded .ui file, trailing
// separator included (so widgets can just concatenate with the raw
// src value). Updated at the top of parser_xml__load_ui; exposed via
// parser_xml__base_dir. Plain file-static -- single parser, no
// re-entrancy, no thread-safety required.
static char _parser_xml_internal__base_dir[512] = "";

/**
 * @brief the directory portion (including trailing slash or backslash) of the path most recently passed to parser_xml__load_ui. Widgets that accept path-valued attributes (e.g. `<image src="foo.png"/>`) use this as the base to resolve relative paths against, so a .ui file can reference sibling assets without hardcoding a platform-specific prefix. Empty string if no .ui has been loaded yet.
 *
 * @return const char* -> Read-only pointer; valid until the next parser_xml__load_ui call.
 */
const char* parser_xml__base_dir(void)
{
    return _parser_xml_internal__base_dir;
}

/**
 * @brief reads the file at the given path (UTF-8) and parses it into a tree.
 *
 * @param char* path -> Path to the file to read.
 *
 * @return gui_node_t* -> Parsed tree root node, or NULL on I/O or parse failure. Errors are logged to stderr on failure. The caller becomes the owner of the returned tree and must call scene__node_free.
 */
gui_node_t* parser_xml__load_ui(char* path)
{
    // Step 0: remember the directory portion of this path so widgets
    // loading sibling assets (e.g. <image src="gradient.png"/>) can
    // resolve the name relative to the .ui file. Split at the last
    // '/' or '\'; if there's no separator the source lives at the
    // CWD and we leave base_dir empty.
    _parser_xml_internal__base_dir[0] = 0;
    if (path != NULL_POINTER)
    {
        int64 last_sep = -1;
        for (int64 i = 0; path[i] != 0; i++)
        {
            if (path[i] == '/' || path[i] == '\\')
            {
                last_sep = i;
            }
        }
        if (last_sep >= 0)
        {
            int64 dir_len = last_sep + 1;
            if (dir_len >= (int64)sizeof(_parser_xml_internal__base_dir))
            {
                dir_len = (int64)sizeof(_parser_xml_internal__base_dir) - 1;
            }
            stdlib__memcpy(_parser_xml_internal__base_dir, path, (size_t)dir_len);
            _parser_xml_internal__base_dir[dir_len] = 0;
        }
    }

    // Step 1: pull the entire file into memory via the fs abstraction.
    // fs__read_entire_file appends a null terminator so peek/advance
    // past the logical end simply returns 0.
    int64 size = 0;
    char* buf  = fs__read_entire_file(path, &size);
    if (buf == NULL_POINTER)
    {
        log_error("%s %s %s", "failed to read '", path, "'");
        return NULL_POINTER;
    }

    // Step 2: initialize the lexer. Stack-allocated -- there is no parser
    // heap state beyond the node tree itself.
    _parser_xml_internal__lexer_t lx;
    lx.src      = buf;
    lx.len      = size;
    lx.pos      = 0;
    lx.line     = 1;
    lx.filename = path;
    lx.ok       = TRUE;

    // Step 3: skip leading whitespace + comments before the root element.
    // Most .ui files start with a big <!-- comment --> explaining what
    // they do; we don't want the opening '<' of the comment to fool the
    // element parser.
    // Loop: skip_whitespace reads spaces/tabs/newlines. Then try to skip
    // one comment. If it skipped one, loop again (another comment could
    // follow). If not, we've reached the root element -- break.
    for (;;)
    {
        _parser_xml_internal__skip_whitespace(&lx);
        if (_parser_xml_internal__skip_comment(&lx) == FALSE)
        {
            break;
        }
    }

    // Step 4: parse the root element. This is the only recursive entry
    // point; it descends into children on its own.
    gui_node_t* root = _parser_xml_internal__parse_element(&lx);

    // Step 5: release the input buffer. The tree doesn't reference it --
    // all strings (id, class, attribute values) were copied into node
    // fields during parsing.
    GUI_FREE(buf);

    // Step 6: check the ok flag. If a helper flipped it to FALSE, at
    // least one error was logged. Discard whatever partial tree came
    // back (if any) so the caller doesn't see a malformed scene.
    if (lx.ok == FALSE)
    {
        if (root != NULL_POINTER)
        {
            scene__node_free(root);
        }
        return NULL_POINTER;
    }
    return root;
}

/**
 * @brief parses a .ui document from an in-memory byte buffer
 *
 * @param const char* bytes -> UTF-8 .ui document bytes; the buffer does not need to be null-terminated, exactly len bytes are read
 * @param int64 len -> byte count of the buffer
 * @param char* source_label -> optional filename-like tag echoed in error logs; pass NULL_POINTER to substitute "(memory)"
 *
 * @attention no source path exists, so the base directory is cleared; embedded .ui that uses <image src="..."> needs explicit absolute paths, relative paths will not resolve
 *
 * @return gui_node_t* -> parsed tree root, or NULL_POINTER on parse failure; the caller owns the tree and must call scene__node_free
 */
gui_node_t* parser_xml__load_ui_from_memory(const char* bytes, int64 len, char* source_label)
{
    _parser_xml_internal__lexer_t lx;
    gui_node_t* root = 0;

    _parser_xml_internal__base_dir[0] = 0;

    if (bytes == NULL_POINTER || len <= 0)
    {
        log_error("%s", "parser_xml__load_ui_from_memory: null or empty buffer");
        return NULL_POINTER;
    }

    lx.src      = (char*)bytes;
    lx.len      = len;
    lx.pos      = 0;
    lx.line     = 1;
    lx.filename = source_label;
    lx.ok       = TRUE;

    if (lx.filename == NULL_POINTER)
    {
        lx.filename = (char*)"(memory)";
    }

    // skip leading whitespace and comments before the root element, same as the file-path entry point
    for (;;)
    {
        _parser_xml_internal__skip_whitespace(&lx);

        if (_parser_xml_internal__skip_comment(&lx) == FALSE)
        {
            break;
        }
    }

    root = _parser_xml_internal__parse_element(&lx);

    if (lx.ok == FALSE)
    {
        if (root != NULL_POINTER)
        {
            scene__node_free(root);
        }
        return NULL_POINTER;
    }

    return root;
}

// ===========================================================================
// Lexer primitives -- character-at-a-time helpers.
// ===========================================================================
// These are tiny on purpose: readable at a glance, and the compiler
// inlines them where it matters (peek / advance appear hundreds of
// times in a parse).

// Log an error with filename:line:message formatting and flag the
// lexer. Every downstream helper bails out when it notices ok == FALSE
// (usually through a boolean return).
static void _parser_xml_internal__error(_parser_xml_internal__lexer_t* lx, char* msg)
{
    log_error("%s %s %lld %s %s", lx->filename, ":", (long long)lx->line, ":", msg);
    lx->ok = FALSE;
}

// Look at the current character without consuming it. Returns 0 (NUL)
// at/past end-of-file so every caller can treat "EOF" and "NUL byte in
// the middle" the same way (neither is a legal XML content byte).
static char _parser_xml_internal__peek(_parser_xml_internal__lexer_t* lx)
{
    if (lx->pos >= lx->len)
    {
        return 0;
    }
    return lx->src[lx->pos];
}

// Consume one character, returning it. Bumps the line counter on '\n'
// so error messages can cite the right line even for multi-line input.
// Returns 0 at EOF (matching peek's convention).
static char _parser_xml_internal__advance(_parser_xml_internal__lexer_t* lx)
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

// "Have we read past the last byte?" Used as a loop guard by every
// parsing function to avoid reading off the end.
static boole _parser_xml_internal__eof(_parser_xml_internal__lexer_t* lx)
{
    return (boole)(lx->pos >= lx->len);
}

// Skip runs of XML whitespace. XML defines whitespace as these four
// ASCII bytes; we don't accept Unicode whitespace (no .ui file needs
// it, and the grammar stays trivially stable under encoding drift).
static void _parser_xml_internal__skip_whitespace(_parser_xml_internal__lexer_t* lx)
{
    while (_parser_xml_internal__eof(lx) == FALSE)
    {
        char c = _parser_xml_internal__peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            _parser_xml_internal__advance(lx);
        }
        else
        {
            break;
        }
    }
}

// Does the remaining input start with the given literal string? Used
// for recognizing multi-character tokens like "<!--", "-->", "</".
// Does NOT consume on match -- the caller decides whether to advance
// past it (usually by `lx->pos += strlen(s)`).
static boole _parser_xml_internal__starts_with(_parser_xml_internal__lexer_t* lx, char* s)
{
    int64 n = (int64)stdlib__strlen(s);
    if (lx->pos + n > lx->len)
    {
        return FALSE;
    }
    return (boole)(stdlib__memcmp(lx->src + lx->pos, s, (size_t)n) == 0);
}

// If the cursor is at "<!--", consume the full comment (through "-->")
// and return TRUE. Otherwise return FALSE without advancing.
// Edge case: comments that never close log "unterminated comment" and
// still return TRUE (so the caller doesn't think the opening "<!--"
// was a real element). The error latches ok=FALSE; the top-level
// caller will notice and discard the tree.
static boole _parser_xml_internal__skip_comment(_parser_xml_internal__lexer_t* lx)
{
    if (_parser_xml_internal__starts_with(lx, "<!--") == FALSE)
    {
        return FALSE;
    }
    // Consume the opening "<!--" and the closing "-->" via advance()
    // calls so line bookkeeping is exact even if a future grammar
    // change ever lets one of those tokens span a newline. The cost
    // (4 + 3 calls per comment) is irrelevant compared to the
    // body-walk cost.
    for (int i = 0; i < 4; i++)
    {
        _parser_xml_internal__advance(lx);
    }

    // Scan until "-->" or EOF. Calling advance() inside the loop
    // correctly bumps the line counter on embedded '\n'.
    while (_parser_xml_internal__eof(lx) == FALSE)
    {
        if (_parser_xml_internal__starts_with(lx, "-->"))
        {
            for (int i = 0; i < 3; i++)
            {
                _parser_xml_internal__advance(lx);
            }
            return TRUE;
        }
        _parser_xml_internal__advance(lx);
    }
    _parser_xml_internal__error(lx, "unterminated comment");
    // Bug #11: return FALSE here so the caller doesn't loop on a
    // "skipped" comment when we actually hit EOF mid-comment.
    // Combined with lx->ok already being FALSE, the top-level
    // cleanup catches the partial parse and discards the tree.
    return FALSE;
}

// XML Name character classification (simplified to ASCII). The full XML
// spec allows many more Unicode categories, but .ui names are always
// tag identifiers chosen by the engine (no user input), so ASCII is
// enough and keeps the check a couple of comparisons.
// The `first` argument distinguishes "first name character" (must be a
// letter or underscore) from "continuation character" (digits, '-',
// '.' also allowed). This matches the XML spec's Name / NameStartChar
// split.
static boole _parser_xml_internal__is_name_char(char c, boole first)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')
    {
        return TRUE;
    }
    if (first == FALSE && ((c >= '0' && c <= '9') || c == '-' || c == '.'))
    {
        return TRUE;
    }
    return FALSE;
}

// Read an XML Name (element tag name or attribute name) into `out`,
// null-terminated, capped to `cap` bytes (including terminator).
// Returns FALSE on:
// - empty name: nothing valid read (logs "expected a name").
// - overflow: name exceeds `cap-1` bytes (logs "name too long").
// Otherwise returns TRUE. Leaves the cursor immediately after the last
// Name character.
static boole _parser_xml_internal__parse_name(_parser_xml_internal__lexer_t* lx, char* out, int64 cap)
{
    int64 n = 0;
    boole first = TRUE;
    while (_parser_xml_internal__eof(lx) == FALSE)
    {
        char c = _parser_xml_internal__peek(lx);
        if (_parser_xml_internal__is_name_char(c, first) == FALSE)
        {
            break;
        }
        if (n + 1 >= cap)
        {
            _parser_xml_internal__error(lx, "name too long");
            return FALSE;
        }
        out[n++] = c;
        _parser_xml_internal__advance(lx);
        first = FALSE;
    }
    if (n == 0)
    {
        _parser_xml_internal__error(lx, "expected a name");
        return FALSE;
    }
    out[n] = 0;
    return TRUE;
}

// Read a quoted attribute value. Handles both ' and " as quote
// delimiters (XML requires matching), and decodes the four mandated
// entity references along the way.
// On return, `out` holds the decoded value, null-terminated. Cursor
// sits immediately after the closing quote.
// The four entities:
// &lt;    -> '<'
// &gt;    -> '>'
// &amp;   -> '&'
// &quot;  -> '"'
// Anything else starting with '&' is a fatal error -- numeric refs
// (&#65;) and named refs (&apos;, &nbsp;) are not accepted. This is
// deliberate: if you want non-ASCII in an attribute, put the raw
// UTF-8 bytes in the file. The parser is byte-oriented and passes
// them through unmolested.
static boole _parser_xml_internal__parse_attr_value(_parser_xml_internal__lexer_t* lx, char* out, int64 cap, int64* length)
{
    char quote = _parser_xml_internal__peek(lx);
    if (quote != '"' && quote != '\'')
    {
        _parser_xml_internal__error(lx, "expected \" or ' to begin attribute value");
        return FALSE;
    }
    _parser_xml_internal__advance(lx); // consume opening quote.

    int64 n = 0;
    while (_parser_xml_internal__eof(lx) == FALSE)
    {
        char c = _parser_xml_internal__peek(lx);

        // Matching closing quote ends the value.
        if (c == quote)
        {
            _parser_xml_internal__advance(lx);
            if (n >= cap)
            {
                _parser_xml_internal__error(lx, "attribute value too long");
                return FALSE;
            }
            if (out != NULL_POINTER)
            {
                out[n] = 0;
            }
            *length = n;
            return TRUE;
        }

        // Entity reference decode. Branches are inlined for each of the
        // four entities; each branch:
        // 1. Checks the full multi-char prefix via starts_with.
        // 2. Jumps the cursor past it.
        // 3. Appends the decoded byte to `out`.
        // 4. `continue`s back to the top of the loop.
        // An '&' that doesn't start one of the four recognized
        // sequences is a fatal error -- we don't want to silently
        // pass through unknown entities and produce a corrupted
        // value.
        if (c == '&')
        {
            if (_parser_xml_internal__starts_with(lx, "&lt;"))
            {
                lx->pos += 4;
                if (n + 1 >= cap)
                {
                    _parser_xml_internal__error(lx, "value overflow");
                    return FALSE;
                }
                if (out != NULL_POINTER)
                {
                    out[n] = '<';
                }
                n++;
                continue;
            }
            if (_parser_xml_internal__starts_with(lx, "&gt;"))
            {
                lx->pos += 4;
                if (n + 1 >= cap)
                {
                    _parser_xml_internal__error(lx, "value overflow");
                    return FALSE;
                }
                if (out != NULL_POINTER)
                {
                    out[n] = '>';
                }
                n++;
                continue;
            }
            if (_parser_xml_internal__starts_with(lx, "&amp;"))
            {
                lx->pos += 5;
                if (n + 1 >= cap)
                {
                    _parser_xml_internal__error(lx, "value overflow");
                    return FALSE;
                }
                if (out != NULL_POINTER)
                {
                    out[n] = '&';
                }
                n++;
                continue;
            }
            if (_parser_xml_internal__starts_with(lx, "&quot;"))
            {
                lx->pos += 6;
                if (n + 1 >= cap)
                {
                    _parser_xml_internal__error(lx, "value overflow");
                    return FALSE;
                }
                if (out != NULL_POINTER)
                {
                    out[n] = '"';
                }
                n++;
                continue;
            }
            _parser_xml_internal__error(lx, "unknown entity (only &lt; &gt; &amp; &quot; are recognized)");
            return FALSE;
        }

        // Ordinary byte. Append and advance.
        if (n + 1 >= cap)
        {
            _parser_xml_internal__error(lx, "attribute value too long");
            return FALSE;
        }
        if (out != NULL_POINTER)
        {
            out[n] = c;
        }
        n++;
        _parser_xml_internal__advance(lx);
    }

    // Fell off the end of the file without seeing the closing quote.
    _parser_xml_internal__error(lx, "unterminated attribute value");
    return FALSE;
}

// Validate that `value` matches the XML Name production: starts with
// a letter or underscore, then letters / digits / '-' / '.' / '_'.
// id selectors (#foo) and class selectors (.foo) downstream do
// strcmp against this stored value, so a `id="hello world"` would
// silently never match `#hello world` (no space allowed in CSS
// selectors). Logs a warning and returns FALSE for the caller to
// decide; current callers still STORE the value (so the user sees
// the bad data in error logs) but the warning makes the cause
// obvious.
static boole _parser_xml_internal__validate_id(char* value, _parser_xml_internal__lexer_t* lx)
{
    if (value == NULL_POINTER || value[0] == 0)
    {
        log_warn("%s %s %lld %s", lx->filename, ":", (long long)lx->line, ": id='' is empty");
        return FALSE;
    }
    boole first = TRUE;
    for (char* c = value; *c != 0; c++)
    {
        if (_parser_xml_internal__is_name_char(*c, first) == FALSE)
        {
            log_warn("%s %s %lld %s %s %s %c %s", lx->filename, ":", (long long)lx->line, ": id='", value, "' contains invalid character '", *c, "' (allowed: letters, digits, '_', '-', '.'); CSS selectors won't match");
            return FALSE;
        }
        first = FALSE;
    }
    return TRUE;
}

// Same rules as id but applied per whitespace-separated token --
// `class="foo bar baz"` is three classes. Empty value is allowed
// (a node with no classes); any non-empty token must validate.
// Tokens with bad characters log a warning so multi-class selector
// matches don't fail mysteriously.
static boole _parser_xml_internal__validate_class(char* value, _parser_xml_internal__lexer_t* lx)
{
    if (value == NULL_POINTER || value[0] == 0)
    {
        return TRUE;
    }
    boole all_ok = TRUE;
    char* token_start = value;
    char* c           = value;
    while (TRUE)
    {
        char ch = *c;
        boole is_sep = (ch == ' ' || ch == '\t' || ch == 0);
        if (is_sep)
        {
            int64 token_len = c - token_start;
            if (token_len > 0)
            {
                // Validate token in-place. Same rules as id.
                boole first = TRUE;
                for (char* t = token_start; t < c; t++)
                {
                    if (_parser_xml_internal__is_name_char(*t, first) == FALSE)
                    {
                        log_warn("%s %s %lld %s %s %s %c %s", lx->filename, ":", (long long)lx->line, ": class='", value, "' token contains invalid character '", *t, "' (allowed: letters, digits, '_', '-', '.'); CSS selectors won't match");
                        all_ok = FALSE;
                        break;
                    }
                    first = FALSE;
                }
            }
            if (ch == 0)
            {
                break;
            }
            token_start = c + 1;
        }
        c++;
    }
    return all_ok;
}

// ===========================================================================
// Element parser -- the recursive heart of the thing.
// ===========================================================================

// Parse ONE element and all its children recursively.
// Grammar (EBNF-ish):
// element       := '<' Name attribute* ( '/>' | '>' content '</' Name '>' )
// attribute     := Name '=' attr-value
// content       := ( element | comment | text )*
// The function is entered when the lexer is positioned at '<' (either
// the root call from parser_xml__load_ui, or a recursive call for a
// nested child). It returns either a valid gui_node_t* rooted at the
// element it just consumed, or NULL + lx.ok=FALSE on any error.
// Resource ownership: this function allocates a gui_node_t for the
// element and may allocate additional nodes for children. On any
// error path AFTER the allocation, we scene__node_free(node) before
// returning NULL so we never leak partial trees.
static gui_node_t* _parser_xml_internal__parse_element(_parser_xml_internal__lexer_t* lx)
{
    // ----- Opening '<' -----------------------------------------------------
    if (_parser_xml_internal__peek(lx) != '<')
    {
        _parser_xml_internal__error(lx, "expected '<'");
        return NULL_POINTER;
    }
    _parser_xml_internal__advance(lx);

    // ----- Tag name --------------------------------------------------------
    // 64 bytes is plenty: our longest tag is maybe 10 chars ("<Button>"),
    // and adding a new widget means adding a registry entry with a
    // similar-length name. Overflow reports "name too long" and bails.
    char tag_name[64];
    if (_parser_xml_internal__parse_name(lx, tag_name, (int64)sizeof(tag_name)) == FALSE)
    {
        return NULL_POINTER;
    }

    // ----- Look up the widget --------------------------------------------
    // Tag name -> node type via the widget registry. Unknown tags are a
    // soft failure: we warn and fall through to GUI_NODE_TYPE__DIV so the
    // rest of the file still parses. This is nice for typos ("Butotn"
    // doesn't wreck the whole tree, just that one node).
    gui_node_type_e type;
    if (widget_registry__lookup_by_name(tag_name, &type) == FALSE)
    {
        log_warn("%s %s %lld %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown tag '", tag_name, "' (treating as div)");
        type = GUI_NODE_TYPE__DIV;
    }

    // ----- Allocate the node ----------------------------------------------
    // scene__node_new zero-initializes the node (calloc). If it fails,
    // the tree is aborted; OOM during parse is rare but handled.
    gui_node_t* node = scene__node_new(type);
    if (node == NULL_POINTER)
    {
        _parser_xml_internal__error(lx, "out of memory");
        return NULL_POINTER;
    }

    // ----- Widget defaults -------------------------------------------------
    // init_defaults lets the widget vtable set per-type fields BEFORE any
    // attribute is parsed. Example: Slider fills in min=0, max=1, value=0
    // so XML without explicit min/max still behaves. If a later attribute
    // (min="5") overrides the default, that still works because attribute
    // parsing comes next.
    const widget_vtable_t* vt = widget_registry__get(type);
    if (vt != NULL_POINTER && vt->init_defaults != NULL_POINTER)
    {
        vt->init_defaults(node);
    }

    // ----- Attribute loop --------------------------------------------------
    // States we can be in after whitespace-skipping:
    // '/'   -> start of self-closing "/>" -> return node.
    // '>'   -> end of opening tag, go parse content.
    // '\0'  -> unexpected EOF, error.
    // other -> must be the start of another attribute (attr-name).
    for (;;)
    {
        _parser_xml_internal__skip_whitespace(lx);
        char c = _parser_xml_internal__peek(lx);

        if (c == '/')
        {
            // Self-closing tag: <Button /> or <Img src="x.png" />.
            // Consume '/' then require '>'. No content, no closing tag.
            _parser_xml_internal__advance(lx);
            if (_parser_xml_internal__peek(lx) != '>')
            {
                _parser_xml_internal__error(lx, "expected '>' after '/'");
                scene__node_free(node);
                return NULL_POINTER;
            }
            _parser_xml_internal__advance(lx);
            return node;
        }

        if (c == '>')
        {
            // End of opening tag; break out of the attribute loop and
            // fall through to content parsing.
            _parser_xml_internal__advance(lx);
            break;
        }

        if (c == 0)
        {
            _parser_xml_internal__error(lx, "unexpected end of file inside tag");
            scene__node_free(node);
            return NULL_POINTER;
        }

        // Must be start of an attribute. Parse name = "value".
        char attr_name[64];
        if (_parser_xml_internal__parse_name(lx, attr_name, (int64)sizeof(attr_name)) == FALSE)
        {
            scene__node_free(node);
            return NULL_POINTER;
        }
        _parser_xml_internal__skip_whitespace(lx);
        if (_parser_xml_internal__peek(lx) != '=')
        {
            _parser_xml_internal__error(lx, "expected '=' after attribute name");
            scene__node_free(node);
            return NULL_POINTER;
        }
        _parser_xml_internal__advance(lx);
        _parser_xml_internal__skip_whitespace(lx);

        // Measure decoded bytes before allocating. Short attributes stay on
        // the stack; escaped text is bounded by its decoded length.
        char local_value[256];
        char* value = local_value;
        int64 value_length = 0;
        _parser_xml_internal__lexer_t probe = *lx;
        if (_parser_xml_internal__parse_attr_value(&probe, NULL_POINTER, (int64)MEEK_UI_MAX_TEXT_BYTES + 1, &value_length) == FALSE)
        {
            lx->ok = FALSE;
            scene__node_free(node);
            return NULL_POINTER;
        }
        if (value_length >= (int64)sizeof(local_value))
        {
            value = GUI_MALLOC((size_t)value_length + 1);
            if (value == NULL_POINTER)
            {
                _parser_xml_internal__error(lx, "out of memory reading attribute");
                scene__node_free(node);
                return NULL_POINTER;
            }
        }
        if (_parser_xml_internal__parse_attr_value(lx, value, value_length + 1, &value_length) == FALSE)
        {
            if (value != local_value)
            {
                GUI_FREE(value);
            }
            scene__node_free(node);
            return NULL_POINTER;
        }

        // -------------------------------------------------------------
        // Dispatch by attribute name.
        // -------------------------------------------------------------
        // Four attributes are generic (any node can carry them) and
        // handled here in the parser:
        // id         -> node->id          (selector target, unique)
        // class      -> node->klass       (selector target, shared)
        // on_click   -> node->on_click_*  (event handler wiring)
        // on_change  -> node->on_change_* (event handler wiring)
        // Plus two common-but-special-cased ones:
        // text       -> node->text        (Text, Button, Input all
        // use this same slot)
        // title      -> recognized but ignored (Window title set via
        // gui_app_config_t in platform_win32__init)
        // Anything else is widget-specific: hand off to the widget's
        // apply_attribute hook. If the widget doesn't recognize it
        // either, log an "unknown attribute" warning but keep parsing
        // (typos shouldn't wreck the file).
        if (stdlib__strcmp(attr_name, "id") == 0)
        {
            // Validate before storing. Bad characters still get stored
            // (so the user sees their original input echoed back in
            // any later log line) but the warning makes the cause
            // obvious. Without this, an id with a space or '#' or
            // any other non-Name char would silently never match a
            // CSS selector since selector parsing rejects the same
            // characters.
            _parser_xml_internal__validate_id(value, lx);
            size_t n = stdlib__strlen(value);
            if (n >= sizeof(node->id))
            {
                // Silent truncation here would manifest as "my
                // selector #foo doesn't match" with no obvious
                // cause. Log so the dev knows their long id was
                // clipped.
                log_warn("%s %s %lld %s %s %s %zu %s", lx->filename, ":", (long long)lx->line, ": id='", value, "' truncated to", sizeof(node->id) - 1, "bytes (max id length)");
                n = sizeof(node->id) - 1;
            }
            stdlib__memcpy(node->id, value, n);
            node->id[n] = 0;
        }
        else if (stdlib__strcmp(attr_name, "class") == 0)
        {
            // Per-token validation; class is a space-separated list
            // and each token has to satisfy the same Name rules
            // selectors apply.
            _parser_xml_internal__validate_class(value, lx);
            size_t n = stdlib__strlen(value);
            if (n >= sizeof(node->klass))
            {
                log_warn("%s %s %lld %s %s %s %zu %s", lx->filename, ":", (long long)lx->line, ": class='", value, "' truncated to", sizeof(node->klass) - 1, "bytes (max class length)");
                n = sizeof(node->klass) - 1;
            }
            stdlib__memcpy(node->klass, value, n);
            node->klass[n] = 0;
        }
        else if (stdlib__strcmp(attr_name, "navigation-title") == 0)
        {
            stdlib__strcpy_safe(node->navigation_title, sizeof(node->navigation_title), value);
        }
        else if (stdlib__strcmp(attr_name, "navigation-parent") == 0)
        {
            stdlib__strcpy_safe(node->navigation_parent, sizeof(node->navigation_parent), value);
        }
        else if (stdlib__strcmp(attr_name, "on_click") == 0)
        {
            // scene__set_on_click both copies the name AND precomputes
            // the fnv-1a hash, so dispatch is a hash compare at click
            // time (no strcmp hot path).
            scene__set_on_click(node, value);
        }
        else if (stdlib__strcmp(attr_name, "on_change") == 0)
        {
            scene__set_on_change(node, value);
        }
        else if (stdlib__strcmp(attr_name, "text") == 0 ||
                 ((node->type == GUI_NODE_TYPE__INPUT || node->type == GUI_NODE_TYPE__TEXTAREA) &&
                  (stdlib__strcmp(attr_name, "placeholder") == 0 || stdlib__strcmp(attr_name, "value") == 0)) ||
                 (node->type == GUI_NODE_TYPE__BUTTON && stdlib__strcmp(attr_name, "value") == 0))
        {
            if (scene__set_text(node, value, value_length) == FALSE)
            {
                _parser_xml_internal__error(lx, "invalid UTF-8 text, text limit exceeded, or out of memory");
            }
        }
        else if (stdlib__strcmp(attr_name, "title") == 0)
        {
            // A window's title is a platform concern (the HWND caption
            // comes from gui_app_config_t in platform_win32__init, not from
            // the node). Another widget may take the attribute: a select
            // shows it above its options. Without a taker it is ignored,
            // with no warning.
            const widget_vtable_t* titled = widget_registry__get(node->type);
            if (titled != NULL_POINTER && titled->apply_attribute != NULL_POINTER)
            {
                titled->apply_attribute(node, attr_name, value);
            }
        }
        else
        {
            // Widget-specific: slider min/max/value, future widgets'
            // exotic knobs. apply_attribute returns TRUE to consume or
            // FALSE to let us log "unknown".
            boole consumed = FALSE;
            // fetched per attribute: <input type="checkbox"> re-types the
            // node from inside the input widget's apply_attribute, and the
            // attributes that follow belong to the new widget
            const widget_vtable_t* cur = widget_registry__get(node->type);
            if (cur != NULL_POINTER && cur->apply_attribute != NULL_POINTER)
            {
                consumed = cur->apply_attribute(node, attr_name, value);
            }
            if (consumed == FALSE)
            {
                log_warn("%s %s %lld %s %s %s %s %s", lx->filename, ":", (long long)lx->line, ": unknown attribute '", attr_name, "' on <", tag_name, ">");
            }
        }
        if (value != local_value)
        {
            GUI_FREE(value);
        }
        if (lx->ok == FALSE)
        {
            scene__node_free(node);
            return NULL_POINTER;
        }
    }

    // ----- Content loop ----------------------------------------------------
    // At this point we've consumed the opening tag's closing '>'. We're
    // either about to see a child element, text content, a comment, or
    // the closing tag "</Name>".
    // States after whitespace-skipping:
    // "</"              -> closing tag of the current element.
    // "<!--"            -> XML comment -- skip and continue.
    // '<' + other       -> child element -- recurse.
    // EOF               -> error (unterminated element).
    // other             -> text content -- currently consumed and
    // discarded (no <Text> child is created from
    // free-form text; text belongs on the <Text>
    // widget via its text="" attribute).
    for (;;)
    {
        if (type != GUI_NODE_TYPE__TD && type != GUI_NODE_TYPE__TH)
        {
            _parser_xml_internal__skip_whitespace(lx);
        }

        if (_parser_xml_internal__skip_comment(lx))
        {
            continue;
        }

        if (_parser_xml_internal__starts_with(lx, "</"))
        {
            // Closing tag. Consume "</", read the name, require '>',
            // and verify the name matches the opening tag. Mismatches
            // ("<Button>...</Slider>") are a fatal parse error.
            lx->pos += 2;
            char close_name[64];
            if (_parser_xml_internal__parse_name(lx, close_name, (int64)sizeof(close_name)) == FALSE)
            {
                scene__node_free(node);
                return NULL_POINTER;
            }
            _parser_xml_internal__skip_whitespace(lx);
            if (_parser_xml_internal__peek(lx) != '>')
            {
                _parser_xml_internal__error(lx, "expected '>' to end closing tag");
                scene__node_free(node);
                return NULL_POINTER;
            }
            _parser_xml_internal__advance(lx);
            if (stdlib__strcmp(close_name, tag_name) != 0)
            {
                _parser_xml_internal__error(lx, "mismatched closing tag");
                scene__node_free(node);
                return NULL_POINTER;
            }
            // Successful close: the element is complete, return it.
            if (type == GUI_NODE_TYPE__TD || type == GUI_NODE_TYPE__TH)
            {
                int64 length = node->text_len;
                if (length > 0 && node->text[length - 1] == ' ')
                {
                    node->text[--node->text_len] = 0;
                }
                if (scene__set_text(node, node->text, node->text_len) == FALSE)
                {
                    _parser_xml_internal__error(lx, "invalid UTF-8 in table cell");
                    scene__node_free(node);
                    return NULL_POINTER;
                }
            }
            return node;
        }

        if (_parser_xml_internal__peek(lx) == '<')
        {
            // Child element. Recurse; the recursive call consumes the
            // entire child subtree, then we append it via
            // scene__add_child so the parent/sibling links are right.
            gui_node_t* child = _parser_xml_internal__parse_element(lx);
            if (child == NULL_POINTER)
            {
                scene__node_free(node);
                return NULL_POINTER;
            }
            scene__add_child(node, child);
            continue;
        }

        if (_parser_xml_internal__eof(lx))
        {
            _parser_xml_internal__error(lx, "unexpected end of file inside element");
            scene__node_free(node);
            return NULL_POINTER;
        }

        // Table cells accept their text as element content, matching HTML authoring.
        if (type == GUI_NODE_TYPE__TD || type == GUI_NODE_TYPE__TH)
        {
            int64 length = node->text_len;
            char content = _parser_xml_internal__peek(lx);
            if (content == '\r' || content == '\n' || content == '\t')
            {
                content = ' ';
            }
            if (content == ' ' && (length == 0 || node->text[length - 1] == ' '))
            {
                _parser_xml_internal__advance(lx);
                continue;
            }
            if (content == '&')
            {
                if (_parser_xml_internal__starts_with(lx, "&amp;"))
                {
                    content = '&';
                    lx->pos += 4;
                }
                else if (_parser_xml_internal__starts_with(lx, "&lt;"))
                {
                    content = '<';
                    lx->pos += 3;
                }
                else if (_parser_xml_internal__starts_with(lx, "&gt;"))
                {
                    content = '>';
                    lx->pos += 3;
                }
                else if (_parser_xml_internal__starts_with(lx, "&quot;"))
                {
                    content = '"';
                    lx->pos += 5;
                }
                else if (_parser_xml_internal__starts_with(lx, "&apos;"))
                {
                    content = 39;
                    lx->pos += 5;
                }
            }
            if (scene__reserve_text(node, length + 1))
            {
                node->text[length] = content;
                node->text[length + 1] = 0;
                node->text_len = length + 1;
            }
            else
            {
                _parser_xml_internal__error(lx, "table cell text limit exceeded or out of memory");
                scene__node_free(node);
                return NULL_POINTER;
            }
        }
        _parser_xml_internal__advance(lx);
    }
}
