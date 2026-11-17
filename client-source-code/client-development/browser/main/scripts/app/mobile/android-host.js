// connects the shared client to Android's Node owner and WebView

// wait for audio suspension before resuming playback

var audio_suspend_promise = null;

// defers idle entry while a return from idle is pending
var is_come_from_idle_in_flight = false;

var come_from_idle_in_flight_timer = null;

// keeps presence active briefly after accepting a call

var CALL_ACCEPT_PRESENCE_GRACE_MS = 5000;

var presence_grace_until_timestamp = 0;

var presence_grace_recheck_timer = null;
var pending_deep_idle_forced = false;
var node_dispatch_generation = null;
var node_dispatch_frames = null;
var node_unread_listeners = [];
var node_session_id = "";
var node_notification_listener = null;
var node_notification_sequence = 0;
var node_current_call = null;
var node_last_call_action = null;
var node_identity_blocked = false;
var node_ui_forced_idle = false;

/**
 * @brief creates a random identifier for a session or call
 *
 * @return a 32 character hexadecimal identifier
 */
function android_host__new_scope_id()
{
    const bytes = new Uint8Array(16);
    global.crypto.getRandomValues(bytes);
    return Array.from(bytes, function(value)
    {
        return value.toString(16).padStart(2, "0");
    }).join("");
}

/**
 * @brief starts a session and clears previous call and notification state
 */
function android_host__begin_session()
{
    node_session_id = android_host__new_scope_id();
    node_current_call = null;
    node_last_call_action = null;
    node_notification_sequence = 0;
}

/**
 * @brief publishes an event with its session and sequence identifiers
 *
 * @param event notification event to publish
 */
function android_host__notify(event)
{
    if (typeof process === "undefined" || node_notification_listener == null)
    {
        return;
    }
    node_notification_listener(Object.assign({}, event,
    {
        session_id: node_session_id,
        event_id: ++node_notification_sequence
    }));
}

/**
 * @brief stores a current call and notifies the native listener
 *
 * @param username caller display name
 * @param channel_id channel entered when the call is accepted
 */
function android_host__incoming_call(username, channel_id)
{
    node_current_call =
    {
        call_id: android_host__new_scope_id(),
        session_id: node_session_id,
        expires_at: Date.now() + 60000,
        caller_username: username,
        channel_id: channel_id
    };
    if (g_node_incoming_call_listener != null)
    {
        g_node_incoming_call_listener(username, channel_id, Object.assign({}, node_current_call));
    }
}

/**
 * @brief applies an accept or decline action for the current call
 *
 * @param action accept or decline command
 * @param record call and session identifiers
 *
 * @return true when the action was applied or had already succeeded
 */
function android_host__handle_call_action(action, record)
{
    if (!record || !g_is_authenticated || record.session_id !== node_session_id)
    {
        return false;
    }
    if (node_last_call_action && node_last_call_action.expires_at > Date.now() && node_last_call_action.call_id === record.call_id && node_last_call_action.action === action && node_last_call_action.session_id === record.session_id)
    {
        if (action === "accept_call")
        {
            const was_idle = g_is_deep_idle;
            node_ui_forced_idle = false;
            android_host__mark_call_accept_presence_grace();
            android_host__exit_deep_idle();
            if (was_idle)
            {
                // restore presence if native recovery replayed an older idle state
                client_msg.send_come_from_idle_mode_request(node_last_call_action.channel_id);
            }
        }
        return true;
    }
    if (!node_current_call || node_current_call.expires_at <= Date.now() || record.session_id !== node_session_id || record.call_id !== node_current_call.call_id)
    {
        return false;
    }
    if (action !== "accept_call" && action !== "decline_call")
    {
        return false;
    }
    const call = node_current_call;
    node_current_call = null;
    node_last_call_action =
    {
        call_id: call.call_id, session_id: call.session_id, expires_at: call.expires_at,
        channel_id: call.channel_id, action: action
    };
    if (action === "accept_call")
    {
        node_ui_forced_idle = false;
        android_host__mark_call_accept_presence_grace();
        android_host__exit_deep_idle();
        client_msg.send_come_from_idle_mode_request(call.channel_id);
    }
    return true;
}

/**
 * @brief normalizes metadata keys for change detection
 *
 * @param values metadata key entries to validate
 *
 * @return serialized key entries. throws when an entry is invalid
 */
function android_host__metadata_keys_signature(values)
{
    if (!Array.isArray(values))
    {
        throw new Error("invalid metadata keys");
    }
    return JSON.stringify(values.map(function(value)
    {
        const entry = connection_crypto__password_entry(value);
        if (entry == null)
        {
            throw new Error("invalid metadata key");
        }
        return [entry.type, entry.password];
    }));
}

/**
 * @brief checks whether this runtime renders a connection owned by Node
 *
 * @return boolean true for a WebView or worker using the local Node connection
 */
function android_host__is_ui_only_runtime()
{
    return (typeof Android !== "undefined") || g_loopback_port > 0;
}

/**
 * @brief creates a worker placeholder that discards messages
 *
 * @return object worker placeholder with postMessage and terminate methods
 */
function android_host__make_discarding_worker()
{
    return {
        postMessage: function()
        {
        },
        terminate: function()
        {
        }
    };
}

/**
 * @brief checks whether this runtime may infer that chat is being read
 *
 * @return boolean false in Node, where page read commands are required
 */
function android_host__is_someone_watching_the_ui()
{
    if (typeof process === "undefined")
    {
        return true;
    }

    // Node waits for an explicit page read before clearing unread state
    return false;
}

/**
 * @brief applies Node idle state from the current page attachment
 *
 * @return void
 */
function android_host__node_apply_idle_for_ui_state()
{
    if (typeof process === "undefined")
    {
        return; // the page receives idle changes from the activity lifecycle
    }

    // apply the current attachment state through the shared idle handlers

    try
    {
        if (g_node_has_attached_ui == true)
        {
            if (!g_is_deep_idle)
            {
                return;
            }
            // rejoin the previous channel or use root when none is known
            let channel_to_rejoin = (typeof g_current_channel_id === "number") ? g_current_channel_id : 0;

            console.log("connect-path: ui attached, leaving idle into channel " + channel_to_rejoin);
            android_host__exit_deep_idle();
            client_msg.send_come_from_idle_mode_request(channel_to_rejoin);
        }
        else
        {
            console.log("connect-path: no ui attached, going idle");
            android_host__enter_deep_idle(node_ui_forced_idle);
        }
    }
    catch (idle_switch_failed)
    {
        console.error("idle switch failed: " + idle_switch_failed);
    }
}

g_show_message_avatars = (utils__storage_get("lemon_show_message_avatars") === "1");

g_use_theme_chat_formatting = (utils__storage_get("lemon_use_theme_chat_formatting") === "1");

let stored_audio_connection = utils__storage_get("lemon_audio_connection", "");
if (["webrtc", "websocket"].indexOf(stored_audio_connection) >= 0)
{
    g_audio_connection_preference = stored_audio_connection;
}

let stored_rsa_key_bits = parseInt(utils__storage_get("lemon_rsa_key_bits", ""));
if (G_ALLOWED_RSA_KEY_BITS.indexOf(stored_rsa_key_bits) >= 0)
{
    g_rsa_key_bits = stored_rsa_key_bits;
}

g_show_seen_indicator = (utils__storage_get("lemon_seen_indicator") !== "0");

g_send_seen_receipts = (utils__storage_get("lemon_send_seen") !== "0");

g_auto_scroll_chat_to_end = (utils__storage_get("lemon_auto_scroll") !== "0");

g_hide_microphone_button = (utils__storage_get("lemon_hide_mic") === "1");

g_hide_mobile_microphone_button = (utils__storage_get("lemon_hide_mobile_mic") === "1");

g_selected_microphone_device_id = utils__storage_get("lemon_mic_device_id", "");

g_is_continuous_mic_mode = (utils__storage_get("lemon_continuous_mic") === "1");

g_is_push_to_talk_disabled_while_typing = (utils__storage_get("lemon_ptt_disable_while_typing") !== "0");

let stored_key_code = parseInt(utils__storage_get("lemon_ptt_key_code"));
let stored_key_label = utils__storage_get("lemon_ptt_key_label");
if (stored_key_code > 0)
{
    g_push_to_talk_key_code = stored_key_code;
}
if (stored_key_label)
{
    g_push_to_talk_key_label = stored_key_label;
}

/**
 * @brief builds a message sender header with an optional avatar
 *
 * @param sender_client_id sender ID or null for an untagged header
 * @param display_name_html escaped display name
 *
 * @return escaped sender header HTML
 */
function android_host__generate_message_sender_html(sender_client_id, display_name_html)
{
    let avatar_html = "";

    if (g_show_message_avatars == true && sender_client_id != null)
    {
        let sender = channel_tree__get_client_by_client_id(sender_client_id);

        if (sender != null && typeof sender.base64_avatar === "string" && sender.base64_avatar.length > 0)
        {
            avatar_html = "<img class=\"chat-message-avatar\" alt=\"\" src=\"" + chat__sanitize_string(sender.base64_avatar) + "\">";
        }
        else
        {
            let initials = sender != null ? chat__sanitize_string(UI.initials_for_name(sender.username)) : "?";
            avatar_html = '<span class="chat-message-avatar chat-message-avatar-placeholder" aria-hidden="true">' + initials + '</span>';
        }
    }

    let id_attribute = (sender_client_id != null) ? (" data-sender-id=\"" + sender_client_id + "\"") : "";

    return avatar_html + "<p" + id_attribute + ">" + display_name_html + "</p>";
}

/**
 * @brief updates message avatars after image or display changes
 *
 * @param number|undefined sender_client_id -> sender to update or all senders when omitted
 *
 * @return void
 */
function android_host__refresh_message_avatars(sender_client_id)
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    document.body.classList.toggle("message-avatars-hidden", !g_show_message_avatars);

    let names = document.querySelectorAll('.single-message-content > :is(.single-chat-message-sender-username-container, .single-chat-message-sender-local-username-container) > p[data-sender-id]');
    for (let name of names)
    {
        let client_id = name.getAttribute("data-sender-id");
        if (sender_client_id != null && String(sender_client_id) !== client_id)
        {
            continue;
        }

        let sender = channel_tree__get_client_by_client_id(Number(client_id));
        let avatar = name.parentElement.querySelector(".chat-message-avatar");
        if (g_show_message_avatars == false)
        {
            if (avatar != null)
            {
                avatar.remove();
            }
            continue;
        }
        if (sender == null)
        {
            continue;
        }
        let has_picture = typeof sender.base64_avatar === "string" && sender.base64_avatar.length > 0;
        let tag = has_picture ? "IMG" : "SPAN";
        if (avatar != null && avatar.tagName !== tag)
        {
            avatar.remove();
            avatar = null;
        }
        if (avatar == null)
        {
            avatar = document.createElement(tag);
            avatar.className = has_picture ? "chat-message-avatar" : "chat-message-avatar chat-message-avatar-placeholder";
            avatar.setAttribute("aria-hidden", "true");
            if (has_picture)
            {
                avatar.alt = "";
            }
            name.before(avatar);
        }
        if (has_picture)
        {
            if (avatar.getAttribute("src") !== sender.base64_avatar)
            {
                avatar.src = sender.base64_avatar;
            }
        }
        else
        {
            avatar.textContent = UI.initials_for_name(sender.username);
        }
    }
    if (typeof chat_replies__refresh_avatars === "function")
    {
        chat_replies__refresh_avatars(sender_client_id);
    }
}

/**
 * @brief updates rendered message headers after a sender is renamed
 *
 * @param number sender_client_id -> sender whose headers should change
 * @param string display_name -> new display name
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
    android_host__refresh_message_avatars(sender_client_id);
}

/**
 * @brief sets whether Node should keep a remote connection
 *
 * @param boolean is_wanted -> true when Node should keep a connection
 *
 * @return void
 */
function android_host__node_set_connection_wanted(is_wanted)
{
    g_node_connection_wanted = (is_wanted == true);

    if (g_node_connection_wanted == false && g_websocket_worker != null)
    {
        connection__invalidate_pending_attempts();
        g_websocket_worker.postMessage({ type: "close" });
    }

    // the caller requests any new connection separately

}

/**
 * @brief dispatches a current worker message and reports handler failures
 *
 * @param function handler -> worker message handler
 * @param object message -> message passed in the data property
 * @param string source_name -> worker name used in error logs
 *
 * @return void
 */
function android_host__dispatch_safely(handler, message, source_name)
{
    if (message == null || typeof message !== "object")
    {
        return;
    }
    if (message.connection_generation != null && message.connection_generation !== g_worker_connection_generation)
    {
        return;
    }
    const previous_generation = node_dispatch_generation;
    const previous_frames = node_dispatch_frames;
    const frames = [];
    node_dispatch_frames = frames;
    node_dispatch_generation = message.connection_generation == null ? null : message.connection_generation;
    let caught_error = null;

    try
    {
        handler({ data: message });
    }
    catch (dispatch_error)
    {
        caught_error = dispatch_error;
        console.error("bridge handler failed via " + source_name);
    }
    finally
    {
        node_dispatch_generation = previous_generation;
        node_dispatch_frames = previous_frames;
    }

    if (caught_error == null && frames.length > 0)
    {
        const generation = message.connection_generation == null ? g_worker_connection_generation : message.connection_generation;
        // publish only after queued handlers have updated chat state

        setTimeout(function()
        {
            if (generation !== g_worker_connection_generation || g_worker_connection_ended)
            {
                return;
            }
            for (const frame of frames)
            {
                if (g_node_frame_listener != null)
                {
                    g_node_frame_listener(null, {release_delivery: frame});
                }
            }
        }, 0);
    }

    // notify the owner that message processing finished

    for (let i = 0; i < g_node_message_listeners.length; i++)
    {
        try
        {
            g_node_message_listeners[i](((message != null && message.type) ? message.type : ""), caught_error != null);
        }
        catch (callback_error)
        {
            console.error("bridge message listener failed");
        }
    }
}

/**
 * @brief creates an asynchronous adapter for a shared worker handler
 *
 * @param function handler -> worker message handler
 * @param string worker_name -> worker name used in error logs
 *
 * @return object adapter with postMessage and terminate methods
 */
function android_host__make_loopback_worker(handler, worker_name)
{
    return {
        postMessage: function(message)
        {
            setTimeout(function()
            {
                android_host__dispatch_safely(handler, message, worker_name);
            }, 0);
        },
        terminate: function()
        {
        }
    };
}

/**
 * @brief initializes workers, heartbeat checks and identity state in Node
 *
 * @param string|null identity_passphrase_string -> saved passphrase or null when settings will supply it
 *
 * @return void
 */
function android_host__init_node_runtime(identity_passphrase_string)
{
    // use the log element provided by the DOM shim

    g_textarea_log = document.getElementById("textarea-log");

    // route worker replies through the shared dispatcher

    global.postMessage = function(message)
    {
        if (message == null || typeof message !== "object")
        {
            return;
        }
        if (message.connection_generation == null && node_dispatch_generation != null)
        {
            message = Object.assign({}, message, {connection_generation: node_dispatch_generation});
        }
        if (message.connection_generation != null && message.connection_generation !== g_worker_connection_generation)
        {
            return;
        }
        // forward decrypted frames separately from worker messages
        if (message != null && message.type === "decrypted_frame")
        {
            // parse matching frames before caching authentication data

            if (message.value.indexOf("\"authentication_status\"") !== -1)
            {
                try
                {
                    if (JSON.parse(message.value).message.type === "authentication_status")
                    {
                        g_node_cached_auth_frame = message.value;
                    }
                }
                catch (e)
                {
                }
            }

            if (node_dispatch_frames != null)
            {
                if (g_node_frame_listener != null)
                {
                    const reservation = g_node_frame_listener(message.value, {defer_delivery: true});
                    if (typeof reservation === "string")
                    {
                        node_dispatch_frames.push(reservation);
                    }
                }
            }
            else if (g_node_frame_listener != null)
            {
                try
                {
                    g_node_frame_listener(message.value);
                }
                catch (listener_error)
                {
                    console.error("frame listener failed: " + listener_error);
                }
            }
            return;
        }

        setTimeout(function()
        {
            android_host__dispatch_safely(dispatch__mainthread_onmessage, message, "mainthread");
        }, 0);
    };

    // run the shared socket and data workers through the local adapter

    g_websocket_worker = android_host__make_loopback_worker(workers__websocket_worker_onmessage, "websocket_worker");
    g_data_processing_worker = android_host__make_loopback_worker(workers__data_processing_worker_onmessage, "data_processing_worker");

    // forward decrypted frames for page replay
    g_data_processing_worker.postMessage({ type: "mainthread__set_frame_forwarding", value: true });

    // check heartbeat expiry independently of the main heartbeat loop

    setInterval(function()
    {
        try
        {
            let heartbeat_age = new Date().valueOf() - g_connection_check.last_response_timestamp;

            if (g_is_authenticated == true && connection__has_keepalive_timed_out(new Date().valueOf(), 15000))
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

    // log connection state once per minute
    setInterval(function()
    {
        try
        {
            let heartbeat_age = (g_connection_check.last_response_timestamp > 0) ? (new Date().valueOf() - g_connection_check.last_response_timestamp) : -1;
            console.log("conn: auth=" + g_is_authenticated + " ws=" + g_is_websocket_connected + " hb_age_ms=" + heartbeat_age + " checker=" + g_should_connection_check_be_running);
        }
        catch (log_error)
        {
        }
    }, 60000);

    // discard audio codec work in the headless runtime

    g_opus_decoder_worker = android_host__make_discarding_worker();
    g_opus_encoder_worker = android_host__make_discarding_worker();
    g_minimp3_worker = android_host__make_discarding_worker();

    // start a supplied identity before waiting for connection settings

    if (typeof identity_passphrase_string === "string" && identity_passphrase_string.length >= 199)
    {
        connection__request_identity(identity_passphrase_string);
    }

    connection__connect_loop();
}

/**
 * @brief updates the controls shown for Android settings
 *
 * @param object settings_from_android -> the current native settings
 *
 * @return void
 */
function android_host__apply_settings_presentation(settings_from_android)
{
    if (!G_HAS_DOM)
    {
        return;
    }

    // hide the log panel when logging is disabled

    if (settings_from_android.is_app_log_enabled == true)
    {
        document.getElementById("show-hide-log-button").style.display = "";
    }
    else
    {
        document.getElementById("show-hide-log-button").style.display = "none";
        document.getElementById("textarea-log").style.display = "none";
    }

    // use native settings for the server address

    document.getElementById("connect-form-grid").style.display = "none";
    document.getElementById("add-key-button").style.display = "none";

    // hide bookmarks because server settings are native
    document.getElementById("server-bookmarks-container").style.display = "none";

    if (g_is_autoconnect_without_user_action_active == false)
    {
        document.getElementById("another-buttons-sub-container").style.display = "";
        document.getElementById("another-buttons-sub-loading-container").style.display = "none";
        // show Connect when autoconnect is disabled

        document.getElementById("connect-button").style.visibility = "visible";
        document.getElementById("import-identity-button").style.display = "none";

        // reveal the connect page in manual mode
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
}

var android_js_bridge = {
    /**
     * @brief applies an idle request from Android
     *
     * @param boolean is_forced -> true to idle from any channel
     *
     * @return void
     */
    send_go_to_idle_mode_request_android: function(is_forced)
    {
        // apply the native idle request

        android_host__enter_deep_idle(is_forced === true);
    },

    /**
     * @brief restores media and requests entry into a channel
     *
     * @param number|null channelId -> channel to rejoin or null for root
     *
     * @return void
     */
    send_come_from_idle_mode_request_android: function(channelId = null)
    {

        // keep presence active when returning to a call channel

        if (channelId != null)
        {
            android_host__mark_call_accept_presence_grace();
        }

        if (channelId == null)
        {
            channelId = 0; // use the root channel
        }

        // restore media before requesting channel entry
        android_host__exit_deep_idle();

        if (!android_host__is_ui_only_runtime())
        {
            client_msg.send_come_from_idle_mode_request(channelId);
        }
    },

    /**
     * @brief defers ordinary idle briefly after a native call accept
     *
     * @return void
     */
    mark_call_accept_presence_android: function()
    {
        android_host__mark_call_accept_presence_grace();
    },

    /**
     * @brief applies the native username and saves it for the next login
     *
     * @param string username -> new username. empty strings are ignored
     *
     * @return void
     */
    set_username_on_connect_android: function(username)
    {
        if (username.length == 0)
        {
            return;
        }

        // keep this name for the next login

        g_chosen_username = username;

        client_msg.send_change_client_username_request(username, g_local_client_id);

        let index = channel_tree__get_client_index_in_array_by_client_id(g_local_client_id);

        if (index != -1)
        {
            g_client_list[index].username = username;
        }

        g_local_username = username;
        {
            let rename_input = document.getElementById('connected-local-client-input');
            if (rename_input != null)
            {
                rename_input.setAttribute('value', g_local_username);
            }
        }
    },

    /**
     * @brief displays the connection phase reported by the native owner
     *
     * @param string state -> native connection phase
     * @param string reason -> explanation shown with the phase
     *
     * @return void
     */
    show_connection_phase_android: function(state, reason)
    {
        if (state === "runtime_failed")
        {
            connection__handle_runtime_failure(reason);
            return;
        }
        if (g_connection_status.runtime_failure_reason)
        {
            connection__render_connection_status();
            return;
        }
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
     * @brief requests missing local settings and reattaches the WebView
     *
     * @return void
     */
    request_loopback_reattach_android: function()
    {
        if (g_is_authenticated == true)
        {
            return;
        }

        // request settings when the local endpoint is missing
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
     * @brief validates and applies a native settings snapshot
     *
     * @param string json_current_settings -> serialized native settings snapshot
     *
     * @return void
     */
    accept_current_settings_from_android: function(json_current_settings)
    {

        let settings_from_android = JSON.parse(json_current_settings);
        if (settings_from_android == null || typeof settings_from_android !== "object" || Array.isArray(settings_from_android))
        {
            throw new Error("invalid Android settings");
        }
        if (typeof settings_from_android.identity_error === "string" && settings_from_android.identity_error)
        {
            node_identity_blocked = true;
            android_host__node_set_connection_wanted(false);
            connection__report_connection_status("error", "The saved identity needs recovery. Open Android settings to choose or import your identity.");
            return;
        }
        if (node_identity_blocked)
        {
            node_identity_blocked = false;
            if (!is_connection_ended_by_user)
            {
                g_node_connection_wanted = true;
            }
        }
        // validate credentials before changing runtime state
        const next_metadata_keys = settings_from_android.metadata_keys || [];
        const next_metadata_signature = android_host__metadata_keys_signature(next_metadata_keys);
        if (android_host__is_ui_only_runtime() && Number.isSafeInteger(settings_from_android.identity_revision) && settings_from_android.identity_revision >= g_identity_revision)
        {
            g_identity_revision = settings_from_android.identity_revision;
        }

        // apply the selected theme

        g_android_app_mode = (typeof settings_from_android.app_mode === "string") ? settings_from_android.app_mode : "";
        console.log("android settings received, app_mode = " + g_android_app_mode);

        main__apply_theme_for_app_mode();

        g_are_server_details_predefined = true;
        is_autoconnect_enabled = true;

        // enable autoconnect and sound unless explicitly disabled

        g_is_autoconnect_without_user_action_active = (settings_from_android.is_autoconnect_enabled != false);

        // copy the native logging preference
        g_is_file_logging_enabled = (settings_from_android.is_file_logging_enabled != false);

        // keep the local microphone preference when one is saved
        let has_local_sound_choice = false;
        has_local_sound_choice = (utils__storage_get("lemon_sound_effects") != null);

        if (has_local_sound_choice == false)
        {
            g_are_sound_effects_enabled = (settings_from_android.is_audio_effect_enabled == true);
        }

        // apply microphone mode changes to an existing connection

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

        android_host__apply_settings_presentation(settings_from_android);

        // save previous connection values for comparison

        let previous_autoconnect_host = g_autoconnect_details.host;
        let previous_autoconnect_port = g_autoconnect_details.port;
        let previous_autoconnect_keys = android_host__metadata_keys_signature(g_autoconnect_details.keys || []);
        // track local endpoint changes

        let previous_loopback = g_loopback_port + ":" + g_loopback_token;

        g_autoconnect_details.host = settings_from_android.host;
        g_autoconnect_details.port = settings_from_android.port;

        // replace the stored metadata keys
        g_autoconnect_details.keys.length = 0;
        g_autoconnect_details.keys = [];

        // copy metadata keys when provided

        if (settings_from_android.metadata_keys != null)
        {
            for (i = 0; i < settings_from_android.metadata_keys.length; i++)
            {
                g_autoconnect_details.keys.push(settings_from_android.metadata_keys[i]);
            }
        }

        // read the local endpoint supplied to the WebView
        g_loopback_port = (typeof settings_from_android.loopback_port === "number") ? settings_from_android.loopback_port : 0;

        g_loopback_token = (typeof settings_from_android.loopback_token === "string") ? settings_from_android.loopback_token : "";

        // tell the data worker whether Node owns encryption

        if (g_data_processing_worker != null)
        {
            g_data_processing_worker.postMessage({ type: "mainthread__set_loopback_mode", value: g_loopback_port });
        }

        // close the current socket when connection settings change
        let are_server_details_changed = (g_autoconnect_details.host != previous_autoconnect_host) || (g_autoconnect_details.port != previous_autoconnect_port) || (next_metadata_signature !== previous_autoconnect_keys) || ((g_loopback_port + ":" + g_loopback_token) != previous_loopback);

        if (are_server_details_changed)
        {
            connection__invalidate_pending_attempts();
            console.log("connect-path: server details changed, dropping the live connection");
            if (g_websocket_worker != null)
            {
                g_websocket_worker.postMessage({ type: "close" });
            }
        }

        // keep the default username for login

        if (typeof settings_from_android.default_username === "string" && settings_from_android.default_username.length > 0)
        {
            g_chosen_username = settings_from_android.default_username;
        }

        // only the connection owner generates the identity

        if (android_host__is_ui_only_runtime() == false)
        {
            connection__request_identity(settings_from_android.identity_string, settings_from_android.rsa_key_bits, settings_from_android.identity_revision);
        }

        if (!is_connection_ended_by_user)
        {
            connection__request_connect("settings");
        }
    }
}

/**
 * @brief defers idle entry while waiting for the server to confirm a return
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

    // clear the pending request if the server does not answer
    come_from_idle_in_flight_timer = setTimeout(function()
    {
        come_from_idle_in_flight_timer = null;
        is_come_from_idle_in_flight = false;
        console.log("connect-path: come-from-idle was never confirmed, idle allowed again");
        android_host__apply_pending_deep_idle_if_any();
    }, 20000);
}

/**
 * @brief clears the pending return and applies any deferred idle request
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
 * @brief applies an idle request deferred during a return from idle
 *
 * @return void
 */
function android_host__apply_pending_deep_idle_if_any()
{
    if (g_is_deep_idle_pending == true && g_is_deep_idle == false)
    {
        const forced = pending_deep_idle_forced;
        g_is_deep_idle_pending = false;
        pending_deep_idle_forced = false;
        console.log("connect-path: running deferred idle entry");
        android_host__enter_deep_idle(forced);
    }
}

/**
 * @brief keeps presence active briefly after accepting a call
 *
 * @return void
 */
function android_host__mark_call_accept_presence_grace()
{
    presence_grace_until_timestamp = new Date().valueOf() + CALL_ACCEPT_PRESENCE_GRACE_MS;
    console.log("connect-path: call accepted, holding presence for " + CALL_ACCEPT_PRESENCE_GRACE_MS + "ms");
}

/**
 * @brief schedules an idle decision after call acceptance grace ends
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
 * @brief cancels the delayed idle decision
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
 * @brief enters idle with a slower heartbeat and suspended media
 *
 * @param boolean is_forced -> true to idle from any channel. false preserves an active channel
 *
 * @return void
 */
function android_host__enter_deep_idle(is_forced)
{
    // ignore duplicate idle entry

    if (g_is_deep_idle == true)
    {
        console.log("connect-path: idle requested, already idle");
        return;
    }

    // retain idle requests until the pending return completes

    if (is_come_from_idle_in_flight == true)
    {
        console.log("connect-path: idle deferred, a come-from-idle is still unconfirmed");
        g_is_deep_idle_pending = true;
        pending_deep_idle_forced = pending_deep_idle_forced || is_forced === true;
        return;
    }

    // retain idle requests received before login

    if (g_is_authenticated == false)
    {
        console.log("connect-path: idle requested while not authenticated, deferred to login");
        g_is_deep_idle_pending = true;
        pending_deep_idle_forced = pending_deep_idle_forced || is_forced === true;
        return;
    }

    // defer ordinary idle during call acceptance

    if (is_forced != true && new Date().valueOf() < presence_grace_until_timestamp)
    {
        console.log("connect-path: idle deferred, inside the call-accept presence grace");
        android_host__schedule_presence_grace_recheck();
        return;
    }

    // keep ordinary background sessions in their current channel

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
    audio_transport__reset();

    if (!android_host__is_ui_only_runtime())
    {
        client_msg.send_go_to_idle_mode_request();
    }

    // use a 120 second heartbeat and a 360 second idle timeout
    g_connection_check.interval_ms = 120 * 1000;
    g_connection_check.lost_threshold_ms = 360 * 1000;

    if (g_audio_context != null && g_audio_context.state === "running")
    {
        audio_suspend_promise = g_audio_context.suspend();
    }

    // stop microphone capture while idle

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

    // close the data channel to stop its background checks

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
        g_video_datachannel = null;
        g_is_video_datachannel_connected = false;
        g_is_webrtc_datachannel_connected = false;
    }
}

/**
 * @brief restores heartbeat timing, audio and the data channel after idle
 *
 * @return void
 */
function android_host__exit_deep_idle()
{
    // cancel any delayed idle decision before returning

    android_host__cancel_presence_grace_recheck();

    g_is_deep_idle_pending = false;
    pending_deep_idle_forced = false;

    if (g_is_deep_idle == false)
    {
        console.log("connect-path: leave-idle requested, was not idle");
        return;
    }

    // track the server's pending return from idle

    if (!android_host__is_ui_only_runtime())
    {
        android_host__mark_come_from_idle_in_flight();
    }

    console.log("connect-path: leaving idle");

    g_is_deep_idle = false;

    g_connection_check.interval_ms = 10 * 1000;
    g_connection_check.lost_threshold_ms = 35 * 1000; // matches the startup timeout
    g_connection_check.last_response_timestamp = new Date().valueOf();

    // wake the heartbeat loop immediately
    if (g_connection_check.sleep_resolve != null)
    {
        g_connection_check.sleep_resolve();
        g_connection_check.sleep_resolve = null;
    }

    if (g_audio_context != null)
    {
        // wait for audio suspension to finish
        let suspend_settled = (audio_suspend_promise != null) ? audio_suspend_promise : Promise.resolve();
        audio_suspend_promise = null;

        suspend_settled.catch(function()
        {
        }).then(function()
        {
            // rebuild the Android audio graph after idle

            if (g_android_app_mode != "")
            {
                audio__rebuild_audio_graph_after_idle();
                return;
            }

            // rebuild the browser audio graph if resume fails
            g_audio_context.resume().catch(function(e)
            {
                console.log("audio resume rejected: " + e.message);
            });
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

    // reconnect the data channel closed during idle
    if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_webrtc_datachannel_check_running == false)
    {
        g_is_webrtc_datachannel_check_running = true;
        voice__webrtc_datachannel_connection_check(true);
    }
}
