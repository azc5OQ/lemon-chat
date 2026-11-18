// Push-to-talk helper for Linux, started by the Lemon Chat Electron app (../app/main.js).
//
// On standard input, "watch <keyCode>" lines set the one key to report, as the JavaScript keyCode the page uses.
// 0 means no key. On standard output the program writes "method <name>" once, then "down" and "up" when that key is
// pressed and released, whichever program has the keyboard focus, and "error <text>" lines that explain a way that
// cannot be used. Other keys are not reported. The program ends when its standard input is closed, which happens
// when the app ends.
//
// There are three ways to see the key. A Wayland session tries them in this order, an X11 session tries x11 first.
// - evdev reads the keyboard devices in /dev/input. It works in X11 and Wayland sessions when the user may read
//   them, for example as a member of the "input" group (sudo usermod -aG input $USER, then log in again).
// - portal uses the GlobalShortcuts portal of xdg-desktop-portal (Wayland sessions of KDE Plasma, GNOME 48 and
//   newer). The desktop shows a dialog once, in which the user confirms or changes the key. The page's key is
//   suggested. Press and release arrive as the Activated and Deactivated signals. It uses libdbus-1.
// - x11 selects the XInput 2 raw key events of the root window, which report every key in an X11 session. In a
//   Wayland session they arrive only while an X11 program has the focus. It uses libX11 and libXi.
// The libraries are loaded with dlopen when a way is tried. So the program needs only the C library to build and to
// start, and a missing library only removes its way.
//
// package_electron.py builds it with the following command, with -target aarch64-linux-gnu.2.17 for 64-bit ARM and
// -target arm-linux-gnueabihf.2.17 for 32-bit ARM.
// zig cc -target x86_64-linux-gnu.2.17 -O2 ptt_linux.c -o ptt-helper -ldl
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "../definitions.h"

#define PTT_MAX_DEVICES 32
// highest key code of the Linux input layer (KEY_MAX) and the code of the A key (KEY_A)
#define PTT_KEY_MAX 0x2ff
#define PTT_KEY_A 30
// EVIOCGBIT(EV_KEY, length) reads which keys a device has
#define PTT_EVIOCGBIT_KEY(length) ((2u << 30) | ((uint)(length) << 16) | ('E' << 8) | (0x20 + 1))
#define PTT_BITS_PER_LONG (8 * sizeof(nuint))

// event record of the Linux input layer (struct input_event). Its time fields have the size of long, which is the
// size of a pointer on Linux, so nint matches them in 32-bit and 64-bit programs.
struct ptt_input_event
{
    nint seconds;
    nint microseconds;
    ushort type;
    ushort code;
    int value;
};

// JavaScript keyCode of a key and its Linux input key codes (left and right variant, 0 when there is one key)
static const struct
{
    int key_code;
    int first;
    int second;
} KEY_TABLE[] = {
    { 8, 14, 0 }, { 9, 15, 0 }, { 13, 28, 96 }, { 16, 42, 54 }, { 17, 29, 97 }, { 18, 56, 100 }, { 19, 119, 0 }, { 20, 58, 0 }, { 27, 1, 0 }, { 32, 57, 0 }, { 33, 104, 0 }, { 34, 109, 0 }, { 35, 107, 0 }, { 36, 102, 0 }, { 37, 105, 0 }, { 38, 103, 0 }, { 39, 106, 0 }, { 40, 108, 0 }, { 44, 99, 0 }, { 45, 110, 0 }, { 46, 111, 0 }, { 48, 11, 0 }, { 49, 2, 0 }, { 50, 3, 0 }, { 51, 4, 0 }, { 52, 5, 0 }, { 53, 6, 0 }, { 54, 7, 0 }, { 55, 8, 0 }, { 56, 9, 0 }, { 57, 10, 0 }, { 65, 30, 0 }, { 66, 48, 0 }, { 67, 46, 0 }, { 68, 32, 0 }, { 69, 18, 0 }, { 70, 33, 0 }, { 71, 34, 0 }, { 72, 35, 0 }, { 73, 23, 0 }, { 74, 36, 0 }, { 75, 37, 0 }, { 76, 38, 0 }, { 77, 50, 0 }, { 78, 49, 0 }, { 79, 24, 0 }, { 80, 25, 0 }, { 81, 16, 0 }, { 82, 19, 0 }, { 83, 31, 0 }, { 84, 20, 0 }, { 85, 22, 0 }, { 86, 47, 0 }, { 87, 17, 0 }, { 88, 45, 0 }, { 89, 21, 0 }, { 90, 44, 0 }, { 91, 125, 0 }, { 92, 126, 0 }, { 93, 127, 0 }, { 96, 82, 0 }, { 97, 79, 0 }, { 98, 80, 0 }, { 99, 81, 0 }, { 100, 75, 0 }, { 101, 76, 0 }, { 102, 77, 0 }, { 103, 71, 0 }, { 104, 72, 0 }, { 105, 73, 0 }, { 106, 55, 0 }, { 107, 78, 0 }, { 109, 74, 0 }, { 110, 83, 0 }, { 111, 98, 0 }, { 112, 59, 0 }, { 113, 60, 0 }, { 114, 61, 0 }, { 115, 62, 0 }, { 116, 63, 0 }, { 117, 64, 0 }, { 118, 65, 0 }, { 119, 66, 0 }, { 120, 67, 0 }, { 121, 68, 0 }, { 122, 87, 0 }, { 123, 88, 0 }, { 124, 183, 0 }, { 125, 184, 0 }, { 126, 185, 0 }, { 127, 186, 0 }, { 128, 187, 0 }, { 129, 188, 0 }, { 130, 189, 0 }, { 131, 190, 0 }, { 132, 191, 0 }, { 133, 192, 0 }, { 134, 193, 0 }, { 135, 194, 0 }, { 144, 69, 0 }, { 145, 70, 0 }, { 186, 39, 0 }, { 187, 13, 0 }, { 188, 51, 0 }, { 189, 12, 0 }, { 190, 52, 0 }, { 191, 53, 0 }, { 192, 41, 0 }, { 219, 26, 0 }, { 220, 43, 0 }, { 221, 27, 0 }, { 222, 40, 0 },
};

// key to report as a JavaScript keyCode and its Linux input key codes
static int watched_key_code = 0;
static int watched_codes[2] = { 0, 0 };
// whether the reported key is held, so that a held key gives one "down"
static boole is_watched_key_down = FALSE;

static int evdev_descriptors[PTT_MAX_DEVICES];
static int evdev_count = 0;
// whether a keyboard device exists that the user may not read
static boole is_evdev_denied = FALSE;

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
 * @brief sets the key to report
 *
 * @param key_code -> JavaScript keyCode, 0 for no key
 *
 * @return void
 */
static void ptt__watch(int key_code)
{
    nuint index;

    // key codes from the table
    watched_key_code = key_code;
    watched_codes[0] = 0;
    watched_codes[1] = 0;
    is_watched_key_down = FALSE;
    for (index = 0; index < sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]); index++)
    {
        if (KEY_TABLE[index].key_code == key_code)
        {
            watched_codes[0] = KEY_TABLE[index].first;
            watched_codes[1] = KEY_TABLE[index].second;
        }
    }
}

/**
 * @brief reports a press or release when it belongs to the watched key
 *
 * @param code -> Linux input key code
 * @param is_press -> TRUE for a press, FALSE for a release
 *
 * @return void
 */
static void ptt__key_event(int code, boole is_press)
{
    // watched key only
    if (code == 0 || (code != watched_codes[0] && code != watched_codes[1]))
    {
        return;
    }

    // press or release
    if (is_press && !is_watched_key_down)
    {
        is_watched_key_down = TRUE;
        ptt__report("down");
    }
    else if (!is_press)
    {
        is_watched_key_down = FALSE;
        ptt__report("up");
    }
}

/**
 * @brief opens every keyboard device in /dev/input that the user may read
 *
 * @return int -> 1 when at least one keyboard device is open
 */
static int ptt__start_evdev(void)
{
    DIR *directory;
    struct dirent *entry;

    // device folder
    directory = opendir("/dev/input");
    if (directory == NULL)
    {
        return 0;
    }

    // keyboards only
    while ((entry = readdir(directory)) != NULL && evdev_count < PTT_MAX_DEVICES)
    {
        char path[300];
        nuint bits[PTT_KEY_MAX / PTT_BITS_PER_LONG + 1];
        int descriptor;

        if (strncmp(entry->d_name, "event", 5) != 0)
        {
            continue;
        }
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor < 0)
        {
            if (errno == EACCES || errno == EPERM)
            {
                is_evdev_denied = TRUE;
            }
            continue;
        }
        memset(bits, 0, sizeof(bits));
        if (ioctl(descriptor, PTT_EVIOCGBIT_KEY(sizeof(bits)), bits) < 0 || (bits[PTT_KEY_A / PTT_BITS_PER_LONG] & (1UL << (PTT_KEY_A % PTT_BITS_PER_LONG))) == 0)
        {
            close(descriptor);
            continue;
        }
        evdev_descriptors[evdev_count++] = descriptor;
    }
    closedir(directory);
    return evdev_count > 0;
}

/**
 * @brief handles the waiting key events of one keyboard device. Key repetition is ignored.
 *
 * @param descriptor -> the open device
 *
 * @return void
 */
static void ptt__read_evdev(int descriptor)
{
    struct ptt_input_event events[64];
    nint size;

    // waiting events
    while ((size = read(descriptor, events, sizeof(events))) > 0)
    {
        nuint index;
        for (index = 0; index < (nuint)size / sizeof(events[0]); index++)
        {
            if (events[index].type == 1 && events[index].value != 2)
            {
                ptt__key_event(events[index].code, events[index].value == 1);
            }
        }
    }
}

// the parts of the Xlib and XInput 2 types that are read here, with the layouts of Xlib.h and XInput2.h. The long and
// unsigned long fields of those headers are nint and nuint here, which have the same size on Linux.
typedef struct ptt_display ptt_display;
typedef struct
{
    int type;
    nuint serial;
    int send_event;
    ptt_display *display;
    int extension;
    int evtype;
    uint cookie;
    void *data;
} ptt_x_cookie;
typedef union
{
    int type;
    ptt_x_cookie cookie;
    nint pad[24];
} ptt_x_event;
typedef struct
{
    int deviceid;
    int mask_len;
    ubyte *mask;
} ptt_xi_event_mask;
typedef struct
{
    int type;
    nuint serial;
    int send_event;
    ptt_display *display;
    int extension;
    int evtype;
    nuint time;
    int deviceid;
    int sourceid;
    int detail;
    int flags;
} ptt_xi_raw_event;

// X11 functions loaded with dlopen, and the connection
static struct
{
    ptt_display *(*open_display)(const char *name);
    nuint (*default_root_window)(ptt_display *display);
    int (*query_extension)(ptt_display *display, const char *name, int *opcode, int *event, int *error);
    int (*connection_number)(ptt_display *display);
    int (*pending)(ptt_display *display);
    int (*next_event)(ptt_display *display, ptt_x_event *event);
    int (*get_event_data)(ptt_display *display, ptt_x_cookie *cookie);
    void (*free_event_data)(ptt_display *display, ptt_x_cookie *cookie);
    int (*flush)(ptt_display *display);
    int (*xi_query_version)(ptt_display *display, int *major, int *minor);
    int (*xi_select_events)(ptt_display *display, nuint window, ptt_xi_event_mask *masks, int count);
    ptt_display *display;
    int opcode;
} x11;

/**
 * @brief connects to the X server and asks for the raw key events of all keyboards (XI_RawKeyPress 13,
 *        XI_RawKeyRelease 14, for XIAllMasterDevices 1)
 *
 * @return int -> 1 when the raw key events are selected
 */
static int ptt__start_x11(void)
{
    void *x_library;
    void *xi_library;
    ubyte mask_bits[4] = { 0, 0, 0, 0 };
    ptt_xi_event_mask mask;
    int event = 0;
    int error = 0;
    int major = 2;
    int minor = 0;

    // libraries and display
    if (getenv("DISPLAY") == NULL)
    {
        return 0;
    }
    x_library = dlopen("libX11.so.6", RTLD_NOW);
    xi_library = dlopen("libXi.so.6", RTLD_NOW);
    if (x_library == NULL || xi_library == NULL)
    {
        ptt__report("error x11 needs libX11.so.6 and libXi.so.6, which are not installed");
        return 0;
    }
    *(void **)&x11.open_display = dlsym(x_library, "XOpenDisplay");
    *(void **)&x11.default_root_window = dlsym(x_library, "XDefaultRootWindow");
    *(void **)&x11.query_extension = dlsym(x_library, "XQueryExtension");
    *(void **)&x11.connection_number = dlsym(x_library, "XConnectionNumber");
    *(void **)&x11.pending = dlsym(x_library, "XPending");
    *(void **)&x11.next_event = dlsym(x_library, "XNextEvent");
    *(void **)&x11.get_event_data = dlsym(x_library, "XGetEventData");
    *(void **)&x11.free_event_data = dlsym(x_library, "XFreeEventData");
    *(void **)&x11.flush = dlsym(x_library, "XFlush");
    *(void **)&x11.xi_query_version = dlsym(xi_library, "XIQueryVersion");
    *(void **)&x11.xi_select_events = dlsym(xi_library, "XISelectEvents");
    if (x11.open_display == NULL || x11.default_root_window == NULL || x11.query_extension == NULL || x11.connection_number == NULL || x11.pending == NULL || x11.next_event == NULL || x11.get_event_data == NULL || x11.free_event_data == NULL || x11.flush == NULL || x11.xi_query_version == NULL || x11.xi_select_events == NULL)
    {
        return 0;
    }
    x11.display = x11.open_display(NULL);
    if (x11.display == NULL)
    {
        return 0;
    }

    // XInput 2 present
    if (!x11.query_extension(x11.display, "XInputExtension", &x11.opcode, &event, &error) || x11.xi_query_version(x11.display, &major, &minor) != 0 || major < 2)
    {
        ptt__report("error x11 needs XInput 2, which the X server does not have");
        return 0;
    }

    // raw key events
    mask_bits[13 >> 3] |= (ubyte)(1 << (13 & 7));
    mask_bits[14 >> 3] |= (ubyte)(1 << (14 & 7));
    mask.deviceid = 1;
    mask.mask_len = (int)sizeof(mask_bits);
    mask.mask = mask_bits;
    x11.xi_select_events(x11.display, x11.default_root_window(x11.display), &mask, 1);
    x11.flush(x11.display);
    return 1;
}

/**
 * @brief handles the waiting X events. An X key code is the Linux input key code plus 8.
 *
 * @return void
 */
static void ptt__read_x11(void)
{
    ptt_x_event event;

    // waiting events
    while (x11.pending(x11.display) > 0)
    {
        x11.next_event(x11.display, &event);
        if (event.cookie.type == 35 && event.cookie.extension == x11.opcode && x11.get_event_data(x11.display, &event.cookie))
        {
            if (event.cookie.evtype == 13 || event.cookie.evtype == 14)
            {
                const ptt_xi_raw_event *raw = (const ptt_xi_raw_event *)event.cookie.data;
                ptt__key_event(raw->detail - 8, event.cookie.evtype == 13);
            }
            x11.free_event_data(x11.display, &event.cookie);
        }
    }
}

// the parts of the libdbus types that are used here, with the layouts of dbus-errors.h and dbus-message.h
typedef struct ptt_dbus_connection ptt_dbus_connection;
typedef struct ptt_dbus_message ptt_dbus_message;
typedef struct
{
    const char *name;
    const char *message;
    uint dummy;
    void *padding;
} ptt_dbus_error;
typedef struct
{
    void *dummy1;
    void *dummy2;
    uint dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    void *pad2;
    void *pad3;
} ptt_dbus_iter;

// libdbus functions loaded with dlopen, and the state of the shortcut session
static struct
{
    void (*error_init)(ptt_dbus_error *error);
    uint (*error_is_set)(const ptt_dbus_error *error);
    void (*error_free)(ptt_dbus_error *error);
    ptt_dbus_connection *(*bus_get)(int type, ptt_dbus_error *error);
    void (*bus_add_match)(ptt_dbus_connection *connection, const char *rule, ptt_dbus_error *error);
    ptt_dbus_message *(*new_method_call)(const char *destination, const char *path, const char *interface, const char *method);
    void (*message_unref)(ptt_dbus_message *message);
    void (*iter_init_append)(ptt_dbus_message *message, ptt_dbus_iter *iter);
    uint (*iter_append_basic)(ptt_dbus_iter *iter, int type, const void *value);
    uint (*iter_open_container)(ptt_dbus_iter *iter, int type, const char *signature, ptt_dbus_iter *sub);
    uint (*iter_close_container)(ptt_dbus_iter *iter, ptt_dbus_iter *sub);
    uint (*iter_init)(ptt_dbus_message *message, ptt_dbus_iter *iter);
    int (*iter_get_arg_type)(ptt_dbus_iter *iter);
    void (*iter_get_basic)(ptt_dbus_iter *iter, void *value);
    uint (*iter_next)(ptt_dbus_iter *iter);
    void (*iter_recurse)(ptt_dbus_iter *iter, ptt_dbus_iter *sub);
    ptt_dbus_message *(*send_with_reply_and_block)(ptt_dbus_connection *connection, ptt_dbus_message *message, int milliseconds, ptt_dbus_error *error);
    uint (*get_unix_fd)(ptt_dbus_connection *connection, int *descriptor);
    uint (*read_write)(ptt_dbus_connection *connection, int milliseconds);
    ptt_dbus_message *(*pop_message)(ptt_dbus_connection *connection);
    uint (*is_signal)(ptt_dbus_message *message, const char *interface, const char *name);
    const char *(*get_path)(ptt_dbus_message *message);
    ptt_dbus_connection *connection;
    char session_handle[256];
    boole is_bound;
    boole is_bind_tried;
} portal;

/**
 * @brief appends an a{sv} dictionary whose values are strings
 *
 * @param iter -> where to append
 * @param pairs -> key, value, key, value, ...
 * @param count -> number of key and value pairs
 *
 * @return void
 */
static void ptt__portal_append_options(ptt_dbus_iter *iter, const char *const *pairs, int count)
{
    ptt_dbus_iter array;
    int index;

    // string dictionary
    portal.iter_open_container(iter, 'a', "{sv}", &array);
    for (index = 0; index < count; index++)
    {
        ptt_dbus_iter entry;
        ptt_dbus_iter variant;
        const char *key = pairs[index * 2];
        const char *value = pairs[index * 2 + 1];

        portal.iter_open_container(&array, 'e', NULL, &entry);
        portal.iter_append_basic(&entry, 's', &key);
        portal.iter_open_container(&entry, 'v', "s", &variant);
        portal.iter_append_basic(&variant, 's', &value);
        portal.iter_close_container(&entry, &variant);
        portal.iter_close_container(&array, &entry);
    }
    portal.iter_close_container(iter, &array);
}

/**
 * @brief sends a portal request and returns the object path of its Request object, whose Response signal carries
 *        the result
 *
 * @param message -> the method call, released here
 * @param path -> receives the object path
 * @param size -> size of path
 *
 * @return int -> 1 when the portal accepted the request
 */
static int ptt__portal_send(ptt_dbus_message *message, char *path, nuint size)
{
    ptt_dbus_error error;
    ptt_dbus_message *reply;
    ptt_dbus_iter iter;
    const char *value = NULL;

    // send and wait
    portal.error_init(&error);
    reply = portal.send_with_reply_and_block(portal.connection, message, 5000, &error);
    portal.message_unref(message);
    if (reply == NULL)
    {
        portal.error_free(&error);
        return 0;
    }

    // request path
    if (portal.iter_init(reply, &iter) && portal.iter_get_arg_type(&iter) == 'o')
    {
        portal.iter_get_basic(&iter, &value);
        snprintf(path, size, "%s", value != NULL ? value : "");
    }
    portal.message_unref(reply);
    return value != NULL;
}

/**
 * @brief waits for the Response signal of a portal request
 *
 * @param request_path -> object path of the Request object
 * @param seconds -> longest wait, as a request that shows a dialog waits for the user
 *
 * @return ptt_dbus_message* -> the signal, to be released by the caller, or NULL
 */
static ptt_dbus_message *ptt__portal_wait_response(const char *request_path, int seconds)
{
    time_t end = time(NULL) + seconds;

    // signals until the answer
    while (time(NULL) < end)
    {
        ptt_dbus_message *message;
        if (!portal.read_write(portal.connection, 500))
        {
            return NULL;
        }
        while ((message = portal.pop_message(portal.connection)) != NULL)
        {
            const char *path = portal.get_path(message);
            if (portal.is_signal(message, "org.freedesktop.portal.Request", "Response") && path != NULL && strcmp(path, request_path) == 0)
            {
                return message;
            }
            portal.message_unref(message);
        }
    }
    return NULL;
}

/**
 * @brief reads the result code of a Response signal (0 = success) and, when wanted, a string from its results
 *
 * @param message -> the Response signal
 * @param key -> result to read, or NULL
 * @param value -> receives the result
 * @param size -> size of value
 *
 * @return uint -> the result code, or 2 when the signal cannot be read
 */
static uint ptt__portal_response(ptt_dbus_message *message, const char *key, char *value, nuint size)
{
    ptt_dbus_iter iter;
    ptt_dbus_iter results;
    uint code = 2;

    // result code
    if (!portal.iter_init(message, &iter) || portal.iter_get_arg_type(&iter) != 'u')
    {
        return 2;
    }
    portal.iter_get_basic(&iter, &code);
    if (key == NULL || !portal.iter_next(&iter) || portal.iter_get_arg_type(&iter) != 'a')
    {
        return code;
    }

    // wanted result
    portal.iter_recurse(&iter, &results);
    while (portal.iter_get_arg_type(&results) == 'e')
    {
        ptt_dbus_iter entry;
        ptt_dbus_iter variant;
        const char *name = NULL;

        portal.iter_recurse(&results, &entry);
        portal.iter_get_basic(&entry, &name);
        if (name != NULL && strcmp(name, key) == 0 && portal.iter_next(&entry) && portal.iter_get_arg_type(&entry) == 'v')
        {
            int type;
            portal.iter_recurse(&entry, &variant);
            type = portal.iter_get_arg_type(&variant);
            if (type == 's' || type == 'o')
            {
                const char *text = NULL;
                portal.iter_get_basic(&variant, &text);
                snprintf(value, size, "%s", text != NULL ? text : "");
            }
        }
        if (!portal.iter_next(&results))
        {
            break;
        }
    }
    return code;
}

/**
 * @brief connects to the session bus and creates a GlobalShortcuts session. The shortcut itself is bound when the
 *        page's key is known (ptt__bind_portal)
 *
 * @return int -> 1 when the session exists
 */
static int ptt__start_portal(void)
{
    static const char *const SESSION_OPTIONS[] = { "handle_token", "lemonchat_session", "session_handle_token", "lemonchat_ptt" };
    void *library;
    ptt_dbus_error error;
    ptt_dbus_message *message;
    ptt_dbus_iter iter;
    char request_path[256] = "";

    // library functions
    library = dlopen("libdbus-1.so.3", RTLD_NOW);
    if (library == NULL)
    {
        return 0;
    }
    *(void **)&portal.error_init = dlsym(library, "dbus_error_init");
    *(void **)&portal.error_is_set = dlsym(library, "dbus_error_is_set");
    *(void **)&portal.error_free = dlsym(library, "dbus_error_free");
    *(void **)&portal.bus_get = dlsym(library, "dbus_bus_get");
    *(void **)&portal.bus_add_match = dlsym(library, "dbus_bus_add_match");
    *(void **)&portal.new_method_call = dlsym(library, "dbus_message_new_method_call");
    *(void **)&portal.message_unref = dlsym(library, "dbus_message_unref");
    *(void **)&portal.iter_init_append = dlsym(library, "dbus_message_iter_init_append");
    *(void **)&portal.iter_append_basic = dlsym(library, "dbus_message_iter_append_basic");
    *(void **)&portal.iter_open_container = dlsym(library, "dbus_message_iter_open_container");
    *(void **)&portal.iter_close_container = dlsym(library, "dbus_message_iter_close_container");
    *(void **)&portal.iter_init = dlsym(library, "dbus_message_iter_init");
    *(void **)&portal.iter_get_arg_type = dlsym(library, "dbus_message_iter_get_arg_type");
    *(void **)&portal.iter_get_basic = dlsym(library, "dbus_message_iter_get_basic");
    *(void **)&portal.iter_next = dlsym(library, "dbus_message_iter_next");
    *(void **)&portal.iter_recurse = dlsym(library, "dbus_message_iter_recurse");
    *(void **)&portal.send_with_reply_and_block = dlsym(library, "dbus_connection_send_with_reply_and_block");
    *(void **)&portal.get_unix_fd = dlsym(library, "dbus_connection_get_unix_fd");
    *(void **)&portal.read_write = dlsym(library, "dbus_connection_read_write");
    *(void **)&portal.pop_message = dlsym(library, "dbus_connection_pop_message");
    *(void **)&portal.is_signal = dlsym(library, "dbus_message_is_signal");
    *(void **)&portal.get_path = dlsym(library, "dbus_message_get_path");
    if (portal.error_init == NULL || portal.error_is_set == NULL || portal.error_free == NULL || portal.bus_get == NULL || portal.bus_add_match == NULL || portal.new_method_call == NULL || portal.message_unref == NULL || portal.iter_init_append == NULL || portal.iter_append_basic == NULL || portal.iter_open_container == NULL || portal.iter_close_container == NULL || portal.iter_init == NULL || portal.iter_get_arg_type == NULL || portal.iter_get_basic == NULL || portal.iter_next == NULL || portal.iter_recurse == NULL || portal.send_with_reply_and_block == NULL || portal.get_unix_fd == NULL || portal.read_write == NULL || portal.pop_message == NULL || portal.is_signal == NULL || portal.get_path == NULL)
    {
        return 0;
    }

    // session bus
    portal.error_init(&error);
    portal.connection = portal.bus_get(0, &error);
    if (portal.connection == NULL)
    {
        portal.error_free(&error);
        return 0;
    }
    portal.bus_add_match(portal.connection, "type='signal',interface='org.freedesktop.portal.Request',member='Response'", &error);
    portal.bus_add_match(portal.connection, "type='signal',interface='org.freedesktop.portal.GlobalShortcuts'", &error);
    if (portal.error_is_set(&error))
    {
        portal.error_free(&error);
        return 0;
    }

    // shortcut session
    message = portal.new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "CreateSession");
    if (message == NULL)
    {
        return 0;
    }
    portal.iter_init_append(message, &iter);
    ptt__portal_append_options(&iter, SESSION_OPTIONS, 2);
    if (!ptt__portal_send(message, request_path, sizeof(request_path)))
    {
        return 0;
    }
    message = ptt__portal_wait_response(request_path, 30);
    if (message == NULL)
    {
        return 0;
    }
    if (ptt__portal_response(message, "session_handle", portal.session_handle, sizeof(portal.session_handle)) != 0 || portal.session_handle[0] == '\0')
    {
        portal.message_unref(message);
        return 0;
    }
    portal.message_unref(message);
    return 1;
}

/**
 * @brief name of a key in the form the shortcut portal accepts as a suggested trigger (an XKB key name)
 *
 * @param key_code -> JavaScript keyCode
 * @param name -> receives the name, empty when the key has no simple name
 * @param size -> size of name
 *
 * @return void
 */
static void ptt__portal_trigger(int key_code, char *name, nuint size)
{
    // letters, digits, function keys
    name[0] = '\0';
    if (key_code >= 65 && key_code <= 90)
    {
        snprintf(name, size, "%c", 'a' + (key_code - 65));
    }
    else if (key_code >= 48 && key_code <= 57)
    {
        snprintf(name, size, "%c", '0' + (key_code - 48));
    }
    else if (key_code >= 112 && key_code <= 135)
    {
        snprintf(name, size, "F%d", key_code - 111);
    }
    else if (key_code == 32)
    {
        snprintf(name, size, "space");
    }
}

/**
 * @brief binds the push-to-talk shortcut in the portal session. The desktop shows a dialog in which the user
 *        confirms or changes the suggested key, so this waits for up to five minutes.
 *
 * @return int -> 1 when the shortcut is bound
 */
static int ptt__bind_portal(void)
{
    static const char *const BIND_OPTIONS[] = { "handle_token", "lemonchat_bind" };
    ptt_dbus_message *message;
    ptt_dbus_iter iter;
    ptt_dbus_iter array;
    ptt_dbus_iter structure;
    const char *session = portal.session_handle;
    const char *shortcut = "push-to-talk";
    const char *parent = "";
    const char *shortcut_options[4];
    char trigger[16];
    char request_path[256] = "";
    uint code;

    // shortcut description
    portal.is_bind_tried = TRUE;
    ptt__portal_trigger(watched_key_code, trigger, sizeof(trigger));
    shortcut_options[0] = "description";
    shortcut_options[1] = "Push to talk (Lemon Chat)";
    shortcut_options[2] = "preferred_trigger";
    shortcut_options[3] = trigger;
    message = portal.new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts");
    if (message == NULL)
    {
        return 0;
    }
    portal.iter_init_append(message, &iter);
    portal.iter_append_basic(&iter, 'o', &session);
    portal.iter_open_container(&iter, 'a', "(sa{sv})", &array);
    portal.iter_open_container(&array, 'r', NULL, &structure);
    portal.iter_append_basic(&structure, 's', &shortcut);
    ptt__portal_append_options(&structure, shortcut_options, trigger[0] != '\0' ? 2 : 1);
    portal.iter_close_container(&array, &structure);
    portal.iter_close_container(&iter, &array);
    portal.iter_append_basic(&iter, 's', &parent);
    ptt__portal_append_options(&iter, BIND_OPTIONS, 1);

    // user answer
    if (!ptt__portal_send(message, request_path, sizeof(request_path)))
    {
        return 0;
    }
    message = ptt__portal_wait_response(request_path, 300);
    if (message == NULL)
    {
        return 0;
    }
    code = ptt__portal_response(message, NULL, NULL, 0);
    portal.message_unref(message);
    portal.is_bound = (code == 0);
    return portal.is_bound;
}

/**
 * @brief handles the waiting portal signals. Activated is a press of the shortcut, Deactivated its release.
 *
 * @return void
 */
static void ptt__read_portal(void)
{
    ptt_dbus_message *message;

    // waiting signals
    portal.read_write(portal.connection, 0);
    while ((message = portal.pop_message(portal.connection)) != NULL)
    {
        boole is_activated = portal.is_signal(message, "org.freedesktop.portal.GlobalShortcuts", "Activated") != 0;
        boole is_deactivated = portal.is_signal(message, "org.freedesktop.portal.GlobalShortcuts", "Deactivated") != 0;
        ptt_dbus_iter iter;
        const char *shortcut = NULL;

        if ((is_activated || is_deactivated) && portal.iter_init(message, &iter) && portal.iter_next(&iter) && portal.iter_get_arg_type(&iter) == 's')
        {
            portal.iter_get_basic(&iter, &shortcut);
            if (shortcut != NULL && strcmp(shortcut, "push-to-talk") == 0)
            {
                if (is_activated && !is_watched_key_down)
                {
                    is_watched_key_down = TRUE;
                    ptt__report("down");
                }
                else if (is_deactivated)
                {
                    is_watched_key_down = FALSE;
                    ptt__report("up");
                }
            }
        }
        portal.message_unref(message);
    }
}

/**
 * @brief handles the complete lines in the command buffer
 *
 * @param buffer -> received bytes
 * @param used -> number of bytes in buffer, updated to the bytes of an unfinished line
 *
 * @return void
 */
static void ptt__handle_commands(char *buffer, nuint *used)
{
    char *start = buffer;
    char *end;

    // complete lines
    while ((end = memchr(start, '\n', (nuint)(buffer + *used - start))) != NULL)
    {
        int value = 0;
        *end = '\0';
        if (sscanf(start, "watch %d", &value) == 1)
        {
            ptt__watch(value);
        }
        start = end + 1;
    }

    // keep the rest
    *used = (nuint)(buffer + *used - start);
    memmove(buffer, start, *used);
}

int main(void)
{
    const char *method = "none";
    const char *session_type = getenv("XDG_SESSION_TYPE");
    boole is_wayland = getenv("WAYLAND_DISPLAY") != NULL || (session_type != NULL && strcmp(session_type, "wayland") == 0);
    char buffer[256];
    nuint used = 0;

    // way to see the key
    if (is_wayland)
    {
        if (ptt__start_evdev())
        {
            method = "evdev";
        }
        else if (ptt__start_portal())
        {
            method = "portal";
        }
        else if (ptt__start_x11())
        {
            method = "x11";
        }
    }
    else
    {
        if (ptt__start_x11())
        {
            method = "x11";
        }
        else if (ptt__start_evdev())
        {
            method = "evdev";
        }
        else if (ptt__start_portal())
        {
            method = "portal";
        }
    }
    if (is_evdev_denied && strcmp(method, "evdev") != 0)
    {
        ptt__report("error evdev cannot read /dev/input. Members of the input group can use it");
    }
    if (is_wayland && strcmp(method, "x11") == 0)
    {
        ptt__report("error x11 sees the key in a Wayland session only while an X11 program has the focus");
    }
    printf("method %s\n", method);
    fflush(stdout);

    // commands and keys
    for (;;)
    {
        struct pollfd descriptors[PTT_MAX_DEVICES + 2];
        int count = 0;
        int index;

        descriptors[count].fd = 0;
        descriptors[count++].events = POLLIN;
        if (strcmp(method, "evdev") == 0)
        {
            for (index = 0; index < evdev_count; index++)
            {
                descriptors[count].fd = evdev_descriptors[index];
                descriptors[count++].events = POLLIN;
            }
        }
        else if (strcmp(method, "x11") == 0)
        {
            descriptors[count].fd = x11.connection_number(x11.display);
            descriptors[count++].events = POLLIN;
            ptt__read_x11();
        }
        else if (strcmp(method, "portal") == 0 && portal.get_unix_fd(portal.connection, &descriptors[count].fd))
        {
            descriptors[count++].events = POLLIN;
            ptt__read_portal();
        }
        for (index = 0; index < count; index++)
        {
            descriptors[index].revents = 0;
        }
        if (poll(descriptors, (nfds_t)count, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return 1;
        }

        // app commands
        if (descriptors[0].revents != 0)
        {
            nint size = read(0, buffer + used, sizeof(buffer) - used - 1);
            if (size <= 0)
            {
                return 0;
            }
            used += (nuint)size;
            ptt__handle_commands(buffer, &used);
            if (used >= sizeof(buffer) - 1)
            {
                used = 0;
            }
            if (strcmp(method, "portal") == 0 && watched_key_code != 0 && !portal.is_bind_tried && !ptt__bind_portal())
            {
                ptt__report("error portal shortcut was not confirmed");
                method = ptt__start_x11() ? "x11" : "none";
                printf("method %s\n", method);
                fflush(stdout);
            }
        }

        // key events
        for (index = 1; index < count; index++)
        {
            if (descriptors[index].revents == 0)
            {
                continue;
            }
            if (strcmp(method, "evdev") == 0)
            {
                ptt__read_evdev(descriptors[index].fd);
            }
            else if (strcmp(method, "x11") == 0)
            {
                ptt__read_x11();
            }
            else if (strcmp(method, "portal") == 0)
            {
                ptt__read_portal();
            }
        }
    }
}
