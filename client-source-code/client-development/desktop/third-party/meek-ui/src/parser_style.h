#ifndef PARSER_STYLE_H
#define PARSER_STYLE_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// parser_style.h - .style file loading.
// paired with parser_style.c. css-shaped subset (see DESIGN.md):
// Window {
// bg: #1e1e1e;
// }
// Button {
// bg: #333333;
// radius: 6;
// size: 200x40;
// :hover   { bg: #555555; }
// :pressed { bg: #222222; }
// }
// Button.primary {
// bg: #0066cc;
// :hover   { bg: #0077e6; }
// :pressed { bg: #004a99; }
// }
// properties supported in this iteration:
// bg     #rrggbb               background color (sets has_bg = TRUE)
// radius <number>              corner radius in pixels
// pad    <number>              padding on all four sides
// gap    <number>              gap between children (column / row)
// size   <number>x<number>     explicit width x height in pixels
// comments are c-style slash-star blocks. nested pseudo-state rules are
// supported one level deep (the parent's selector + ":pseudo" forms
// the new selector and is registered separately). every declaration
// must end with a ';', no exceptions.

GUI_API boole parser_style__load_styles(char* path);
GUI_API boole parser_style__load_styles_from_memory(const char* bytes, int64 len, char* source_label);
GUI_API boole parser_style__parse_gradient(const char* value, gui_gradient_t* out);
GUI_API boole parser_style__parse_color(const char* value, gui_color_t* out);

#endif
