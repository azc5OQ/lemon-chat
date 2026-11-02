#ifndef WIDGET_DATEPICKER_H
#define WIDGET_DATEPICKER_H

// widget_datepicker.h - public API for the <datepicker> widget.
// An inline month calendar. .ui:
// <datepicker id="due" value="2026-09-13" week-start="monday" on_change="on_due_change" />
// handler.c:
// UI_HANDLER void on_due_change(gui_event_t* ev)
// {
// log_info("%lld-%lld-%lld", ev->date.year, ev->date.month, ev->date.day);
// }
// The selected date is also kept in node->text as "YYYY-MM-DD", so a
// host can read it without a handler. From C, build the node with
// scene__node_new(GUI_NODE_TYPE__DATEPICKER) and call the setters
// below; the widget has no notion of "today" (the library has no
// calendar clock), so a host that wants the current month passes it.

#include "types.h"
#include "gui.h"
#include "gui_api.h"

/**
 * @brief select a date and show its month. Writes "YYYY-MM-DD" into node->text; does not fire on_change (that is for user input).
 *
 * @param gui_node_t* n -> A datepicker node.
 * @param int64 year -> Four-digit year.
 * @param int64 month -> 1 to 12.
 * @param int64 day -> 1 to the month's length; clamped.
 *
 * @return void
 */
GUI_API void  widget_datepicker__set(gui_node_t* n, int64 year, int64 month, int64 day);

/**
 * @brief read the selected date.
 *
 * @param gui_node_t* n -> A datepicker node.
 * @param int64* out_year -> Receives the year; may be NULL.
 * @param int64* out_month -> Receives the month; may be NULL.
 * @param int64* out_day -> Receives the day; may be NULL.
 *
 * @return boole -> FALSE when nothing is selected or n is not a datepicker.
 */
GUI_API boole widget_datepicker__get(gui_node_t* n, int64* out_year, int64* out_month, int64* out_day);

/**
 * @brief show a month without changing the selection.
 *
 * @param gui_node_t* n -> A datepicker node.
 * @param int64 year -> Four-digit year.
 * @param int64 month -> 1 to 12.
 *
 * @return void
 */
GUI_API void  widget_datepicker__show_month(gui_node_t* n, int64 year, int64 month);

#endif
