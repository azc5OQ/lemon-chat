#ifndef PARSER_XML_H
#define PARSER_XML_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"

// parser_xml.h - .ui file loading.
// paired with parser_xml.c. minimal xml subset (see DESIGN.md):
// tags + attributes + comments + the four mandated entities. no
// namespaces, no doctype, no processing instructions, no cdata.

GUI_API gui_node_t*   parser_xml__load_ui(char* path);
GUI_API gui_node_t*   parser_xml__load_ui_from_memory(const char* bytes, int64 len, char* source_label);

GUI_API const char* parser_xml__base_dir(void);

#endif
