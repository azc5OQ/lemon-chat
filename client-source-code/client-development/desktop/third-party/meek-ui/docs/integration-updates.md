# Updating a running interface

## Keeping a form at its current scroll position

Suppose a settings form has 400 pixels of content in a 120-pixel viewport.
The user has scrolled down 80 pixels, and smooth scrolling is moving toward
160. Flex layout first measures the form without offering a final height.
The form can temporarily grow to its full content height during that probe.
Clamping at that point would change both offsets to zero, even though the
final viewport still needs scrolling.

Flex and intrinsic sizing probes now use `scene__measure_node`. It computes
bounds but preserves current and target scroll offsets throughout nested
layout calls. The final layout still clamps offsets when content really
shrinks. If the form becomes shorter than its viewport, its children are
repositioned at the clamped offset in that same layout call.

Custom container widgets can use `scene__measure_node(node, x, y, width,
height)` for probes and `scene__layout_node` for placement.
`scene__layout_is_measuring()` reports whether the current call is nested
inside a probe. A zero height alone does not select measurement mode.
This is a scroll-state guarantee; it does not make arbitrary custom-widget
side effects transactional.

## Replacing a draft without silently shortening it

Suppose switching conversations replaces a composer's draft with `hello`.
Writing the node buffer directly leaves the host responsible for length,
selection and layout bookkeeping. The bounded setter does those together:

```c
if (scene__set_text(composer, "hello", 5) == FALSE)
{
    // Keep the original draft and report that the replacement was rejected.
}
```

The length is a byte count excluding the terminator. The source needs that
many readable bytes, but does not need its own terminator. All nodes that carry
text are supported. An input's caret moves to the end and
its selection collapses, including when the new text equals the old text.
The operation does not emit `on_change`; a subsequent user edit still does.

Invalid UTF-8, embedded zero bytes and text that does not fit are rejected
without changing the old text or selection. Single-line inputs also reject
control characters, including tabs and newlines. `NULL` with length zero
clears the text. Overlapping source and destination are supported.

### Keeping a longer draft

Suppose a chat composer must accept a 4,000-byte draft but the application
allows at most 16 KiB. Set that control's limit once, then replace its draft:

```c
if (scene__set_text_limit(composer, 16 * 1024) == FALSE)
{
    // The existing draft exceeds this limit, or the library's maximum is lower.
}
if (scene__set_text(composer, draft, draft_bytes) == FALSE)
{
    // Keep the old draft: invalid UTF-8, a limit, or allocation failure prevented replacement.
}
```

A new node has 256 bytes of inline storage, enough for 255 text bytes and
its terminator. The 4,000-byte draft grows into owned heap storage. Storage
grows geometrically and is reused when the text gets shorter; clearing a
control does not release its capacity. Freeing the node releases the buffer.
Short captions still need no separate text allocation.

The default maximum is 65,536 UTF-8 bytes per node. Build the library with
`MEEK_UI_MAX_TEXT_BYTES` to change that ceiling. A control's `text_limit`
counts bytes excluding NUL; `text_capacity` counts allocated bytes including
NUL; `text_len` counts the current text bytes. A limit of zero permits only
empty text. Lowering a limit below the existing draft fails without truncation.
A multibyte character therefore uses more of the limit than an ASCII character.

Typing refuses an insertion that cannot fit. Paste inserts the complete UTF-8
prefix that fits, preserving whole codepoints. Allocation failure leaves text
and selection unchanged and emits no change event. Single-line paste converts
line breaks and tabs to spaces. Textarea paste normalizes CRLF to LF and keeps
tabs. Both inputs and textareas paste at the caret, replacing any selection.

### Editing and copying several lines

Suppose a textarea contains `first\nsecond` and you want to replace just the
second line. Click in that line, press Home and then Shift+End, and press
Ctrl+V. The clipboard text replaces `second`; `first` and its newline stay
intact. Ctrl+A selects the entire draft, Ctrl+C copies the selection, and
Ctrl+X copies and removes it. An empty clipboard or a failed clipboard read
leaves the selection intact; a failed clipboard write does not cut anything.

Drag across lines or hold Shift while clicking or using the arrow keys to
extend a selection. Home and End move within the current displayed line,
including a wrapped line. Ctrl+Home and Ctrl+End move to the document's ends.
Up and Down retain the preferred horizontal position across shorter lines.
Copying a visually wrapped line preserves its original text: wrapping does
not add newline characters to the clipboard.

Enter inserts a newline and keeps focus. Typing, Backspace and Delete act at
the caret or on the selection. The editor scrolls the caret into view after
editing or navigation; wheel scrolling lets you inspect other lines without
moving the caret. Ctrl+Insert, Shift+Insert and Shift+Delete are also available
for copy, paste and cut. Undo/redo is not implemented.

XML `text`, input/textarea `value` and `placeholder`, button `value`, and table
cell content now use the same owned storage. Invalid or oversized text makes
parsing fail instead of silently truncating it. Labels are no longer limited
to 64 rendered lines. OpenGL3, D3D9 and GLES3 text batches now flush and
continue when full, matching D3D11 instead of dropping the remaining glyphs.

Hot reload moves editable text and its runtime byte limit
to the replacement node, including an intentionally empty draft. This transfer
requires no new text allocation.

### Rebuilding callers

Rebuild the library and every application using these headers together.
`gui_node_t.text` is now a pointer to node-owned storage, so its binary layout
has changed. Continue reading through `node->text`, but never assign or free
that pointer, shallow-copy a node, or use `sizeof(node->text)` for its capacity.
Create nodes with `scene__node_new` and replace content with `scene__set_text`.
The examples and existing shell capacity callers have been migrated.

A custom editor can call `scene__reserve_text(node, desired_bytes)` before
writing. On success it can use `node->text_capacity` bytes, including NUL,
but must keep its writes within `text_limit`, terminate the string, update
`text_len`, and manage its own editing state and layout invalidation. Reserving
does not change the current text or selection. Growth can invalidate pointers
saved into the old buffer; obtain `node->text` again after any edit that may grow
it. The setter remains the convenient API for application-driven replacement.

## Changing chat scale without filling the rule table

Suppose the settings dialog changes chat text from 14 to 18 pixels each time
Save is pressed. Registering another rule on every Save eventually fills the
style table. Register the runtime rule once and retain its handle instead:

```c
gui_style_t chat_style =
{
    0
};
chat_style.font_size = 14;
uint64 chat_rule = scene__add_style(".chat-text", &chat_style, FALSE);
if (chat_rule == 0)
{
    // Registration failed; report the error instead of assuming it applied.
}

// Later, when the setting changes:
chat_style.font_size = 18;
if (scene__update_style(chat_rule, &chat_style) == FALSE)
{
    // The handle may have been invalidated by a stylesheet reload.
}
```

Each update copies the supplied style into the existing rule. It keeps that
rule's selector, normal/important priority and original cascade position.
Other rules with the same selector remain separate. Updating an earlier
rule does not move it after a later rule. Normal scene style resolution
applies the changed value.

`scene__clear_styles()` invalidates all handles. A host that reloads its
stylesheet must register its runtime rules again and store their new handles.
An old handle cannot accidentally update a new rule. Updates can still succeed
when the table is full because they consume no additional slots.

Keep the complete runtime style value and edit its desired fields, as above.
This API does not redesign the resolver's preservation of runtime style fields;
removing a class or clearing a previously supplied property still has the
existing limitations. There is no new rule-removal API in this change.

## Detecting capacity failures

`scene__style_count()` and `scene__style_capacity()` report the live table.
The default capacity remains 256. Define `MEEK_UI_MAX_STYLES` when compiling
the library to choose another positive capacity. Comma-separated selectors,
pseudo-state registrations and separate important declarations consume slots.

The stylesheet parser now returns `FALSE` if rule registration fails, including
when the table is full. Earlier successfully registered rules remain installed;
parsing is not an atomic stylesheet replacement. Callers must handle failure.
The existing void registration functions remain available for compatibility;
new callers can use `scene__add_style` to check its result.

`hot_reload__watch_style` already performs the initial stylesheet load. Do not
load the same file separately first unless duplicate registrations are intended.

## Reading the select indicator

Closed selects now draw a downward triangle in the accent color; open selects
draw an upward triangle. Both use the control's UI scale. This replaces the
two horizontal bars without changing selection, popup placement or animation.
