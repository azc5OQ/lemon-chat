// android-bridge.js is embedded in template.html in its own script block, outside the shared closure
// it holds the JavascriptJavaBridge__* page functions that the android java side calls by name
// each one forwards into android-webview.js, which does the actual work inside the closure

// some globals for dev console
var g_client_list = null;
var g_channel_list = null;

// Android javascript java bridge functions, used in WebView
var g_send_go_to_idle_mode_request = null;
var g_send_come_from_idle_mode_request = null;
var g_accept_current_settings_from_android = null;
var g_nudge_loopback_reattach = null;
var g_show_connection_phase = null;
var g_mark_call_accept_presence = null;

/**
 * @brief java's "go idle" entry point, forwarded into the closure
 *
 * @param boolean is_forced -> true for a swipe-away (idle from any channel), false for the home button
 *
 * @return void
 */
function JavascriptJavaBridge__send_go_to_idle_mode_request(is_forced)
{
    g_send_go_to_idle_mode_request(is_forced);
}

/**
 * @brief java's "leave idle" entry point, forwarded into the closure
 *
 * @param number|null channelId -> the channel to rejoin, null for the one left
 *
 * @return void
 */
function JavascriptJavaBridge__send_come_from_idle_mode_request(channelId)
{
    g_send_come_from_idle_mode_request(channelId);
}

/**
 * @brief java stamps this on a call accept, so the page holds presence through the accept transition
 *
 * @return void
 */
function JavascriptJavaBridge__mark_call_accept_presence()
{
    if (typeof g_mark_call_accept_presence === "function" && g_mark_call_accept_presence != null)
    {
        g_mark_call_accept_presence();
    }
}

/**
 * @brief java hands over the username the page asks for while connecting
 *
 * @param string username -> the name, "" for the assigned one
 *
 * @return void
 */
function JavascriptJavaBridge__set_username_on_connect(username)
{
    g_set_username_on_connect(username);
}

/**
 * @brief java calls this on app resume and when node reports "connected", so the page re-attaches to the loopback
 *
 * @return void
 */
function JavascriptJavaBridge__nudge_loopback_reattach()
{
    if (typeof g_nudge_loopback_reattach === "function" && g_nudge_loopback_reattach != null)
    {
        g_nudge_loopback_reattach();
    }
}

/**
 * @brief java relays node's connection phase, so the page can show it before the loopback attaches
 *
 * @param string state -> the phase name node reported
 * @param string reason -> the text to show with it
 *
 * @return void
 */
function JavascriptJavaBridge__show_connection_phase(state, reason)
{
    if (typeof g_show_connection_phase === "function" && g_show_connection_phase != null)
    {
        g_show_connection_phase(state, reason);
    }
}

// java can push before main__window_onload wires the bridge; losing that push left the page
// with no settings at all, so hold it and let onload drain it
var g_pending_android_settings = null;

/**
 * @brief java pushes the current settings; kept in g_pending_android_settings until the closure is ready to take them
 *
 * @param string settings_json_string -> the settings as json text
 *
 * @return void
 */
function JavascriptJavaBridge__accept_current_settings_from_android(settings_json_string)
{
    if (typeof g_accept_current_settings_from_android === "function"
        && g_accept_current_settings_from_android != null)
    {
        g_accept_current_settings_from_android(settings_json_string);
    }
    else
    {
        g_pending_android_settings = settings_json_string;
    }
}

