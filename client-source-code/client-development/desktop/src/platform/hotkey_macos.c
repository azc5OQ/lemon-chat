#include "platform/memory.h"
#include "clib.h"
#include <stdlib.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include "platform/hotkey.h"

/**
 * @brief prepares the platform key adapter before the GUI starts.
 * @return boole -> 1; Quartz key-state polling needs no initialization
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
 * @brief creates a push-to-talk polling context; macOS controls input-monitoring authorization.
 * @return desktop_hotkey_t* -> owned context, or NULL
 */
desktop_hotkey_t* desktop_hotkey__create(void)
{
    return calloc(1, sizeof(desktop_hotkey_t));
}

/**
 * @brief releases the polling context after its worker exits.
 * @param desktop_hotkey_t* hotkey -> owned context, or NULL
 * @return void
 */
void desktop_hotkey__destroy(desktop_hotkey_t* hotkey)
{
    desktop_memory__erase(hotkey, sizeof(*hotkey));
    free(hotkey);
}

/**
 * @brief reads the chosen physical key without installing a global event tap.
 * @param desktop_hotkey_t* hotkey -> initialized polling context
 * @param int64 key -> printable ASCII key or legacy F-key number
 * @return boole -> nonzero while the authorized key query reports it held
 */
boole desktop_hotkey__down(desktop_hotkey_t* hotkey, int64 key)
{
    static const CGKeyCode functions[12] = {kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12};
    static const CGKeyCode letters[26] =
    {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F,
        kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L,
        kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
        kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X,
        kVK_ANSI_Y, kVK_ANSI_Z
    };
    static const CGKeyCode digits[10] = {kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9};
    CGKeyCode code = kVK_Space;
    if (!hotkey || desktop_hotkey__index(key) < 0)
    {
        return FALSE;
    }
    if (key >= 1 && key <= 12)
    {
        code = functions[key - 1];
    }
    else if (key >= 'A' && key <= 'Z')
    {
        code = letters[key - 'A'];
    }
    else if (key >= '0' && key <= '9')
    {
        code = digits[key - '0'];
    }
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, code);
}
