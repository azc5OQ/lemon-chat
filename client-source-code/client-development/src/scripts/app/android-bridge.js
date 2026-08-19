
// some globals for dev console
var g_client_list = null;
var g_channel_list = null

// Android javascript java bridge functions, used in WebView
var g_send_go_to_idle_mode_request = null;
var g_send_come_from_idle_mode_request = null;
var g_accept_current_settings_from_android = null;

function JavascriptJavaBridge__send_go_to_idle_mode_request()
{
    g_send_go_to_idle_mode_request();
}

function JavascriptJavaBridge__send_come_from_idle_mode_request(channelId)
{
    g_send_come_from_idle_mode_request(channelId);
}

function JavascriptJavaBridge__set_username_on_connect(username)
{
    g_set_username_on_connect(username);
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

