#include "platform/memory.h"
#include "clib.h"
#include <windows.h>
#include <stdlib.h>
#include "platform/hotkey.h"

/**
 * @brief prepares the platform key adapter before the GUI starts.
 * @return boole -> 1; Windows key-state polling needs no initialization
 */
boole desktop_hotkey__initialize(void)
{
    return 1;
}

struct desktop_hotkey_t
{
    boole available;
};

/**
 * @brief creates a polling context for the user's chosen global push-to-talk key.
 * @return desktop_hotkey_t* -> owned context, or NULL on allocation failure
 */
desktop_hotkey_t *desktop_hotkey__create(void)
{
    return calloc(1, sizeof(desktop_hotkey_t));
}

/**
 * @brief releases a push-to-talk context after its polling worker stops.
 * @param desktop_hotkey_t* hotkey -> owned context, or NULL
 * @return void
 */
void desktop_hotkey__destroy(desktop_hotkey_t *hotkey)
{
    desktop_memory__erase(hotkey, sizeof(*hotkey));
    free(hotkey);
}

/**
 * @brief reads the selected key on the input desktop, independently of window focus.
 * @param desktop_hotkey_t* hotkey -> initialized polling context
 * @param int64 key -> printable ASCII key or legacy F-key number
 * @return boole -> nonzero while the key is held; never consumes the key
 */
boole desktop_hotkey__down(desktop_hotkey_t *hotkey, int64 key)
{
    int code = 0;

    // check arguments
    if (!hotkey || desktop_hotkey__index(key) < 0)
    {
        return FALSE;
    }

    // read the key state
    code = (int)(key >= 1 && key <= 12 ? VK_F1 + key - 1 : key);
    return (GetAsyncKeyState(code) & 0x8000) != 0;
}
