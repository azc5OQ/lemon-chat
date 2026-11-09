#include "platform/memory.h"
#include "clib.h"
#include <stdlib.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include "platform/hotkey.h"

static pthread_once_t g_initialize = PTHREAD_ONCE_INIT;
static boole g_available;

// declarations
static void _hotkey_internal__initialize(void);

/**
 * @brief initializes Xlib threading before any GUI or key-state display is opened.
 * @return void
 */
static void _hotkey_internal__initialize(void)
{
    g_available = XInitThreads() != 0;
}

/**
 * @brief enables safe concurrent use of the GUI display and separate key-state display.
 * @return boole -> nonzero when Xlib threading initialized
 */
boole desktop_hotkey__initialize(void)
{
    pthread_once(&g_initialize, _hotkey_internal__initialize);
    return g_available;
}

struct desktop_hotkey_t
{
    Display *display;
};

/**
 * @brief opens a worker-owned X11 connection for push-to-talk key-state queries.
 * @return desktop_hotkey_t* -> owned context, or NULL without an X11 display
 */
desktop_hotkey_t *desktop_hotkey__create(void)
{
    desktop_hotkey_t *hotkey = calloc(1, sizeof(*hotkey));
    if (hotkey)
    {
        hotkey->display = desktop_hotkey__initialize() ? XOpenDisplay(NULL) : NULL;
        if (!hotkey->display)
        {
            desktop_memory__erase(hotkey, sizeof(*hotkey));
            free(hotkey);
            hotkey = NULL;
        }
    }
    return hotkey;
}

/**
 * @brief closes the worker's separate X11 connection.
 * @param desktop_hotkey_t* hotkey -> owned context, or NULL
 * @return void
 */
void desktop_hotkey__destroy(desktop_hotkey_t *hotkey)
{
    if (hotkey)
    {
        XCloseDisplay(hotkey->display);
        desktop_memory__erase(hotkey, sizeof(*hotkey));
        free(hotkey);
    }
}

/**
 * @brief queries the selected key without grabbing keys away from another X11 app.
 * @param desktop_hotkey_t* hotkey -> context used only by its creating worker
 * @param int64 key -> printable ASCII key or legacy F-key number
 * @return boole -> nonzero while the key is held; native Wayland apps may require compositor support
 */
boole desktop_hotkey__down(desktop_hotkey_t *hotkey, int64 key)
{
    char keys[32] = { 0 };
    KeyCode code = 0;
    KeySym symbol = 0;
    boole down = FALSE;
    if (!hotkey || desktop_hotkey__index(key) < 0)
    {
        return 0;
    }
    symbol = (KeySym)(key >= 1 && key <= 12 ? XK_F1 + key - 1 : key);
    if (key >= 'A' && key <= 'Z')
    {
        symbol = (KeySym)(XK_a + key - 'A');
    }
    code = XKeysymToKeycode(hotkey->display, symbol);
    XQueryKeymap(hotkey->display, keys);
    down = code && (keys[code >> 3] & (1 << (code & 7))) != 0;
    desktop_memory__erase(keys, sizeof(keys));
    return down;
}
