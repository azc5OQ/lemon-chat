// exposes the page functions called by Java outside the shared client closure
// android-host.js supplies their handlers after initialization

// client lists exposed to the development console
var g_client_list = null;
var g_channel_list = null;

// handlers supplied by the Android host
var g_send_go_to_idle_mode_request = null;
var g_send_come_from_idle_mode_request = null;
var g_accept_current_settings_from_android = null;
var g_request_loopback_reattach = null;
var g_show_connection_phase = null;
var g_mark_call_accept_presence = null;

/**
 * @brief forwards Android's idle request to the shared client
 *
 * @param boolean is_forced -> true to idle from any channel, false for ordinary background entry
 *
 * @return void
 */
function JavascriptJavaBridge__send_go_to_idle_mode_request(is_forced)
{
    if (typeof g_send_go_to_idle_mode_request === "function" && g_send_go_to_idle_mode_request != null)
    {
        g_send_go_to_idle_mode_request(is_forced);
    }
}

/**
 * @brief forwards Android's request to return from idle
 *
 * @param number|null channelId -> channel to rejoin, null for root
 *
 * @return void
 */
function JavascriptJavaBridge__send_come_from_idle_mode_request(channelId)
{
    if (typeof g_send_come_from_idle_mode_request === "function" && g_send_come_from_idle_mode_request != null)
    {
        g_send_come_from_idle_mode_request(channelId);
    }
}

/**
 * @brief keeps presence active while a call accept is applied
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
 * @brief supplies the username requested for the next login
 *
 * @param string username -> requested username, empty strings are ignored
 *
 * @return void
 */
function JavascriptJavaBridge__set_username_on_connect(username)
{
    if (typeof g_set_username_on_connect === "function" && g_set_username_on_connect != null)
    {
        g_set_username_on_connect(username);
    }
}

/**
 * @brief requests a local connection when Android resumes the page
 *
 * @return void
 */
function JavascriptJavaBridge__request_loopback_reattach()
{
    if (typeof g_request_loopback_reattach === "function" && g_request_loopback_reattach != null)
    {
        g_request_loopback_reattach();
    }
}

/**
 * @brief displays Node's connection phase before the page attaches
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

// keep settings received before the shared client has installed its handler
var g_pending_android_settings = null;

/**
 * @brief applies native settings or keeps them until the handler is ready
 *
 * @param string settings_json_string -> the settings as json text
 *
 * @return void
 */
function JavascriptJavaBridge__accept_current_settings_from_android(settings_json_string)
{
    if (typeof g_accept_current_settings_from_android === "function" && g_accept_current_settings_from_android != null)
    {
        g_accept_current_settings_from_android(settings_json_string);
    }
    else
    {
        g_pending_android_settings = settings_json_string;
    }
}

