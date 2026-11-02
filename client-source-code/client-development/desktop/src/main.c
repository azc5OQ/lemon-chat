// Application startup, copies of received data for the window, and shutdown.
// Button and menu actions live in ui/ui_window.c; protocol handling lives in msg/.
#include "platform/system.h"
#include "platform/hotkey.h"
#include "platform/memory.h"
#include "session/session.h"
#include "settings/settings.h"
#include "overlay_controller.h"
#include "ui/ui_headers.h"
#include "ui/ui_theme.h"
#include "ui/ui_window.h"
#include "third_party/log.h"

#ifdef LEMON_DESKTOP_STARTUP_CHECK
#include "../checks/startup_check.h"
#endif

// Main owns these objects for the entire application lifetime. UI modules borrow
// their addresses. The window copies the state published by the connection thread
// into g_session_view; that thread never changes this copy or accesses controls.
static desktop_session_t* g_session;
static desktop_session_view_t g_session_view;
static desktop_settings_t g_settings;
static desktop_overlay_controller_t* g_overlay;

// A failed startup uses the same cleanup as a closed window. These flags record
// which cleanup functions can run, including a partially initialized UI.
static boole g_platform_initialized;
static boole g_ui_initialized;

// declarations
static boole _main_internal__initialize(boole smoke, boole preview);
static void _main_internal__load_settings(boole smoke);
static void _main_internal__update(boole smoke);
static void _main_internal__shutdown(void);

/**
 * @brief creates the native window, connection objects and controls in their required order.
 * @param boole smoke -> hide the window and leave real audio devices and saved settings alone
 * @param boole preview -> show example members instead of an empty disconnected channel tree
 * @return boole -> TRUE when startup completed; FALSE requires the same shutdown as normal exit
 */
static boole _main_internal__initialize(boole smoke, boole preview)
{
    gui_app_config_t configuration = {0};

    // Asset paths are relative to the application directory. On Linux, hotkey
    // initialization must also enable Xlib threading before Meek opens a display.
    if (!desktop_system__asset_directory() || !desktop_hotkey__initialize())
    {
        log_error("Could not initialize the application directory or keyboard support.");
        return FALSE;
    }
    configuration.title = L"Lemon Chat";
    configuration.width = 1180;
    configuration.height = 780;
    configuration.clear_color = scene__hex(0xffffff);
    configuration.hidden = smoke;
    if (!platform__init(&configuration))
    {
        log_error("Could not create the desktop window and renderer.");
        return FALSE;
    }
    g_platform_initialized = TRUE;

    // Creating a session prepares its queues and audio state; it does not connect.
    // The overlay controller checks for the chosen game only when enabled in settings.
    g_session = desktop_session__create();
    g_overlay = desktop_overlay_controller__create();
    if (!g_session || !g_overlay)
    {
        log_error("Could not allocate the connection or game-overlay state.");
        return FALSE;
    }

    // Controls keep pointers to these objects, so neither may be a temporary local.
    // Mark UI cleanup as required before registration: layout loading can fail later.
    g_ui_initialized = TRUE;
    if (!desktop_ui_window__initialize(g_session, &g_session_view, &g_settings, g_overlay, preview, !smoke))
    {
        log_error("Could not register input handlers or load the desktop controls.");
        return FALSE;
    }
    _main_internal__load_settings(smoke);
    desktop_ui_window__apply_settings();
    return TRUE;
}

/**
 * @brief loads the user's preferences, or supplies isolated defaults for the startup check.
 * @param boole smoke -> use defaults without reading the saved identity or connection details
 * @return void
 */
static void _main_internal__load_settings(boole smoke)
{
    if (!smoke)
    {
        desktop_settings__load(&g_settings);
        return;
    }

    // The check displays forms without opening a microphone, connecting to a
    // server, or using a saved setting that could enable injection into a game.
    g_settings.audio_volume = 100;
    g_settings.microphone_gain = 100;
    g_settings.push_to_talk_key = DESKTOP_HOTKEY_DEFAULT;
    g_settings.activation_threshold = -45;
    g_settings.voice_bitrate = 64;
}

/**
 * @brief copies received state into the window and supplies channel members to the game overlay.
 * @param boole smoke -> use a fixed preview time so captured indicators are repeatable
 * @return void
 */
static void _main_internal__update(boole smoke)
{
    boole changed = FALSE;
    desktop_overlay_snapshot_t snapshot = {0};
    char overlay_status[256] = {0};

    // A layout reload replaces controls, so reapply text limits and display
    // settings before rebuilding their contents from the current connection.
    if (hot_reload__tick())
    {
        desktop_ui_window__reload();
    }

    // The session and attachment manager copy their data while holding their own
    // locks. No lock stays held while the window creates rows or uploads images.
    changed = desktop_session__view(g_session, &g_session_view);
    changed |= desktop_attachments__view(desktop_session__attachments(g_session), &g_session_view.attachments);
    desktop_ui_window__update(changed);

    // Send only the channel and member display data to the overlay. On Windows,
    // the controller shares this copy with the code drawing inside the chosen game.
    // Preview uses example members and does not require a server connection.
    desktop_overlay_controller__status(g_overlay, overlay_status, sizeof(overlay_status));
    if (desktop_ui_window__preview())
    {
        desktop_overlay_data__preview(&snapshot, smoke ? 1100 : desktop_system__milliseconds());
    }
    else
    {
        desktop_session__overlay(&g_session_view, &snapshot);
    }
    desktop_overlay_controller__publish(g_overlay, &snapshot);
    desktop_ui_window__overlay(&snapshot, overlay_status);
    desktop_memory__erase(&snapshot, sizeof(snapshot));
    desktop_memory__erase(overlay_status, sizeof(overlay_status));
}

/**
 * @brief stops game detection and connections, frees window resources, and clears retained data.
 * Also handles failed startup; NULL objects and initialization flags identify what exists.
 * @return void
 */
static void _main_internal__shutdown(void)
{
    // Stop checking game processes and stop supplying their overlay before releasing
    // the window's data. Destroy waits for the controller's thread to finish.
    desktop_overlay_controller__destroy(g_overlay);
    g_overlay = NULL;
    if (g_ui_initialized)
    {
        // Textures require a live renderer to be freed. Connection fields are
        // erased here while their editable text buffers still exist.
        desktop_ui_window__shutdown();
        g_ui_initialized = FALSE;
    }

    // Disconnect, join the connection thread and stop audio/file activity before
    // clearing the copied messages, identities and preferences below.
    desktop_session__destroy(g_session);
    g_session = NULL;
    if (g_platform_initialized)
    {
        hot_reload__shutdown();
        desktop_ui_theme__shutdown();
        platform__shutdown();
        g_platform_initialized = FALSE;
    }
    desktop_memory__erase(&g_session_view, sizeof(g_session_view));
    desktop_memory__erase(&g_settings, sizeof(g_settings));
}

/**
 * @brief opens the chat window, processes input until it closes, then releases application resources.
 * @param int argc -> command-line argument count
 * @param char** argv -> arguments; --demo shows example members, --smoke runs the optional startup check
 * @return int -> zero on normal exit or a passed check; nonzero on startup or check failure
 */
int main(int argc, char** argv)
{
    boole smoke = FALSE;
    boole preview = FALSE;
    int64 index = 0;
    int result = 1;

    // Keep warnings and errors, without logging every mouse click or loaded image.
    log_set_level(LOG_WARN);
    for (index = 1; index < argc; index++)
    {
        if (clib__strcmp(argv[index], "--smoke") == 0)
        {
            smoke = TRUE;
        }
        if (clib__strcmp(argv[index], "--demo") == 0)
        {
            preview = TRUE;
        }
    }
#ifndef LEMON_DESKTOP_STARTUP_CHECK
    if (smoke)
    {
        log_error("Startup checks are not included in this build.");
        return 1;
    }
#endif

    if (_main_internal__initialize(smoke, preview))
    {
        result = 0;
        while (platform__tick())
        {
            _main_internal__update(smoke);
#ifdef LEMON_DESKTOP_STARTUP_CHECK
            if (smoke && desktop_startup_check__tick(&g_session_view, &g_settings))
            {
                result = desktop_startup_check__result();
                break;
            }
#endif
        }
    }
    _main_internal__shutdown();
    return result;
}
