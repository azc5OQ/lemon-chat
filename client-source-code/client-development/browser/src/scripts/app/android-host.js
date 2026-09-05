// android-host.js is embedded in template.html along with the other client files, and in the node bundle
// it is the client's two android hosts: the headless node runtime of the background service (the workers
// stood in for in-process, the api the host calls), the webview bridge (the implementations behind the
// JavascriptJavaBridge__* functions of android-bridge.js), and the deep idle mode both of them use
// a desktop or website client wires none of it; dispatch.js calls the idle side

// state private to this file
// pending AudioContext.suspend() from idle entry; exit chains its resume on it so a
// late-settling suspend can never land after the resume and silence the graph
var audio_suspend_promise = null;

// the "leaving idle" request in flight: it keeps a late idle request from putting the client back to sleep
var is_come_from_idle_in_flight = false;

var come_from_idle_in_flight_timer = null;

// call-accept presence grace: gentle idle waits this long after an accept before re-deciding,
// so the accept transition's blink of invisibility cannot idle the user out of the call
var CALL_ACCEPT_PRESENCE_GRACE_MS = 5000;

var presence_grace_until_timestamp = 0;

var presence_grace_recheck_timer = null;

// ---- headless node runtime ----

/**
 * @brief whether this runtime only renders: the android webview draws the ui while node owns the connection over the loopback
 *        a ui-only runtime must not answer the protocol (keys, votes, transport encryption) or the
 *        client gets kicked; the workers have no Android object, so they key on the loopback port
 *        the main thread hands them
 *
 * @return boolean true for the webview and for a worker of it
 */
function android_host__is_ui_only_runtime()
{
    return (typeof Android !== "undefined") || g_loopback_port > 0;
}

/**
 * @brief a stand-in for the workers that genuinely do not exist headless; everything posted to it is dropped
 *
 * @return object an object with postMessage and terminate that do nothing
 */
function android_host__make_discarding_worker()
{
    return {
        postMessage: function() {},
        terminate: function() {}
    };
}

/**
 * @brief whether a person is in front of this client
 *        a browser or the webview always has somebody in front of it; headless node only does
 *        while a ui is attached. typeof process is how this codebase tells node from a page
 *
 * @return boolean true when somebody is looking
 */
function android_host__is_someone_watching_the_ui()
{
    if (typeof process === "undefined")
    {
        return true;
    }

    return g_node_has_attached_ui;
}

/**
 * @brief node with no ui is an idle client: present on the server so it can be called and messaged, but not standing in a channel as if somebody were there
 *        a ui attaching means the user arrived, so the client comes back out of idle; a page
 *        manages its own idle through the activity lifecycle and leaves at once
 *
 * @return void
 */
function android_host__node_apply_idle_for_ui_state()
{
    if (typeof process === "undefined")
    {
        return; // a page manages its own idle through the activity lifecycle
    }

    // android_host__enter_deep_idle and android_host__exit_deep_idle own g_is_deep_idle and are both idempotent, so this
    // can run on every attach and detach without tracking state here
    try
    {
        if (g_node_has_attached_ui == true)
        {
            // rejoin the channel that was open, or root, because the server drops a null channel id
            let channel_to_rejoin = (typeof g_current_channel_id === "number") ? g_current_channel_id : 0;

            console.log("connect-path: ui attached, leaving idle into channel " + channel_to_rejoin);
            android_host__exit_deep_idle();
            client_msg.send_come_from_idle_mode_request(channel_to_rejoin);
        }
        else
        {
            console.log("connect-path: no ui attached, going idle");
            android_host__enter_deep_idle();
        }
    }
    catch (idle_switch_failed)
    {
        console.error("idle switch failed: " + idle_switch_failed);
    }
}

g_show_message_avatars = (utils__storage_get("lemon_show_message_avatars") === "1");

let stored_rsa_key_bits = parseInt(utils__storage_get("lemon_rsa_key_bits", ""));
if (G_ALLOWED_RSA_KEY_BITS.indexOf(stored_rsa_key_bits) >= 0) { g_rsa_key_bits = stored_rsa_key_bits; }

g_show_seen_indicator = (utils__storage_get("lemon_seen_indicator") !== "0");

g_send_seen_receipts = (utils__storage_get("lemon_send_seen") !== "0");

g_auto_scroll_chat_to_end = (utils__storage_get("lemon_auto_scroll") !== "0");

g_hide_microphone_button = (utils__storage_get("lemon_hide_mic") === "1");

g_selected_microphone_device_id = utils__storage_get("lemon_mic_device_id", "");

g_is_continuous_mic_mode = (utils__storage_get("lemon_continuous_mic") === "1");

let stored_key_code = parseInt(utils__storage_get("lemon_ptt_key_code"));
let stored_key_label = utils__storage_get("lemon_ptt_key_label");
if (stored_key_code > 0) { g_push_to_talk_key_code = stored_key_code; }
if (stored_key_label) { g_push_to_talk_key_label = stored_key_label; }

/**
 * @brief the sender header of one message: an optional avatar and the name, tagged with the sender id so a rename can find and retag old messages
 *
 * @param number|null sender_client_id -> the sender, null for an untagged header
 * @param string display_name_html -> the name, already sanitized
 *
 * @return string the header html
 */
function android_host__generate_message_sender_html(sender_client_id, display_name_html)
{
    let avatar_html = "";

    if (g_show_message_avatars == true && sender_client_id != null)
    {
        let sender = channel_tree__get_client_by_client_id(sender_client_id);

        if (sender != null && typeof sender.base64_avatar === "string" && sender.base64_avatar.length > 0)
        {
            avatar_html = "<img class=\"chat-message-avatar\" src=\"" + sender.base64_avatar + "\">";
        }
    }

    let id_attribute = (sender_client_id != null) ? (" data-sender-id=\"" + sender_client_id + "\"") : "";

    return avatar_html + "<p" + id_attribute + ">" + display_name_html + "</p>";
}

/**
 * @brief renames every already-rendered message header of one sender, after a rename or an alias change
 *
 * @param number sender_client_id -> the sender
 * @param string display_name -> the new name
 *
 * @return void
 */
function android_host__retag_rendered_messages_of_sender(sender_client_id, display_name)
{
    let name_nodes = document.querySelectorAll('p[data-sender-id="' + sender_client_id + '"]');

    for (let i = 0; i < name_nodes.length; i++)
    {
        name_nodes[i].textContent = display_name;
    }
}

/**
 * @brief the host's park flag: with false the socket closes and the driver idles instead of redialing, with true the driver may dial again right away
 *        re-dialing after a re-arm rides the settings push; this only owns the flag and the park
 *
 * @param boolean is_wanted -> whether node should hold a connection
 *
 * @return void
 */
function android_host__node_set_connection_wanted(is_wanted)
{
    g_node_connection_wanted = (is_wanted == true);

    if (g_node_connection_wanted == false && g_websocket_worker != null)
    {
        g_websocket_worker.postMessage({ type: "close" });
    }

    // re-dialing after a re-arm rides the settings push, connection__request_connect("settings")
    // this function only owns the wanted flag and the park
}

/**
 * @brief runs a handler on one message and contains any throw, because the loopback workers run in-process and node can not be restarted once dead; one malformed message must not kill the runtime
 *        the message listeners are told afterwards, with whether the handler failed
 *
 * @param function handler -> the worker entry point to call
 * @param object message -> the message, handed over as { data: message }
 * @param string source_name -> which worker it stands for, for the log
 *
 * @return void
 */
function android_host__dispatch_safely(handler, message, source_name)
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

    // this fires per internal hop, so several times per server message. it only means state
    // may have changed; the host side debounces
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

/**
 * @brief a stand-in for a real webworker: the entry points are plain functions in this scope, so node runs them in-process
 *        the reply is async on purpose, like a real worker's message queue
 *
 * @param function handler -> the worker entry point
 * @param string worker_name -> the name, for the log
 *
 * @return object an object with postMessage and terminate
 */
function android_host__make_loopback_worker(handler, worker_name)
{
    return {
        postMessage: function(message)
        {
            setTimeout(function() { android_host__dispatch_safely(handler, message, worker_name); }, 0);
        },
        terminate: function() {}
    };
}

/**
 * @brief wires the headless runtime the way main__window_onload does for the browser, minus everything that needs a dom
 *        it lives inside the factory because a bootstrap outside cannot assign the factory's locals
 *
 * @param string|null identity_passphrase_string -> the passphrase the identity is derived from, null for a random one
 *
 * @return void
 */
function android_host__init_node_runtime(identity_passphrase_string)
{
    // every outgoing message goes through utils__custom_log, which falls back to global.postMessage
    // when this is null. a dead element's .value is "", so the append and the 50kb truncation
    // both work harmlessly and main.js needs no change
    g_textarea_log = document.getElementById("textarea-log");

    // the entry points reply with global.postMessage; here global is the shim window, so the reply
    // is routed to the same dispatcher a browser uses, contained so a throwing handler cannot kill node
    global.postMessage = function(message)
    {
        // raw frames go to the frame listener only, not into the dispatcher
        if (message != null && message.type === "decrypted_frame")
        {
            // the cheap indexOf check runs first and the real parse only on a hit, so a chat
            // message containing the literal can not overwrite the cached frame
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

        setTimeout(function() { android_host__dispatch_safely(dispatch__mainthread_onmessage, message, "mainthread"); }, 0);
    };

    // workers__websocket_worker_onmessage does `new WebSocket(...)`, which node has natively, so the
    // transport needs no replacement, only somewhere to run
    g_websocket_worker = android_host__make_loopback_worker(workers__websocket_worker_onmessage, "websocket_worker");
    g_data_processing_worker = android_host__make_loopback_worker(workers__data_processing_worker_onmessage, "data_processing_worker");

    // every decrypted frame also reaches the frame listener, for the ui replay
    g_data_processing_worker.postMessage({ type: "mainthread__set_frame_forwarding", value: true });

    // the last line of defence. the heartbeat loss loop is an async while and one escaped throw
    // can kill it silently; a setInterval survives its own throws, so this watchdog always runs
    setInterval(function()
    {
        try
        {
            let heartbeat_age = new Date().valueOf() - g_connection_check.last_response_timestamp;

            if (g_is_authenticated == true
                && g_connection_check.last_response_timestamp > 0
                && heartbeat_age > (g_connection_check.lost_threshold_ms + 15000))
            {
                console.error("watchdog: heartbeat response " + heartbeat_age + "ms old, forcing reset + reconnect");
                g_is_authenticated = false;
                connection__reset_chat_app_keep_identity();
            }
        }
        catch (watchdog_error)
        {
            console.error("watchdog failed: " + watchdog_error);
        }
    }, 15000);

    // logs one greppable line per minute, so a dead night can be reconstructed from logcat
    setInterval(function()
    {
        try
        {
            let heartbeat_age = (g_connection_check.last_response_timestamp > 0)
                ? (new Date().valueOf() - g_connection_check.last_response_timestamp) : -1;
            console.log("conn: auth=" + g_is_authenticated + " ws=" + g_is_websocket_connected
                + " hb_age_ms=" + heartbeat_age + " checker=" + g_should_connection_check_be_running);
        }
        catch (log_error) { }
    }, 60000);

    // opus and minimp3 are not in this bundle, but shared code posts to them unconditionally; a
    // discarding stand-in keeps those call sites working, messages to it go nowhere on purpose
    g_opus_decoder_worker = android_host__make_discarding_worker();
    g_opus_encoder_worker = android_host__make_discarding_worker();
    g_minimp3_worker = android_host__make_discarding_worker();

    // a persisted identity can start generating right away; otherwise the settings push
    // supplies the seed. the driver waits on the identity slot either way
    if (typeof identity_passphrase_string === "string" && identity_passphrase_string.length >= 199)
    {
        connection__request_identity(identity_passphrase_string);
    }

    connection__connection_driver();
}

// ---- webview bridge ----

var android_js_bridge = {
    /**
     * @brief java's "go idle": enters deep idle, which sends the request to the server itself and shuts down audio, the opus tick and the datachannel
     *        a gentle call (home button) keeps an in-channel session alive; a forced one (swipe-away) idles from any channel
     *
     * @param boolean is_forced -> true for a swipe-away
     *
     * @return void
     */
    send_go_to_idle_mode_request_android: function(is_forced)
    {
        // android_host__enter_deep_idle sends the go-to-idle request to the server itself and additionally
        // shuts down audio, the opus tick and the webrtc datachannel. a gentle call (home
        // button) keeps an in-channel session alive; a forced one (swipe-away) idles from any channel
        android_host__enter_deep_idle(is_forced === true);
    },

    /**
     * @brief java's "leave idle": asks the server to bring the client back into a channel
     *        sometimes runs twice (a call accept, then onResume) and the server refuses the second;
     *        a channel id means a call accept, so presence is held through the accept transition
     *
     * @param number|null channelId -> the channel to rejoin, null for root
     *
     * @return void
     */
    send_come_from_idle_mode_request_android: function(channelId = null)
    {
        // this function sometimes gets ran twice,
        // when client accepts the call and a bit later when app runs onResume (because he accepted it)
        // but server is smart and doesnt let user come back from idle mode twice

        // a channel id means a call accept, so presence is held, because the accept
        // transition's blink of invisibility could otherwise idle the user right back
        // out of the call
        if (channelId != null)
        {
            android_host__mark_call_accept_presence_grace();
        }

        if (channelId == null)
        {
            channelId = 0; // unspecified, so root channel.
        }

        // restore audio, the opus tick, webrtc and the fast heartbeat before rejoining
        android_host__exit_deep_idle();

        client_msg.send_come_from_idle_mode_request(channelId);
    },

    /**
     * @brief java calls this on ACTION_ACCEPT_CALL, so the webview's own idle logic knows a call was just accepted; node learns it through its come-from-idle instead
     *
     * @return void
     */
    mark_call_accept_presence_android: function()
    {
        android_host__mark_call_accept_presence_grace();
    },

    /**
     * @brief java hands over the username: it is sent as a rename now and remembered as the chosen name, so the next connect applies it at login (a rename the admin may have switched off for users)
     *
     * @param string username -> the name, "" is ignored
     *
     * @return void
     */
    set_username_on_connect_android: function(username)
    {
        if (username.length == 0)
        {
            return;
        }

        // remembered as the chosen username too, because on the next connect the
        // server can then apply the name at login instead of through a rename -
        // a rename the admin may have switched off for users
        g_chosen_username = username;

        client_msg.send_change_client_username_request(username, g_local_client_id);

        let index = channel_tree__get_client_index_in_array_by_client_id(g_local_client_id);

        if (index != -1)
        {
            g_client_list[index].username = username;
        }

        g_local_username = username;
        { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.setAttribute('value', g_local_username); } }
    },

    /**
     * @brief node's connection phase relayed by java
     *        it feeds the page's own status machinery, which the ticker keeps repainting, and keeps
     *        the spinner up while node works; nothing once authenticated
     *
     * @param string state -> the phase name
     * @param string reason -> the text to show with it
     *
     * @return void
     */
    show_connection_phase_android: function(state, reason)
    {
        if (g_is_authenticated == true)
        {
            return;
        }

        g_connection_status.state = state;
        g_connection_status.reason = (reason != null) ? reason : "";
        connection__render_connection_status();

        if (state === "connecting" || state === "connected")
        {
            connection__extend_connect_page_holdback();
        }
        else
        {
            connection__reveal_connect_page();
        }
    },

    /**
     * @brief java says node is connected or the app came to front
     *        the dial decision belongs to connection__request_connect; this only self-heals a lost
     *        settings push first (no loopback details yet means one got lost, so it asks again)
     *
     * @return void
     */
    nudge_loopback_reattach_android: function()
    {
        if (g_is_authenticated == true)
        {
            return;
        }

        // no loopback details yet means a settings push got lost, so ask again
        if (g_loopback_port <= 0)
        {
            if (typeof Android !== "undefined")
            {
                Android.JavaExportRequestCurrentSettingsFromAndroid();
            }
            return;
        }

        connection__request_connect("resume");
    },

    /**
     * @brief takes the settings json java pushes: app mode and theme first, then the loopback details, the identity, the microphone mode and the autoconnect, and finally hands the driver its target
     *        the theme is applied before anything below can throw: the settings json grows
     *        incrementally on the java side, and a missing field must not skip the theme
     *
     * @param string json_current_settings -> the settings as json
     *
     * @return void
     */
    accept_current_settings_from_android: function(json_current_settings)
    {

        let settings_from_android = JSON.parse(json_current_settings);

        // the theme is applied first, before anything below can throw: the settings json
        // grows incrementally on the java side, and a missing field must not skip the theme
        g_android_app_mode = (typeof settings_from_android.app_mode === "string") ? settings_from_android.app_mode : "";
        console.log("android settings received, app_mode = " + g_android_app_mode);

        main__apply_theme_for_app_mode();

        g_are_server_details_predefined = true;
        is_autoconnect_enabled = true;

        // there is difference between how web browser app and how android app handles auto connects

        // in a browser: predefined details show only the connect button, no details show the
        // form as well, and autoconnect without user action shows neither and keeps dialing

        // in android
        // autoconnect is on -> connect button doesnt show, app goes in loop and tries to join the server until it succeeds
        // autoconnect is off -> connect button shows (but server details need specified in android settings)

        // these differences exists because web browser has limitation, sometimes it requries user interaction to play soudns and android app doesnt
        // different enviroments sometimes need different way of doing things

        // yes, this is confusing, and needs improvements, should be documentated better

        // a first-run json may lack these fields; an absent field means on, because a
        // fresh install must connect (undefined == false is false in js, so the checks need it)
        g_is_autoconnect_without_user_action_active = (settings_from_android.is_autoconnect_enabled != false);

        // android decides if the log file is written. the page only copies the answer
        g_is_file_logging_enabled = (settings_from_android.is_file_logging_enabled != false);

        // a choice made in the local settings panel outranks the android settings json
        let has_local_sound_choice = false;
        has_local_sound_choice = (utils__storage_get("lemon_sound_effects") != null);

        if (has_local_sound_choice == false)
        {
            g_are_sound_effects_enabled = (settings_from_android.is_audio_effect_enabled == true);
        }

        // the mic has to start or stop with the flag; the connect path applies it on the first
        // push, so only a switch moved while running is acted on here. the flag is set directly,
        // not through the click handler, so a runtime with no ui sets it and touches no audio
        let is_microphone_always_on_wanted = (settings_from_android.is_microphone_always_on == true);

        if (g_have_received_android_settings == false)
        {
            voice__set_microphone_always_on(is_microphone_always_on_wanted);
        }
        else if (is_microphone_always_on_wanted != g_is_microphone_always_on)
        {
            voice__set_microphone_always_on(is_microphone_always_on_wanted);
            UI.activate_continous_audio_broadcast_apply();
        }

        g_have_received_android_settings = true;

        // the app log is opt-in from the java settings. the open textarea is hidden too
        // when the switch turns off, because it would otherwise linger with no button to close it
        if (settings_from_android.is_app_log_enabled == true)
        {
            document.getElementById("show-hide-log-button").style.display = "";
        }
        else
        {
            document.getElementById("show-hide-log-button").style.display = "none";
            document.getElementById("textarea-log").style.display = "none";
        }

        // on android the details come from the java settings window, so the in-page form
        // is hidden (display none, no dead gap); autoconnect off means the user needs a connect button
        document.getElementById("connect-form-sub-container-1").style.display = "none";
        document.getElementById("connect-form-sub-container-2").style.display = "none";
        document.getElementById("add-key-button").style.display = "none";

        // bookmarks fill those hidden fields, so they have nothing to act on here
        document.getElementById("server-bookmarks-container").style.display = "none";

        if (g_is_autoconnect_without_user_action_active == false)
        {
            document.getElementById("another-buttons-sub-container").style.display = "";
            document.getElementById("another-buttons-sub-loading-container").style.display = "none";
            // explicit visible is needed because a stylesheet default hides it; the spinner
            // page has an opaque background now, so the button can not float through anymore
            document.getElementById("connect-button").style.visibility = "visible";
            document.getElementById("import-identity-button").style.display = "none";

            // nothing dials on its own here: no spinner, the connect page is the page
            connection__reveal_connect_page();
        }

        if (g_are_sound_effects_enabled == true)
        {
            document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGc+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxMSwzMjAgMzcwLDMxOCAzNDAsMzMyQzI5NywzNTQgMzAxLDM2MCAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMCwyNTYgMTQ5LDE2MkMxNTMsMTU5IDE5MSwxMzYgMTk5LDEzNEMyMDMsMTMyIDI0NiwxMTggMjQ5LDExOEMyNjgsMTE3IDI2NywxMTMgMjg5LDExM0MyOTEsMTEzIDI5MCwxMTEgMjkyLDExMUMzMTEsMTA5IDM3MSwxMDkgMzc1LDExMUMzODEsMTE0IDM4NSwxMTIgMzg3LDExM0MzOTQsMTE3IDM5NSwxMTUgMzk1LDExNUM0MDIsMTE4IDQwMiwxMTYgNDAyLDExN0M0MTEsMTIxIDQzOCwxMjQgNDQyLDEzNFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIxMjAuOTE0MDA0LDIxNTAuNTUzNjA1KSI+PHBhdGggZD0iTTQ0NSwxMzJDNDQ3LDEzMCA0NDcsMTI5IDQ0OSwxMzFDNDU0LDEzMyA0NTUsMTMxIDQ1OCwxMzJDNDY4LDEzNyA1ODIsMTU0IDU4MiwyNjNDNTgyLDI2NSA1ODAsMjY1IDU4MCwyNjdDNTc5LDMyMSA1NTQsMzIxIDU0NywzMDVDNTQyLDI5NiA1NTgsMjYxIDU0NywyMjhDNTM2LDE5NiA0OTksMTc1IDUwOCwxODRDNTE2LDE5MyA1MTUsMTk0IDUyMiwyMDRDNTM2LDIyNSA1NDIsMjYxIDU0MiwyNjNDNTQyLDI4OSA1NDEsMjg4IDU0MCwzMDZDNTQwLDMwOSA1MzksMzA4IDUzOSwzMTFDNTM4LDMxNiA1MzgsMzE1IDUzNywzMjFDNTM2LDMyOCA1MzUsMzI3IDUzMywzMzRDNTMzLDMzOSA1MjksMzUwIDUyOCwzNTNDNTI2LDM2NCA1MjEsMzY1IDUyNywzNzVDNTM3LDM4OSA2MDUsNDMyIDU0OCw0NjJDNTQxLDQ2NSA1NDIsNDY2IDU0NSw0NzNDNTU0LDQ5MSA1MzUsNDk1IDUzNyw0OTlDNTUyLDUxOCA1MzAsNTI0IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjkxLDQwNCAzMzgsMzQ1IDM5MywzNDVDNDMxLDM0NSA0NDEsMzMyIDQ1NiwzMjBDNDU5LDMxNyA0NTksMzE3IDQ2MiwzMTRDNDY0LDMxMiA0NzYsMjk5IDQ4MywyODZDNTA0LDI0MyA0OTgsMjM5IDUwMSwyMzRDNTA0LDIyNiA1MDIsMjIzIDUwMiwyMjFDNTA2LDIxMiA1MDEsMTk0IDUwNSwxODNDNTA3LDE3OCA0NDcsMTM3IDQ0NSwxMzJaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik04NDMsNDcwQzg0NCw0ODMgODQ1LDQ4MiA4NDUsNDk0Qzg0NSw1MjcgODQ1LDUyNyA4NDIsNTQ2QzgzMyw1OTAgODMxLDU4OSA4MTIsNjI5QzgxMiw2MzAgNzk5LDY0OSA3OTgsNjUwQzc5OCw2NTEgNzc4LDY3NyA3NzAsNjgyQzc2MSw2ODkgNzM0LDY3NyA3NTYsNjU1QzgwNSw2MDYgODQ0LDUwMiA3OTAsNDAxQzc2MSwzNDYgNzMzLDM0NiA3NTUsMzI3Qzc2NywzMTYgNzgxLDMzOCA3OTIsMzUwQzgwMSwzNTkgODM0LDQxNSA4MzcsNDM4QzgzNyw0NDIgODM5LDQ0MiA4NDAsNDUzQzg0MCw0NTQgODQzLDQ2MiA4NDMsNDcwWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNzI2LDQ3MUM3MjUsNDY4IDcyMCw0NTIgNzE4LDQ1MEM2OTcsNDA3IDY3NCw0MDYgNjk0LDM4OEM3MDIsMzgxIDcxNSwzODkgNzE4LDM5NkM3MjEsNDAyIDczMiw0MDkgNzQ0LDQzNkM3NjMsNDc0IDc2MCw1MTMgNzYwLDUxM0M3NTgsNTE5IDc1Nyw1MzUgNzU3LDUzOEM3NTIsNTYyIDczMiw1OTcgNzI1LDYwNEM3MTUsNjE1IDcxNyw2MTggNzA0LDYyNEM2OTksNjI2IDY4OCw2MTcgNjg4LDYxM0M2ODQsNTkzIDcwNiw1OTQgNzIxLDU1M0M3MjUsNTQwIDcyNSw1NDAgNzI4LDUyN0M3MzMsNDk4IDcyNiw0NzggNzI2LDQ3MVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTY1NSw1MTZDNjU1LDUxNCA2NTcsNTA2IDY1NCw0OTFDNjUwLDQ2NyA2MTgsNDUyIDY0Niw0MzdDNjU1LDQzMyA2NzEsNDU0IDY3NSw0NjJDNzA4LDUyNSA2NTcsNTgzIDY0Myw1NjlDNjIwLDU0OCA2NTEsNTQ3IDY1NSw1MTZaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNODQzLDQ3MEM4NDQsNDgzIDg0NSw0ODIgODQ1LDQ5NEM4NDUsNTI3IDg0NSw1MjcgODQyLDU0NkM4MzMsNTkwIDgzMSw1ODkgODEyLDYyOUM4MTIsNjMwIDc5OSw2NDkgNzk4LDY1MEM3OTgsNjUxIDc3OCw2NzcgNzcwLDY4MkM3NjEsNjg5IDczNCw2NzcgNzU2LDY1NUM4MDUsNjA2IDg0NCw1MDIgNzkwLDQwMUM3NjEsMzQ2IDczMywzNDYgNzU1LDMyN0M3NjcsMzE2IDc4MSwzMzggNzkyLDM1MEM4MDEsMzU5IDgzNCw0MTUgODM3LDQzOEM4MzcsNDQyIDgzOSw0NDIgODQwLDQ1M0M4NDAsNDU0IDg0Myw0NjIgODQzLDQ3MFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTcyNiw0NzFDNzI1LDQ2OCA3MjAsNDUyIDcxOCw0NTBDNjk3LDQwNyA2NzQsNDA2IDY5NCwzODhDNzAyLDM4MSA3MTUsMzg5IDcxOCwzOTZDNzIxLDQwMiA3MzIsNDA5IDc0NCw0MzZDNzYzLDQ3NCA3NjAsNTEzIDc2MCw1MTNDNzU4LDUxOSA3NTcsNTM1IDc1Nyw1MzhDNzUyLDU2MiA3MzIsNTk3IDcyNSw2MDRDNzE1LDYxNSA3MTcsNjE4IDcwNCw2MjRDNjk5LDYyNiA2ODgsNjE3IDY4OCw2MTNDNjg0LDU5MyA3MDYsNTk0IDcyMSw1NTNDNzI1LDU0MCA3MjUsNTQwIDcyOCw1MjdDNzMzLDQ5OCA3MjYsNDc4IDcyNiw0NzFaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik02NTUsNTE2QzY1NSw1MTQgNjU3LDUwNiA2NTQsNDkxQzY1MCw0NjcgNjE4LDQ1MiA2NDYsNDM3QzY1NSw0MzMgNjcxLDQ1NCA2NzUsNDYyQzcwOCw1MjUgNjU3LDU4MyA2NDMsNTY5QzYyMCw1NDggNjUxLDU0NyA2NTUsNTE2WiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjwvZz48L3N2Zz4=)";
        }
        else
        {
            document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik0zMzEsMzM3QzI5NywzNTUgMjk4LDM2MyAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMSwyNjYgMTM5LDE3MUwzMzEsMzM3Wk0xNzksMTQ0QzE4OCwxMzkgMTk1LDEzNSAxOTksMTM0QzIwMywxMzIgMjQ2LDExOCAyNDksMTE4QzI2OCwxMTcgMjY3LDExMyAyODksMTEzQzI5MSwxMTMgMjkwLDExMSAyOTIsMTExQzMxMSwxMDkgMzcxLDEwOSAzNzUsMTExQzM4MSwxMTQgMzg1LDExMiAzODcsMTEzQzM5NCwxMTcgMzk1LDExNSAzOTUsMTE1QzQwMiwxMTggNDAyLDExNiA0MDIsMTE3QzQxMSwxMjEgNDM4LDEyNCA0NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxNCwzMTggNDAwLDMxOCAzODMsMzIxTDE3OSwxNDRaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00MTAsMzQ0QzQzNCwzNDEgNDQzLDMzMCA0NTYsMzIwQzQ1OSwzMTcgNDU5LDMxNyA0NjIsMzE0QzQ2NCwzMTIgNDc2LDI5OSA0ODMsMjg2QzUwNCwyNDMgNDk4LDIzOSA1MDEsMjM0QzUwNCwyMjYgNTAyLDIyMyA1MDIsMjIxQzUwNiwyMTIgNTAxLDE5NCA1MDUsMTgzQzUwNywxNzggNDQ3LDEzNyA0NDUsMTMyQzQ0NywxMzAgNDQ3LDEyOSA0NDksMTMxQzQ1NCwxMzMgNDU1LDEzMSA0NTgsMTMyQzQ2OCwxMzcgNTgyLDE1NCA1ODIsMjYzQzU4MiwyNjUgNTgwLDI2NSA1ODAsMjY3QzU3OSwzMjEgNTU0LDMyMSA1NDcsMzA1QzU0MiwyOTYgNTU4LDI2MSA1NDcsMjI4QzUzNiwxOTYgNDk5LDE3NSA1MDgsMTg0QzUxNiwxOTMgNTE1LDE5NCA1MjIsMjA0QzUzNiwyMjUgNTQyLDI2MSA1NDIsMjYzQzU0MiwyODkgNTQxLDI4OCA1NDAsMzA2QzU0MCwzMDkgNTM5LDMwOCA1MzksMzExQzUzOCwzMTYgNTM4LDMxNSA1MzcsMzIxQzUzNiwzMjggNTM1LDMyNyA1MzMsMzM0QzUzMywzMzkgNTI5LDM1MCA1MjgsMzUzQzUyNiwzNjQgNTIxLDM2NSA1MjcsMzc1QzUzNywzODkgNjA1LDQzMiA1NDgsNDYyQzU0Nyw0NjIgNTQ3LDQ2MiA1NDYsNDYyTDQxMCwzNDRaTTU0MCw1MTlDNTM2LDUyNCA1MzEsNTI3IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjg2LDQwNyAzMTUsMzczIDM1MiwzNTZMNTQwLDUxOVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTg0Myw0NzBDODQ0LDQ4MyA4NDUsNDgyIDg0NSw0OTRDODQ1LDUyNyA4NDUsNTI3IDg0Miw1NDZDODMzLDU5MCA4MzEsNTg5IDgxMiw2MjlDODEyLDYzMCA3OTksNjQ5IDc5OCw2NTBDNzk4LDY1MSA3NzgsNjc3IDc3MCw2ODJDNzYxLDY4OSA3MzQsNjc3IDc1Niw2NTVDODA1LDYwNiA4NDQsNTAyIDc5MCw0MDFDNzYxLDM0NiA3MzMsMzQ2IDc1NSwzMjdDNzY3LDMxNiA3ODEsMzM4IDc5MiwzNTBDODAxLDM1OSA4MzQsNDE1IDgzNyw0MzhDODM3LDQ0MiA4MzksNDQyIDg0MCw0NTNDODQwLDQ1NCA4NDMsNDYyIDg0Myw0NzBaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik03MjYsNDcxQzcyNSw0NjggNzIwLDQ1MiA3MTgsNDUwQzY5Nyw0MDcgNjc0LDQwNiA2OTQsMzg4QzcwMiwzODEgNzE1LDM4OSA3MTgsMzk2QzcyMSw0MDIgNzMyLDQwOSA3NDQsNDM2Qzc2Myw0NzQgNzYwLDUxMyA3NjAsNTEzQzc1OCw1MTkgNzU3LDUzNSA3NTcsNTM4Qzc1Miw1NjIgNzMyLDU5NyA3MjUsNjA0QzcxNSw2MTUgNzE3LDYxOCA3MDQsNjI0QzY5OSw2MjYgNjg4LDYxNyA2ODgsNjEzQzY4NCw1OTMgNzA2LDU5NCA3MjEsNTUzQzcyNSw1NDAgNzI1LDU0MCA3MjgsNTI3QzczMyw0OTggNzI2LDQ3OCA3MjYsNDcxWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNjU1LDUxNkM2NTUsNTE0IDY1Nyw1MDYgNjU0LDQ5MUM2NTAsNDY3IDYxOCw0NTIgNjQ2LDQzN0M2NTUsNDMzIDY3MSw0NTQgNjc1LDQ2MkM3MDgsNTI1IDY1Nyw1ODMgNjQzLDU2OUM2MjAsNTQ4IDY1MSw1NDcgNjU1LDUxNloiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjY1ODc0OCwwLjU3MjE0LC0wLjY1NTczMiwwLjc1NDk5NCwyODczLjU3MjI1NywtMTEyMy4zODAwNykiPjxyZWN0IHg9IjE5NTIiIHk9IjMwMDMiIHdpZHRoPSIxMDk4IiBoZWlnaHQ9IjQ3IiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjwvc3ZnPg==)";
        }

        // kept so a save that did not touch the server details can not tear down a live
        // connection. captured before the keys array below is cleared and refilled
        let previous_autoconnect_host = g_autoconnect_details.host;
        let previous_autoconnect_port = g_autoconnect_details.port;
        let previous_autoconnect_keys = (g_autoconnect_details.keys || []).join("\n");
        // gaining or losing the loopback must count as a change, because a webview that
        // connected directly before node announced itself would otherwise never re-route through it
        let previous_loopback = g_loopback_port + ":" + g_loopback_token;

        g_autoconnect_details.host = settings_from_android.host;
        g_autoconnect_details.port = settings_from_android.port;

        // null it out in case it wasnt
        g_autoconnect_details.keys.length = 0;
        g_autoconnect_details.keys = [];

        // metadata_keys is optional, because the json grows incrementally (identity
        // first, app_mode on the first-run answer) and only a native-settings save
        // writes the full form - and a server without keys never gets any
        if (settings_from_android.metadata_keys != null)
        {
            for (i = 0; i < settings_from_android.metadata_keys.length; i++)
            {
                g_autoconnect_details.keys.push(settings_from_android.metadata_keys[i]);
            }
        }

        // the loopback fields only ever appear in the webview's settings json
        g_loopback_port = (typeof settings_from_android.loopback_port === "number") ? settings_from_android.loopback_port : 0;

        g_loopback_token = (typeof settings_from_android.loopback_token === "string") ? settings_from_android.loopback_token : "";

        // no unconditional spinner hold here, because connection__request_connect raises the spinner
        // only when it actually dials; a save with autoconnect off changes nothing on screen

        // the data worker has its own copy of the flag and encrypts outgoing direct
        // messages itself, so it must skip that in loopback mode too
        if (g_data_processing_worker != null)
        {
            g_data_processing_worker.postMessage({ type: "mainthread__set_loopback_mode", value: g_loopback_port });
        }

        console.log("accept_current_settings_from_android autoconnect_details" + JSON.stringify(g_autoconnect_details));

        // a save that changed the server means the live connection is stale
        let are_server_details_changed = (g_autoconnect_details.host != previous_autoconnect_host)
            || (g_autoconnect_details.port != previous_autoconnect_port)
            || ((g_autoconnect_details.keys || []).join("\n") != previous_autoconnect_keys)
            || ((g_loopback_port + ":" + g_loopback_token) != previous_loopback);

        if (are_server_details_changed && g_is_authenticated == true)
        {
            console.log("connect-path: server details changed, dropping the live connection");
            g_websocket_worker.postMessage({ type: "close" });
        }

        // the android default username becomes the chosen username, because the settings
        // arrive before the dial and the server then applies the name at login - the old
        // way was a rename after connect, which an admin can switch off for users
        if (typeof settings_from_android.default_username === "string" && settings_from_android.default_username.length > 0)
        {
            g_chosen_username = settings_from_android.default_username;
        }

        // only node derives its identity from the settings, because the webview does not
        // own the connection. whether to actually dial is decided inside connection__request_connect,
        // and that holds for both runtimes
        if (android_host__is_ui_only_runtime() == false)
        {
            connection__request_identity(settings_from_android.identity_string);
        }

        connection__request_connect("settings");
    }
}

// ---- idle mode ----

/**
 * @brief remembers that a come-from-idle request is waiting for the server's answer, so idle entry is deferred meanwhile
 *        if the answer never comes, the flag clears itself after 20 s so idle is not blocked forever
 *
 * @return void
 */
function android_host__mark_come_from_idle_in_flight()
{
    is_come_from_idle_in_flight = true;

    if (come_from_idle_in_flight_timer != null)
    {
        clearTimeout(come_from_idle_in_flight_timer);
    }

    // if the answer never comes, give up after a while so idle is not blocked forever
    come_from_idle_in_flight_timer = setTimeout(function()
    {
        come_from_idle_in_flight_timer = null;
        is_come_from_idle_in_flight = false;
        console.log("connect-path: come-from-idle was never confirmed, idle allowed again");
        android_host__apply_pending_deep_idle_if_any();
    }, 20000);
}

/**
 * @brief the server answered the come-from-idle: idle is allowed again, and a deferred idle entry runs now
 *
 * @return void
 */
function android_host__clear_come_from_idle_in_flight()
{
    if (come_from_idle_in_flight_timer != null)
    {
        clearTimeout(come_from_idle_in_flight_timer);
        come_from_idle_in_flight_timer = null;
    }

    is_come_from_idle_in_flight = false;

    android_host__apply_pending_deep_idle_if_any();
}

/**
 * @brief runs an idle entry that android_host__enter_deep_idle deferred while a come-from-idle was unconfirmed
 *        android_host__exit_deep_idle cancels the pending flag, so a foregrounded app never re-enters idle here
 *
 * @return void
 */
function android_host__apply_pending_deep_idle_if_any()
{
    if (g_is_deep_idle_pending == true && g_is_deep_idle == false)
    {
        g_is_deep_idle_pending = false;
        console.log("connect-path: running deferred idle entry");
        android_host__enter_deep_idle();
    }
}

/**
 * @brief holds presence for a moment after a call accept
 *        accepting a call blinks the screen off or hands the call screen over to the activity, and
 *        both read as "nobody is looking" exactly while the user IS there, which idled people right
 *        back out of the call they just answered
 *
 * @return void
 */
function android_host__mark_call_accept_presence_grace()
{
    presence_grace_until_timestamp = new Date().valueOf() + CALL_ACCEPT_PRESENCE_GRACE_MS;
    console.log("connect-path: call accepted, holding presence for " + CALL_ACCEPT_PRESENCE_GRACE_MS + "ms");
}

/**
 * @brief one shot at the end of the presence grace: node re-reads whether a ui is attached, the webview retries the gentle idle entry
 *        when the user actually came back, android_host__exit_deep_idle cancelled this and nothing runs
 *
 * @return void
 */
function android_host__schedule_presence_grace_recheck()
{
    if (presence_grace_recheck_timer != null)
    {
        return;
    }

    let wait_ms = Math.max(250, (presence_grace_until_timestamp - new Date().valueOf()) + 250);

    presence_grace_recheck_timer = setTimeout(function()
    {
        presence_grace_recheck_timer = null;

        if (typeof process !== "undefined")
        {
            android_host__node_apply_idle_for_ui_state();
        }
        else
        {
            android_host__enter_deep_idle(false);
        }
    }, wait_ms);
}

/**
 * @brief drops the queued presence-grace re-check
 *
 * @return void
 */
function android_host__cancel_presence_grace_recheck()
{
    if (presence_grace_recheck_timer != null)
    {
        clearTimeout(presence_grace_recheck_timer);
        presence_grace_recheck_timer = null;
    }
}

/**
 * @brief deep idle, entered when the android app goes to background: keeps the websocket alive on a slow heartbeat and shuts down everything else (audio graph, opus decoder tick, webrtc datachannel)
 *        every refusal is logged, because a silent one is indistinguishable from the request never
 *        arriving; an unconfirmed come-from-idle defers the entry instead of dropping it
 *
 * @param boolean is_forced -> true idles from any channel, false keeps an in-channel session alive
 *
 * @return void
 */
function android_host__enter_deep_idle(is_forced)
{
    // every refusal below says so. a silent one is indistinguishable from the request
    // never arriving, which is what made this so hard to pin down
    if (g_is_deep_idle == true)
    {
        console.log("connect-path: idle requested, already idle");
        return;
    }

    // an unconfirmed come-from-idle blocks idle entry, but the request must not be thrown
    // away: dropping it left the page awake while the server considered the client idle,
    // and every datachannel handshake attempted from that split state was doomed
    if (is_come_from_idle_in_flight == true)
    {
        console.log("connect-path: idle deferred, a come-from-idle is still unconfirmed");
        g_is_deep_idle_pending = true;
        return;
    }

    // app backgrounded before authentication finished - remember it, the auth success handler
    // re-enters (fixes the "went to background while still connecting" race)
    if (g_is_authenticated == false)
    {
        console.log("connect-path: idle requested while not authenticated, deferred to login");
        g_is_deep_idle_pending = true;
        return;
    }

    // inside the call-accept presence grace: hold, then re-decide from the real state.
    // a swipe-away (is_forced) stays deliberate and is honoured immediately
    if (is_forced != true && new Date().valueOf() < presence_grace_until_timestamp)
    {
        console.log("connect-path: idle deferred, inside the call-accept presence grace");
        android_host__schedule_presence_grace_recheck();
        return;
    }

    // backgrounding (home) keeps an in-channel session alive - music and calls continue.
    // only a swipe-away forces idle from any channel (is_forced, from onTaskRemoved)
    if (is_forced != true)
    {
        let local_client = channel_tree__get_client_by_client_id(g_local_client_id);

        if (local_client != null && local_client.channel_id != 0)
        {
            console.log("connect-path: idle refused, in channel " + local_client.channel_id + " not root");
            return;
        }
    }

    console.log("connect-path: going idle" + (is_forced == true ? " (forced)" : ""));

    g_is_deep_idle = true;

    client_msg.send_go_to_idle_mode_request();

    // heartbeat 10s -> 120s, loss detector 120s -> 360s (three missed checks)
    g_connection_check.interval_ms = 120 * 1000;
    g_connection_check.lost_threshold_ms = 360 * 1000;

    if (g_audio_context != null && g_audio_context.state === "running")
    {
        audio_suspend_promise = g_audio_context.suspend();
    }

    // a push-to-talk held down at background time must not keep capturing all idle long;
    // capture stays off until the next press. guarded: node has no audio at all, and
    // an unguarded call threw out of here, so headless node never actually went idle
    if (typeof audio__set_microphone_capture_active === "function")
    {
        audio__set_microphone_capture_active(false);
    }

    if (typeof voice__set_mic_transmitting_visual === "function")
    {
        voice__set_mic_transmitting_visual(false);
    }

    if (g_opus_decoder_worker != null)
    {
        g_opus_decoder_worker.postMessage({ type: "deep_idle_stop" });
    }

    // the datachannel's ICE consent checks ping the server every few seconds even when nothing
    // streams - close it, android_host__exit_deep_idle re-establishes it
    if (g_peer_connection_with_server != null)
    {
        try
        {
            g_peer_connection_with_server.close();
        }
        catch (e)
        {
            console.log("deep idle: peer connection close failed ", e.message);
        }
        g_peer_connection_with_server = null;
        g_datachannel = null;
        g_is_webrtc_datachannel_connected = false;
    }
}

/**
 * @brief leaves deep idle: restores the heartbeat cadence, the audio graph, the opus tick and the webrtc datachannel
 *
 * @return void
 */
function android_host__exit_deep_idle()
{
    // the user is demonstrably here: a queued presence-grace re-check must not fire behind them.
    // before the was-not-idle return, because the webview arms that timer without ever being idle
    android_host__cancel_presence_grace_recheck();

    g_is_deep_idle_pending = false;

    if (g_is_deep_idle == false)
    {
        console.log("connect-path: leave-idle requested, was not idle");
        return;
    }

    // we really were idle, so an answer from the server is coming. armed here rather
    // than where the request is sent, because a request sent while not idle gets none
    android_host__mark_come_from_idle_in_flight();

    console.log("connect-path: leaving idle");

    g_is_deep_idle = false;

    g_connection_check.interval_ms = 10 * 1000;
    g_connection_check.lost_threshold_ms = 35 * 1000; // keep in sync with the startup value
    g_connection_check.last_response_timestamp = new Date().valueOf();

    // wake the heartbeat loop out of its long idle sleep so a fresh check goes out now
    if (g_connection_check.sleep_resolve != null)
    {
        g_connection_check.sleep_resolve();
        g_connection_check.sleep_resolve = null;
    }

    if (g_audio_context != null)
    {
        // wait for idle entry's suspend to settle before acting, so the two can never race
        let suspend_settled = (audio_suspend_promise != null) ? audio_suspend_promise : Promise.resolve();
        audio_suspend_promise = null;

        suspend_settled.catch(function() {}).then(function()
        {
            // android resets the audio HAL under a backgrounded webview; resume() then
            // "succeeds" into a broken graph (gurgling both ways), so rebuild instead
            if (g_android_app_mode != "")
            {
                audio__rebuild_audio_graph_after_idle();
                return;
            }

            // browsers: resume, but verify it took - rebuild when the context stays stuck
            g_audio_context.resume().catch(function(e) { console.log("audio resume rejected: " + e.message); });
            setTimeout(function()
            {
                if (g_audio_context.state !== "running")
                {
                    console.log("audio context stuck in '" + g_audio_context.state + "' after resume, rebuilding");
                    audio__rebuild_audio_graph_after_idle();
                }
            }, 1000);
        });
    }

    if (g_opus_decoder_worker != null)
    {
        g_opus_decoder_worker.postMessage({ type: "deep_idle_resume" });
    }

    // re-establish the webrtc datachannel that idle entry closed
    if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_webrtc_datachannel_check_running == false)
    {
        g_is_webrtc_datachannel_check_running = true;
        voice__webrtc_datachannel_connection_check(true);
    }
}
