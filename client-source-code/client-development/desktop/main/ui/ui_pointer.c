#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#endif
#include "ui/ui_pointer.h"
#include "ui/ui_headers.h"
#include "ui/ui_context.h"
#include "ui/ui_chat_format.h"
#include "platform/memory.h"
#include "widget_registry.h"

static widget_vtable_t g_pointer_widgets[GUI_NODE_TYPE__COUNT];
static void (*g_pointer_down[GUI_NODE_TYPE__COUNT])(gui_node_t *, int64, int64, int64);

// declarations
static void _ui_pointer_internal__down(gui_node_t *node, int64 x, int64 y, int64 button);

/**
 * @brief keeps secondary clicks on tree rows and dismisses popups on the first outside press.
 * @param gui_node_t* node -> deepest pressed control
 * @param int64 x -> physical pointer x
 * @param int64 y -> physical pointer y
 * @param int64 button -> left 0, right 1, middle 2
 * @return void
 */
static void _ui_pointer_internal__down(gui_node_t *node, int64 x, int64 y, int64 button)
{
    gui_node_t *parent = node;
    boole row = FALSE;
    boole tree = FALSE;
    boole menu = FALSE;

    // search the parent nodes
    while (parent)
    {
        row |= clib__strncmp(parent->klass, "member", 6) == 0 || clib__strncmp(parent->klass, "channel-row", 11) == 0;
        tree |= clib__strcmp(parent->id, "member-list") == 0;
        menu |= clib__strcmp(parent->id, "context-menu") == 0 || clib__strcmp(parent->id, "context-submenu") == 0;
        parent = parent->parent;
    }

    // close the context menu
    if (!menu)
    {
        desktop_ui_context__close();
    }

    // check the mouse button
    if (button != 0)
    {
        if (button == 1 && clib__strcmp(node->id, "message-input") == 0)
        {
            desktop_ui_chat_format__open_fonts();
        }
        if (button != 1 || !tree || !row)
        {
            scene__cancel_press();
        }
        return;
    }

    // call the original function
    if (g_pointer_down[node->type])
    {
        g_pointer_down[node->type](node, x, y, button);
    }
}

#ifdef _WIN32
static boole g_pointer_installed;
static boole g_pointer_pressed;

// declarations
static LRESULT CALLBACK _ui_pointer_internal__window(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR identifier, DWORD_PTR context);
static BOOL CALLBACK _ui_pointer_internal__attach(HWND window, LPARAM context);

/**
 * @brief forwards secondary clicks missing from the current Windows backend, on this window's UI thread.
 * @param HWND window -> application-owned Meek window
 * @param UINT message -> native window event
 * @param WPARAM word -> native event data
 * @param LPARAM data -> native event data containing signed client coordinates
 * @param UINT_PTR identifier -> subclass registration identifier
 * @param DWORD_PTR context -> unused registration data
 * @return LRESULT -> native event result
 */
static LRESULT CALLBACK _ui_pointer_internal__window(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR identifier, DWORD_PTR context)
{
    int64 x = (SHORT)LOWORD(data);
    int64 y = (SHORT)HIWORD(data);
    (void)context;

    // send the right press
    if (message == WM_RBUTTONDOWN)
    {
        g_pointer_pressed = TRUE;
        SetCapture(window);
        scene__on_mouse_move(x, y);
        scene__on_mouse_button(1, TRUE, x, y);
        return 0;
    }

    // send the right release
    if (message == WM_RBUTTONUP)
    {
        g_pointer_pressed = FALSE;
        if (GetCapture() == window)
        {
            ReleaseCapture();
        }
        scene__on_mouse_button(1, FALSE, x, y);
        return 0;
    }

    // cancel a lost press
    if ((message == WM_CAPTURECHANGED || message == WM_CANCELMODE) && g_pointer_pressed)
    {
        g_pointer_pressed = FALSE;
        scene__cancel_press();
    }

    // remove the window subclass
    if (message == WM_NCDESTROY)
    {
        g_pointer_pressed = FALSE;
        g_pointer_installed = FALSE;
        RemoveWindowSubclass(window, _ui_pointer_internal__window, identifier);
    }

    // call the default procedure
    return DefSubclassProc(window, message, word, data);
}

/**
 * @brief attaches only to Meek's native window owned by the calling UI thread.
 * @param HWND window -> enumerated thread window
 * @param LPARAM context -> unused enumeration data
 * @return BOOL -> FALSE after installing the adapter, TRUE to keep searching
 */
static BOOL CALLBACK _ui_pointer_internal__attach(HWND window, LPARAM context)
{
    wchar_t name[64] = { 0 };
    (void)context;

    // attach to the window
    if (GetClassNameW(window, name, 64) && lstrcmpW(name, L"GuiDemoWindowClass") == 0)
    {
        g_pointer_installed = SetWindowSubclass(window, _ui_pointer_internal__window, 1, 0) != 0;
    }

    // erase the class name
    desktop_memory__erase(name, sizeof(name));
    return !g_pointer_installed;
}
#endif

/**
 * @brief installs the Windows secondary-button adapter; other backends already deliver these events.
 * @return boole -> TRUE when native pointer input is ready
 */
boole desktop_ui_pointer__initialize(void)
{
    const widget_vtable_t *original = NULL_POINTER;
    int64 type = 0;

    // replace every press function
    for (type = 0; type < GUI_NODE_TYPE__COUNT; type++)
    {
        original = widget_registry__get((gui_node_type_e)type);
        if (original && original->on_mouse_down != _ui_pointer_internal__down)
        {
            g_pointer_widgets[type] = *original;
            g_pointer_down[type] = original->on_mouse_down;
            g_pointer_widgets[type].on_mouse_down = _ui_pointer_internal__down;
            widget_registry__register((gui_node_type_e)type, &g_pointer_widgets[type]);
        }
    }
#ifdef _WIN32
    // search the thread windows
    if (!g_pointer_installed)
    {
        EnumThreadWindows(GetCurrentThreadId(), _ui_pointer_internal__attach, 0);
    }
    return g_pointer_installed;
#else
    return TRUE;
#endif
}
