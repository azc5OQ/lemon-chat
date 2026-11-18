// Push-to-talk helper for Windows, started by the Lemon Chat Electron app (../app/main.js).
//
// On standard input, "watch <keyCode>" lines set the one key to report, as the JavaScript keyCode the page uses.
// 0 means no key. On standard output the program writes "method hook" once, then "down" and "up" when that key is
// pressed and released, whichever program has the keyboard focus. Other keys are not reported.
//
// A low-level keyboard hook (WH_KEYBOARD_LL) observes the keys and passes every key on unchanged. The program ends
// when its standard input is closed, which happens when the app ends.
//
// package_electron.py builds it with the following command, with -target aarch64-windows-gnu for 64-bit ARM and
// -target x86-windows-gnu for 32-bit x86.
// zig cc -target x86_64-windows-gnu -O2 ptt_windows.c -o ptt-helper.exe -luser32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../definitions.h"

// key to report as a JavaScript keyCode, written by the command thread and read by the hook
static volatile LONG watched_key_code = 0;
// whether the reported key is held, so that key repetition while it is held gives one "down"
static boole is_watched_key_down = FALSE;

/**
 * @brief writes one line to standard output at once, so that the app receives it without delay
 *
 * @param text -> the line without its line break
 *
 * @return void
 */
static void ptt__report(const char *text)
{
    // line out
    fputs(text, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/**
 * @brief converts the virtual-key code from the hook to the keyCode the browser reports. The hook reports the
 *        left and right Shift, Ctrl and Alt keys with their own codes, and the browser reports one code for both.
 *        All other virtual-key codes are equal to the browser's keyCode.
 *
 * @param virtual_key -> virtual-key code from the hook
 *
 * @return LONG -> keyCode as the page sees it
 */
static LONG ptt__browser_key_code(DWORD virtual_key)
{
    // left and right modifiers
    switch (virtual_key)
    {
    case VK_LSHIFT:
    case VK_RSHIFT:
        return VK_SHIFT;
    case VK_LCONTROL:
    case VK_RCONTROL:
        return VK_CONTROL;
    case VK_LMENU:
    case VK_RMENU:
        return VK_MENU;
    default:
        return (LONG)virtual_key;
    }
}

/**
 * @brief low-level keyboard hook that reports presses and releases of the watched key and passes every key on
 *
 * @param code -> hook code, HC_ACTION for a key event
 * @param message -> WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN or WM_SYSKEYUP
 * @param data -> pointer to the KBDLLHOOKSTRUCT of the key
 *
 * @return LRESULT -> result of the next hook
 */
static LRESULT CALLBACK ptt__keyboard_hook(int code, WPARAM message, LPARAM data)
{
    // watched key
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT *key = (const KBDLLHOOKSTRUCT *)data;
        LONG watched = watched_key_code;
        if (watched != 0 && ptt__browser_key_code(key->vkCode) == watched)
        {
            if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !is_watched_key_down)
            {
                is_watched_key_down = TRUE;
                ptt__report("down");
            }
            else if (message == WM_KEYUP || message == WM_SYSKEYUP)
            {
                is_watched_key_down = FALSE;
                ptt__report("up");
            }
        }
    }

    // pass it on
    return CallNextHookEx(NULL, code, message, data);
}

/**
 * @brief reads "watch <keyCode>" commands until the app closes standard input, then ends the program
 *
 * @param unused -> thread parameter, not used
 *
 * @return DWORD -> not reached, because the program ends here
 */
static DWORD WINAPI ptt__read_commands(LPVOID unused)
{
    char line[64];
    (void)unused;

    // read commands
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int value = 0;
        if (sscanf(line, "watch %d", &value) == 1)
        {
            InterlockedExchange(&watched_key_code, (LONG)value);
        }
    }

    // app ended
    ExitProcess(0);
    return 0;
}

int main(void)
{
    HANDLE thread;
    HHOOK hook;
    MSG message;

    // command thread
    thread = CreateThread(NULL, 0, ptt__read_commands, NULL, 0, NULL);
    if (thread == NULL)
    {
        ptt__report("error the command thread could not start");
        return 1;
    }

    // keyboard hook
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, ptt__keyboard_hook, GetModuleHandleW(NULL), 0);
    if (hook == NULL)
    {
        ptt__report("error the keyboard hook could not be installed");
        ptt__report("method none");
        WaitForSingleObject(thread, INFINITE);
        return 1;
    }
    ptt__report("method hook");

    // hook messages
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(hook);
    return 0;
}
