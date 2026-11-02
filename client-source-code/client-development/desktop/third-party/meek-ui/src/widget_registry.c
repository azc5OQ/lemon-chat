// widget_registry.c - the type -> vtable table + lookups + bootstrap.
// table is a fixed array sized GUI_NODE_TYPE__COUNT. each registration
// slot stores a pointer to the static widget_vtable_t defined in the
// corresponding widget_<name>.c file.

#include <string.h>

#include "types.h"
#include "gui.h"
#include "widget.h"
#include "widget_registry.h"
#include "clib/stdlib.h"

// forward declarations of every built-in widget's register entry point.
// each widget .c defines exactly one of these (non-static) and uses it
// to call widget_registry__register with its vtable.
// an HTML tag name that maps onto a widget registered under another name
typedef struct _widget_registry_internal__alias_t
{
    const char*     name; // 0x0
    gui_node_type_e type; // 0x8
} _widget_registry_internal__alias_t;

extern void widget_window__register(void);
extern void widget_table__register(void);
extern void widget_button__register(void);
extern void widget_slider__register(void);
extern void widget_div__register(void);
extern void widget_text__register(void);
extern void widget_input__register(void);
extern void widget_checkbox__register(void);
extern void widget_radio__register(void);
extern void widget_select__register(void);
extern void widget_option__register(void);
extern void widget_image__register(void);
extern void widget_collection__register(void);
extern void widget_colorpicker__register(void);
extern void widget_popup__register(void);
extern void widget_textarea__register(void);
extern void widget_canvas__register(void);
extern void widget_keyboard__register(void);
extern void widget_process_window__register(void);
extern void widget_navigation__register(void);
extern void widget_datepicker__register(void);
extern void widget_hexview__register(void);
extern void widget_filepicker__register(void);

static const widget_vtable_t* _widget_registry_internal__table[GUI_NODE_TYPE__COUNT];

/**
 * @brief register a widget vtable for a node type. Called once per type from the widget's widget_<name>__register function.
 *
 * @param gui_node_type_e type -> The node-type slot to populate.
 * @param const widget_vtable_t* vt -> Pointer to a static vtable. Stored by reference; must outlive the registration.
 *
 * @return void
 */
void widget_registry__register(gui_node_type_e type, const widget_vtable_t* vt)
{
    if ((int)type < 0 || (int)type >= (int)GUI_NODE_TYPE__COUNT)
    {
        return;
    }
    _widget_registry_internal__table[(int)type] = vt;
}

/**
 * @brief look up the registered vtable for a type. Returns NULL if no widget is registered for that type (treat as "no special behavior").
 *
 * @param gui_node_type_e type -> The node-type slot to query.
 *
 * @return const widget_vtable_t* -> The vtable, or NULL.
 */
const widget_vtable_t* widget_registry__get(gui_node_type_e type)
{
    if ((int)type < 0 || (int)type >= (int)GUI_NODE_TYPE__COUNT)
    {
        return NULL_POINTER;
    }
    return _widget_registry_internal__table[(int)type];
}

/**
 * @brief map an XML tag name (e.g. "Button") to a node type. parser_xml uses this instead of a hardcoded tag table.
 *
 * @param char* name -> Tag name to look up.
 * @param gui_node_type_e* out_type -> On success, receives the matching type.
 *
 * @return boole -> TRUE if a widget is registered under that name.
 */
boole widget_registry__lookup_by_name(char* name, gui_node_type_e* out_type)
{
    // the HTML names of widgets registered under another name, so a
    // .ui file can be written with the tags a web page would use
    // (`<input type="checkbox">` and friends are handled by the input
    // widget's type attribute, not here)
    static const _widget_registry_internal__alias_t aliases[] = {
        { "img", GUI_NODE_TYPE__IMAGE },
        { "body", GUI_NODE_TYPE__WINDOW },
        { "dialog", GUI_NODE_TYPE__POPUP },
        { "text", GUI_NODE_TYPE__TEXT },
    };
    const widget_vtable_t* vt = NULL_POINTER;
    int64                  i  = 0;

    if (name == NULL_POINTER || out_type == NULL_POINTER)
    {
        return FALSE;
    }

    for (i = 0; i < (int64)GUI_NODE_TYPE__COUNT; i++)
    {
        vt = _widget_registry_internal__table[i];
        if (vt == NULL_POINTER || vt->type_name == NULL_POINTER)
        {
            continue;
        }
        if (stdlib__strcmp(vt->type_name, name) == 0)
        {
            *out_type = (gui_node_type_e)i;
            return TRUE;
        }
    }

    for (i = 0; i < (int64)(sizeof(aliases) / sizeof(aliases[0])); i++)
    {
        if (stdlib__strcmp(aliases[i].name, name) == 0)
        {
            *out_type = aliases[i].type;
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief get a node type's registered name (for selector matching in scene's style resolver). Returns "" for unregistered types.
 *
 * @param gui_node_type_e type -> The node-type slot to query.
 *
 * @return char* -> The registered name, or "".
 */
char* widget_registry__type_name(gui_node_type_e type)
{
    if ((int)type < 0 || (int)type >= (int)GUI_NODE_TYPE__COUNT)
    {
        return "";
    }
    const widget_vtable_t* vt = _widget_registry_internal__table[(int)type];
    if (vt == NULL_POINTER || vt->type_name == NULL_POINTER)
    {
        return "";
    }
    return vt->type_name;
}

/**
 * @brief register every built-in widget. Called by platform_win32__init at startup so that parser_xml + scene see all built-in widgets before any tree is loaded.
 *
 * @return void
 */
void widget_registry__bootstrap_builtins(void)
{
    widget_window__register();
    widget_table__register();
    widget_button__register();
    widget_slider__register();
    widget_div__register();
    widget_text__register();
    widget_input__register();
    widget_checkbox__register();
    widget_radio__register();
    widget_select__register();
    widget_option__register();
    widget_image__register();
    widget_collection__register();
    widget_colorpicker__register();
    widget_popup__register();
    widget_textarea__register();
    widget_canvas__register();
    widget_keyboard__register();
    widget_process_window__register();
    widget_navigation__register();
    widget_datepicker__register();
    widget_hexview__register();
    widget_filepicker__register();
}
