// TRUE when this instance renders but does NOT own the server connection: the android
// WebView talking to node over the loopback. It must not answer the protocol - no key
// generation, no maintainer votes, no transport encryption - because node already does,
// and two answers on one connection get the client kicked.
//
// The counterpart is node itself, which owns the connection and answers everything.
// A desktop or website client owns its own connection, so this is always false there.
// the page sees the Android bridge object; the real webworkers do NOT, so they key on
// the loopback port the main thread hands them (mainthread__set_loopback_mode). without
// the second clause the data worker metadata-encrypted outgoing messages and the server
// kicked the connection the moment one was sent
function is_ui_only_runtime()
{
    return (typeof Android !== "undefined") || g_loopback_port > 0;
}

// ===========================================================================
// NODE RUNTIME - only ever executed by the embedded nodejs client (android).
// included in BOTH bundles because it must live inside moduleFactory's scope,
// but nothing here runs in a browser: init_node_runtime is called only by
// nodejs-client/android-main.js. See client-source-code/nodejs-client/README.md.
// ===========================================================================

// a stand-in for a real webworker. the five worker entry points are plain functions in this
// same scope, so node can run them in-process instead of in a thread. async on purpose, so a
// handler that posts a reply cannot re-enter its own caller mid-statement, which is what a
// real worker's message queue guarantees
// for the workers that genuinely do not exist headless. everything posted to it is dropped
function make_discarding_worker()
{
    return {
        postMessage: function() {},
        terminate: function() {}
    };
}

// set through the seam; every listener is called after every dispatched message
var g_node_message_listeners = [];

// set through the seam; receives every decrypted server frame raw, for the ui replay
var g_node_frame_listener = null;

// set through the seam; receives the total unread count for the launcher icon badge
var g_node_unread_listener = null;

// node only: true while a ui is attached to the loopback. node runs the same client
// code as the page, so without this it would assume somebody is reading its "current"
// channel and never count those messages as unread
var g_node_has_attached_ui = false;

// the bridge host's hook for incoming calls; null everywhere but android
var g_node_incoming_call_listener = null;

// just a copy so the checkbox can show the right state. the real one is in java
var g_is_file_logging_enabled = true;

// true once java has answered "is anyone looking". it watches the activity, the
// loopback socket only knows "is anyone connected", so java outranks it
var g_node_ui_visibility_from_host = false;

// a browser or the webview always has somebody in front of it; node only does while a
// ui is attached. `typeof process` is how this codebase tells node from a page
function is_someone_watching_the_ui()
{
    if (typeof process === "undefined")
    {
        return true;
    }

    return g_node_has_attached_ui;
}

// node with no ui is an idle client: present on the server so it can be called and
// messaged, but not standing in a channel as if somebody were there. a ui attaching
// is the user arriving, so it comes back out of idle
function node_apply_idle_for_ui_state()
{
    if (typeof process === "undefined")
    {
        return; // pages manage their own idle through the activity lifecycle
    }

    // these two own g_is_deep_idle and are both idempotent, so this can be called
    // on every attach/detach without tracking state here
    try
    {
        if (g_node_has_attached_ui == true)
        {
            // root, unless a channel was opened. the server drops a null one
            let channel_to_rejoin = (typeof current_channel_id === "number") ? current_channel_id : 0;

            console.log("connect-path: ui attached, leaving idle into channel " + channel_to_rejoin);
            exit_deep_idle();
            client_msg.send_come_from_idle_mode_request(channel_to_rejoin);
        }
        else
        {
            console.log("connect-path: no ui attached, going idle");
            enter_deep_idle();
        }
    }
    catch (idle_switch_failed)
    {
        console.error("idle switch failed: " + idle_switch_failed);
    }
}

// the server's auth frame, kept from the moment it arrives - the ui replay leads with it
var g_node_cached_auth_frame = null;

// loopback mode: connect to the node runtime on-device, plaintext, token-gated.
// zero on the desktop and website, which never receive these settings fields
var g_loopback_port = 0;
var g_loopback_token = "";

// local-only preference: avatar circles next to chat messages
var g_show_message_avatars = false;
try { g_show_message_avatars = (typeof localStorage !== "undefined" && localStorage.getItem("lemon_show_message_avatars") === "1"); } catch (e) { }

// read receipts, two halves that are set separately: whether we draw the eye others
// send us, and whether we send one back. both on unless turned off
var g_show_seen_indicator = true;
try { g_show_seen_indicator = (typeof localStorage === "undefined" || localStorage.getItem("lemon_seen_indicator") !== "0"); } catch (e) { }

var g_send_seen_receipts = true;
try { g_send_seen_receipts = (typeof localStorage === "undefined" || localStorage.getItem("lemon_send_seen") !== "0"); } catch (e) { }

// the mic button can be hidden outright by the user. off by default
var g_hide_microphone_button = false;
try { g_hide_microphone_button = (typeof localStorage !== "undefined" && localStorage.getItem("lemon_hide_mic") === "1"); } catch (e) { }

// local-only preference: mic button toggles transmission instead of push-to-talk
var g_is_continuous_mic_mode = false;
try { g_is_continuous_mic_mode = (typeof localStorage !== "undefined" && localStorage.getItem("lemon_continuous_mic") === "1"); } catch (e) { }

// which key pushes to talk. 81 is Q, the long-standing default
var g_push_to_talk_key_code = 81;
var g_push_to_talk_key_label = "Q";
try
{
    if (typeof localStorage !== "undefined")
    {
        let stored_key_code = parseInt(localStorage.getItem("lemon_ptt_key_code"));
        let stored_key_label = localStorage.getItem("lemon_ptt_key_label");

        if (stored_key_code > 0) { g_push_to_talk_key_code = stored_key_code; }
        if (stored_key_label) { g_push_to_talk_key_label = stored_key_label; }
    }
}
catch (e) { }

// true while continuous-mode transmission is running (tap started, no tap stopped yet)
var g_is_continuous_transmission_active = false;

// sender header for one message: optional avatar + name tagged with the sender id, so a
// rename can find and retag old messages. display_name_html must already be sanitized
function generate_message_sender_html(sender_client_id, display_name_html)
{
    let avatar_html = "";

    if (g_show_message_avatars == true && sender_client_id != null)
    {
        let sender = get_client_by_client_id(sender_client_id);

        if (sender != null && typeof sender.base64_avatar === "string" && sender.base64_avatar.length > 0)
        {
            avatar_html = "<img class=\"chat-message-avatar\" src=\"" + sender.base64_avatar + "\">";
        }
    }

    let id_attribute = (sender_client_id != null) ? (" data-sender-id=\"" + sender_client_id + "\"") : "";

    return avatar_html + "<p" + id_attribute + ">" + display_name_html + "</p>";
}

// renames every already-rendered message header of one sender (rename, alias change)
function retag_rendered_messages_of_sender(sender_client_id, display_name)
{
    let name_nodes = document.querySelectorAll('p[data-sender-id="' + sender_client_id + '"]');

    for (let i = 0; i < name_nodes.length; i++)
    {
        name_nodes[i].textContent = display_name;
    }
}

// false parks the connection: the socket is closed and the reconnect ticker idles.
// always true in the browser - only the node host flips it, for the webview handover
var g_node_connection_wanted = true;

// is_wanted false: close the socket; the driver then idles instead of redialing.
// is_wanted true: let the driver dial again right away
function node_set_connection_wanted(is_wanted)
{
    g_node_connection_wanted = (is_wanted == true);

    if (g_node_connection_wanted == false && g_websocket_worker != null)
    {
        g_websocket_worker.postMessage({ type: "close" });
    }

    if (g_node_connection_wanted == true && g_is_authenticated == false)
    {
        nudge_connection_driver();
    }
}

// contains a throw from any handler. the loopback workers run in-process and node cannot
// be restarted once dead, so one malformed message must not kill the runtime
function dispatch_safely(handler, message, source_name)
{
    let caught_error = null;

    try
    {
        handler({ data: message });
    }
    catch (dispatch_error)
    {
        caught_error = dispatch_error;
        console.error("handler failed for '" + ((message != null && message.type) ? message.type : "?")
            + "' via " + source_name + ": "
            + (dispatch_error != null && dispatch_error.stack ? dispatch_error.stack : dispatch_error));
    }

    // fires per internal hop, so several times per server message. it only means "state
    // may have changed" - debounce on the host side
    for (let i = 0; i < g_node_message_listeners.length; i++)
    {
        try
        {
            g_node_message_listeners[i](((message != null && message.type) ? message.type : ""), caught_error != null);
        }
        catch (callback_error)
        {
            console.error("message listener failed: " + callback_error);
        }
    }
}

function make_loopback_worker(handler, worker_name)
{
    return {
        postMessage: function(message)
        {
            setTimeout(function() { dispatch_safely(handler, message, worker_name); }, 0);
        },
        terminate: function() {}
    };
}

// wires the runtime the way window_onload does for the browser, minus everything needing a
// dom. it lives INSIDE the factory because all of these are moduleFactory locals - a node
// bootstrap outside cannot assign them, which is why `globalThis.g_websocket_worker = x`
// silently does nothing.
function init_node_runtime(identity_passphrase_string)
{
    // every outgoing message goes through custom_log, which falls back to global.postMessage
    // when this is null. a dead element's .value is "", so the append and the 50kb
    // truncation both work harmlessly and main.js needs no change
    g_textarea_log = document.getElementById("textarea-log");

    // closes the loop: the worker entry points reply with global.postMessage, and in a
    // browser that lands on the main thread. here `global` is the shim window, so route it
    // to the same dispatcher the browser would have used - through the same containment
    // the loopback workers get, so a throw in a server_msg handler cannot kill node
    global.postMessage = function(message)
    {
        // raw frames go to the listener only, not into the dispatcher
        if (message != null && message.type === "decrypted_frame")
        {
            // cheap check first, real parse only on a hit, so a chat message
            // containing the literal cannot overwrite the cached frame
            if (message.value.indexOf("\"authentication_status\"") !== -1)
            {
                try
                {
                    if (JSON.parse(message.value).message.type === "authentication_status")
                    {
                        g_node_cached_auth_frame = message.value;
                    }
                }
                catch (e) { }
            }

            if (g_node_frame_listener != null)
            {
                try { g_node_frame_listener(message.value); }
                catch (listener_error) { console.error("frame listener failed: " + listener_error); }
            }
            return;
        }

        setTimeout(function() { dispatch_safely(mainthread_onmessage, message, "mainthread"); }, 0);
    };

    // websocket_worker_onmessage does `new WebSocket(...)`, which node has natively, so the
    // transport needs no replacement - only somewhere to run
    g_websocket_worker = make_loopback_worker(websocket_worker_onmessage, "websocket_worker");
    g_data_processing_worker = make_loopback_worker(data_processing_worker_onmessage, "data_processing_worker");

    // every decrypted frame also reaches the frame listener, for the ui replay
    g_data_processing_worker.postMessage({ type: "mainthread__set_frame_forwarding", value: true });

    // last line of defence: the heartbeat/loss loop is an async while and one escape can
    // kill it silently. a setInterval survives its own throws, so this always runs
    setInterval(function()
    {
        try
        {
            let heartbeat_age = new Date().valueOf() - g_connection_check_message_response_received_timestamp;

            if (g_is_authenticated == true
                && g_connection_check_message_response_received_timestamp > 0
                && heartbeat_age > (g_connection_check_lost_threshold_ms + 15000))
            {
                console.error("watchdog: heartbeat response " + heartbeat_age + "ms old, forcing reset + reconnect");
                g_is_authenticated = false;
                reset_chat_app_keep_identity();
            }
        }
        catch (watchdog_error)
        {
            console.error("watchdog failed: " + watchdog_error);
        }
    }, 15000);

    // one greppable line per minute, so a dead night can be reconstructed from logcat
    setInterval(function()
    {
        try
        {
            let heartbeat_age = (g_connection_check_message_response_received_timestamp > 0)
                ? (new Date().valueOf() - g_connection_check_message_response_received_timestamp) : -1;
            console.log("conn: auth=" + g_is_authenticated + " ws=" + g_is_websocket_connected
                + " hb_age_ms=" + heartbeat_age + " checker=" + g_should_connection_check_be_running);
        }
        catch (log_error) { }
    }, 60000);

    // opus and minimp3 have no implementation here - their entry points are not even in the
    // bundle - but plenty of shared code posts to them unconditionally, channel key
    // distribution being the one that matters for text chat. a DISCARDING stand-in keeps
    // those call sites working without sprinkling null checks through code the browser
    // also runs. it is not pretending audio works: messages to it go nowhere, on purpose
    g_opus_decoder_worker = make_discarding_worker();
    g_opus_encoder_worker = make_discarding_worker();
    g_minimp3_worker = make_discarding_worker();

    // a persisted identity can start generating right away; otherwise the settings
    // push supplies the seed. the driver waits on the identity slot either way
    if (typeof identity_passphrase_string === "string" && identity_passphrase_string.length >= 199)
    {
        request_identity(identity_passphrase_string);
    }

    connection_driver();
}
