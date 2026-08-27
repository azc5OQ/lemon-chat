
// some globals for dev console
var g_client_list = null;
var g_channel_list = null

// Android javascript java bridge functions, used in WebView
var g_send_go_to_idle_mode_request = null;
var g_send_come_from_idle_mode_request = null;
var g_accept_current_settings_from_android = null;
var g_nudge_loopback_reattach = null;
var g_show_connection_phase = null;
var g_mark_call_accept_presence = null;

function JavascriptJavaBridge__send_go_to_idle_mode_request(is_forced)
{
    g_send_go_to_idle_mode_request(is_forced);
}

function JavascriptJavaBridge__send_come_from_idle_mode_request(channelId)
{
    g_send_come_from_idle_mode_request(channelId);
}

// java stamps this on a call accept, so the page holds presence through the accept transition
function JavascriptJavaBridge__mark_call_accept_presence()
{
    if (typeof g_mark_call_accept_presence === "function" && g_mark_call_accept_presence != null)
    {
        g_mark_call_accept_presence();
    }
}

function JavascriptJavaBridge__set_username_on_connect(username)
{
    g_set_username_on_connect(username);
}

// java calls this on app resume and when node reports "connected"
function JavascriptJavaBridge__nudge_loopback_reattach()
{
    if (typeof g_nudge_loopback_reattach === "function" && g_nudge_loopback_reattach != null)
    {
        g_nudge_loopback_reattach();
    }
}

// java relays node's connection phase, so the page can show it before the loopback attaches
function JavascriptJavaBridge__show_connection_phase(state, reason)
{
    if (typeof g_show_connection_phase === "function" && g_show_connection_phase != null)
    {
        g_show_connection_phase(state, reason);
    }
}

// java can push before window_onload wires the bridge; losing that push left the page
// with no settings at all, so hold it and let onload drain it
var g_pending_android_settings = null;

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

