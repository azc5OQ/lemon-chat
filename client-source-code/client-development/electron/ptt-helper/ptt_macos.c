// Push-to-talk helper for macOS, started by the Lemon Chat Electron app (../app/main.js).
//
// On standard input, "watch <keyCode>" lines set the one key to report, as the JavaScript keyCode the page uses.
// 0 means no key. On standard output the program writes "method <name>" once, then "down" and "up" when that key is
// pressed and released, whichever program is in front. Other keys are not reported. The program ends when its
// standard input is closed, which happens when the app ends.
//
// There are two ways to see the key.
// - eventtap is a listen-only event tap (CGEventTapCreate). macOS asks the user once to allow input monitoring for
//   the app (System Settings > Privacy & Security > Input Monitoring).
// - poll reads the state of the key every 15 milliseconds (CGEventSourceKeyState) when the tap is not allowed.
//
// package_electron.py builds it on a Mac with
// clang -O2 ptt_macos.c -o ptt-helper -framework ApplicationServices -framework CoreFoundation
#include <ApplicationServices/ApplicationServices.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "../definitions.h"

// JavaScript keyCode of a key and its macOS virtual key code (the kVK_ values of Carbon's Events.h)
static const struct
{
    int key_code;
    int mac_code;
} KEY_TABLE[] = {
    { 8, 0x33 }, { 9, 0x30 }, { 13, 0x24 }, { 16, 0x38 }, { 17, 0x3B }, { 18, 0x3A }, { 20, 0x39 }, { 27, 0x35 }, { 32, 0x31 }, { 33, 0x74 }, { 34, 0x79 }, { 35, 0x77 }, { 36, 0x73 }, { 37, 0x7B }, { 38, 0x7E }, { 39, 0x7C }, { 40, 0x7D }, { 46, 0x75 }, { 48, 0x1D }, { 49, 0x12 }, { 50, 0x13 }, { 51, 0x14 }, { 52, 0x15 }, { 53, 0x17 }, { 54, 0x16 }, { 55, 0x1A }, { 56, 0x1C }, { 57, 0x19 }, { 65, 0x00 }, { 66, 0x0B }, { 67, 0x08 }, { 68, 0x02 }, { 69, 0x0E }, { 70, 0x03 }, { 71, 0x05 }, { 72, 0x04 }, { 73, 0x22 }, { 74, 0x26 }, { 75, 0x28 }, { 76, 0x25 }, { 77, 0x2E }, { 78, 0x2D }, { 79, 0x1F }, { 80, 0x23 }, { 81, 0x0C }, { 82, 0x0F }, { 83, 0x01 }, { 84, 0x11 }, { 85, 0x20 }, { 86, 0x09 }, { 87, 0x0D }, { 88, 0x07 }, { 89, 0x10 }, { 90, 0x06 }, { 91, 0x37 }, { 93, 0x36 }, { 96, 0x52 }, { 97, 0x53 }, { 98, 0x54 }, { 99, 0x55 }, { 100, 0x56 }, { 101, 0x57 }, { 102, 0x58 }, { 103, 0x59 }, { 104, 0x5B }, { 105, 0x5C }, { 106, 0x43 }, { 107, 0x45 }, { 109, 0x4E }, { 110, 0x41 }, { 111, 0x4B }, { 112, 0x7A }, { 113, 0x78 }, { 114, 0x63 }, { 115, 0x76 }, { 116, 0x60 }, { 117, 0x61 }, { 118, 0x62 }, { 119, 0x64 }, { 120, 0x65 }, { 121, 0x6D }, { 122, 0x67 }, { 123, 0x6F }, { 124, 0x69 }, { 125, 0x6B }, { 126, 0x71 }, { 127, 0x6A }, { 128, 0x40 }, { 129, 0x4F }, { 130, 0x50 }, { 131, 0x5A }, { 186, 0x29 }, { 187, 0x18 }, { 188, 0x2B }, { 189, 0x1B }, { 190, 0x2F }, { 191, 0x2C }, { 192, 0x32 }, { 219, 0x21 }, { 220, 0x2A }, { 221, 0x1E }, { 222, 0x27 },
};

// macOS key code of the watched key, -1 for none, written by the command thread
static volatile int watched_mac_code = -1;
// whether the reported key is held, so that a held key gives one "down"
static boole is_watched_key_down = FALSE;
static CFMachPortRef event_tap = NULL;

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
 * @brief reports a change of the watched key's state
 *
 * @param is_down -> TRUE when the key is held
 *
 * @return void
 */
static void ptt__key_state(boole is_down)
{
    // changes only
    if (is_down && !is_watched_key_down)
    {
        is_watched_key_down = TRUE;
        ptt__report("down");
    }
    else if (!is_down && is_watched_key_down)
    {
        is_watched_key_down = FALSE;
        ptt__report("up");
    }
}

/**
 * @brief modifier flag of a modifier key. Modifiers come as flag changes, not as key presses.
 *
 * @param mac_code -> macOS key code
 *
 * @return CGEventFlags -> the flag, 0 for other keys
 */
static CGEventFlags ptt__modifier_flag(int mac_code)
{
    // modifier keys
    switch (mac_code)
    {
    case 0x38:
    case 0x3C:
        return kCGEventFlagMaskShift;
    case 0x3B:
    case 0x3E:
        return kCGEventFlagMaskControl;
    case 0x3A:
    case 0x3D:
        return kCGEventFlagMaskAlternate;
    case 0x37:
    case 0x36:
        return kCGEventFlagMaskCommand;
    case 0x39:
        return kCGEventFlagMaskAlphaShift;
    default:
        return 0;
    }
}

/**
 * @brief listen-only event tap that reports presses and releases of the watched key. The events pass unchanged.
 *
 * @param proxy -> tap proxy, not used
 * @param type -> event type
 * @param event -> the event
 * @param unused -> user data, not used
 *
 * @return CGEventRef -> the unchanged event
 */
static CGEventRef ptt__event_tap(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *unused)
{
    int watched = watched_mac_code;
    int mac_code;
    (void)proxy;
    (void)unused;

    // tap switched off
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput)
    {
        CGEventTapEnable(event_tap, true);
        return event;
    }

    // watched key
    mac_code = (int)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    if (watched < 0 || mac_code != watched)
    {
        return event;
    }
    if (type == kCGEventKeyDown)
    {
        ptt__key_state(TRUE);
    }
    else if (type == kCGEventKeyUp)
    {
        ptt__key_state(FALSE);
    }
    else if (type == kCGEventFlagsChanged)
    {
        ptt__key_state((CGEventGetFlags(event) & ptt__modifier_flag(mac_code)) != 0);
    }
    return event;
}

/**
 * @brief timer of the poll method that reads the state of the watched key
 *
 * @param timer -> the timer, not used
 * @param unused -> user data, not used
 *
 * @return void
 */
static void ptt__poll_key(CFRunLoopTimerRef timer, void *unused)
{
    int watched = watched_mac_code;
    (void)timer;
    (void)unused;

    // key state
    if (watched >= 0)
    {
        ptt__key_state(CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, (CGKeyCode)watched) ? TRUE : FALSE);
    }
}

/**
 * @brief reads "watch <keyCode>" commands until the app closes standard input, then ends the program
 *
 * @param unused -> thread parameter, not used
 *
 * @return void* -> not reached, because the program ends here
 */
static void *ptt__read_commands(void *unused)
{
    char line[64];
    (void)unused;

    // read commands
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int value = 0;
        if (sscanf(line, "watch %d", &value) == 1)
        {
            nuint index;
            int mac_code = -1;
            for (index = 0; index < sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]); index++)
            {
                if (KEY_TABLE[index].key_code == value)
                {
                    mac_code = KEY_TABLE[index].mac_code;
                }
            }
            watched_mac_code = mac_code;
        }
    }

    // app ended
    exit(0);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);

    // command thread
    if (pthread_create(&thread, NULL, ptt__read_commands, NULL) != 0)
    {
        ptt__report("error the command thread could not start");
        return 1;
    }

    // event tap
    event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask, ptt__event_tap, NULL);
    if (event_tap != NULL)
    {
        CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, event_tap, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
        CGEventTapEnable(event_tap, true);
        ptt__report("method eventtap");
    }

    // polling instead
    else
    {
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 0.015, 0, 0, ptt__poll_key, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
        ptt__report("error eventtap is not allowed because input monitoring is off for the app. The key state is read instead");
        ptt__report("method poll");
    }
    CFRunLoopRun();
    return 0;
}
