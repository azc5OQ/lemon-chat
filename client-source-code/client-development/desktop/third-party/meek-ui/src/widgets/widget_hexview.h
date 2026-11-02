#ifndef WIDGET_HEXVIEW_H
#define WIDGET_HEXVIEW_H

// widget_hexview.h - public API for the <hexview> widget, a memory
// viewer for debugging. .ui:
// <hexview id="ram" cols="16" rows="8" format="hex" group="4" address="0x1000" on_change="on_byte_pick" />
// .style: give it a monospace font and overflow-y: auto for scrolling.
// The bytes come from the host, since the memory to look at is by
// nature the host's:
// widget_hexview__set_buffer(scene__find_by_id("ram"), buffer, size, (uint64)(uintptr_t)buffer);
// The buffer is borrowed, not copied, so the view shows live memory;
// keep it alive while the node exists, or clear it with a NULL
// pointer. A click selects a byte (on_change fires with the offset in
// ev->change.scalar), the arrow keys move the selection.

#include "types.h"
#include "gui.h"
#include "gui_api.h"

/**
 * @brief point the view at a buffer. NULL data (or len 0) clears the view; it then draws a note instead of rows.
 *
 * @param gui_node_t* n -> A hexview node.
 * @param const void* data -> The bytes to show; borrowed for the life of the node.
 * @param int64 len -> Byte count.
 * @param uint64 base_address -> The address printed for offset 0, usually the buffer's own address.
 *
 * @return void
 */
GUI_API void  widget_hexview__set_buffer(gui_node_t* n, const void* data, int64 len, uint64 base_address);

/**
 * @brief the selected byte's offset into the buffer.
 *
 * @param gui_node_t* n -> A hexview node.
 *
 * @return int64 -> Offset, or -1 when nothing is selected.
 */
GUI_API int64 widget_hexview__selected(gui_node_t* n);

/**
 * @brief select a byte and scroll its row into view.
 *
 * @param gui_node_t* n -> A hexview node.
 * @param int64 offset -> Offset into the buffer; -1 clears the selection.
 *
 * @return void
 */
GUI_API void  widget_hexview__select(gui_node_t* n, int64 offset);

/**
 * @brief bytes per row, what the columns attribute sets in .ui.
 *
 * @param gui_node_t* n -> A hexview node.
 * @param int64 columns -> 1 to 32.
 *
 * @return void
 */
GUI_API void  widget_hexview__set_columns(gui_node_t* n, int64 columns);

/**
 * @brief the value format, what the format attribute sets in .ui.
 *
 * @param gui_node_t* n -> A hexview node.
 * @param const char* format -> "hex" (2 digits), "dec" (3), "oct" (3) or "bin" (8).
 *
 * @return boole -> FALSE for an unknown format.
 */
GUI_API boole widget_hexview__set_format(gui_node_t* n, const char* format);

#endif
