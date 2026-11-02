#ifndef WIDGET_SELECT_H
#define WIDGET_SELECT_H

#include "../gui.h"
#include "../gui_api.h"

// The select in its styles (select-style, docs/select-styles.md). list,
// wheel, sheet, page, menu, dialog and search open over the page: opening
// installs a modal overlay, outside clicks dismiss it without activating a
// control underneath. segmented, chips and carousel draw their options
// inside the control and open nothing. No new widget or host window is
// created by any of them.

// Opens or closes a style that opens; no effect on the other three.
GUI_API void scene__select_open(gui_node_t* node, boole open);
// Device-pixel geometry shared by painting, input and host integrations:
// the panel of an open select at rest (after its entrance).
GUI_API gui_rect_t scene__select_popup_bounds(gui_node_t* node);
// The rectangle an option is drawn in right now, with the reveal, the
// scroll and the scroll effect applied; for segmented, chips and carousel
// the option's place inside the control. FALSE for an invalid option and
// for one that is not visible (not revealed yet, scrolled out, filtered out
// by a search). The optional alpha is the displayed opacity.
GUI_API boole scene__select_option_bounds(gui_node_t* node, int64 index, gui_rect_t* bounds, float* alpha);

// The parts of an open select besides its options, as drawn right now. A
// part the style does not have returns a rectangle of zero size.
typedef enum gui_select_part_e
{
    GUI_SELECT_PART__PANEL   = 0, // the whole panel
    GUI_SELECT_PART__ROWS    = 1, // the area the options are drawn and clipped in
    GUI_SELECT_PART__HEADER  = 2, // sheet: handle and title; page, dialog: title bar; search: the text field
    GUI_SELECT_PART__FOOTER  = 3, // dialog: the strip with the two buttons
    GUI_SELECT_PART__CONFIRM = 4, // dialog: the button that takes the marked option
    GUI_SELECT_PART__CANCEL  = 5, // dialog: the button that closes without a change
    GUI_SELECT_PART__BACK    = 6, // page: the arrow that closes without a change
    GUI_SELECT_PART__BAR     = 7  // the scrollbar's track
} gui_select_part_e;
GUI_API gui_rect_t scene__select_part_bounds(gui_node_t* node, gui_select_part_e part);

// select-style: search. The text typed into the field ("" for every other
// style), and the number of options that match it.
GUI_API const char* scene__select_query(gui_node_t* node);
GUI_API int64 scene__select_match_count(gui_node_t* node);
// select-style: dialog. The option marked before the confirm button takes
// it, -1 for every other style and while the dialog is closed.
GUI_API int64 scene__select_pending(gui_node_t* node);
// Diagnostic. The number of option nodes the select's functions have
// stepped over since the last reset, for all selects together. One frame
// of one select steps over each option a fixed number of times, whatever
// the number of rows on screen; the checks verify that with this counter.
GUI_API int64 scene__select_walked(boole reset);

#endif
