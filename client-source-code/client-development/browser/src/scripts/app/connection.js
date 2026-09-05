// connection.js is embedded in template.html along with the other client files, and in the node bundle
// it is the connecting side: the driver that decides when to dial (who am i, which identity, where to,
// one attempt in flight), the dial itself, the heartbeat loop, fast reconnect, the connect-page hold
// and the saved server bookmarks
// ui.js and android-host.js nudge the driver through connection__request_connect; dispatch.js reports what the socket did

// state private to this file
var port = 0;

var FAST_RECONNECT_DEADLINE_MS = 12000;

var VERIFICATION_MESSAGE = "welcome";

// where to connect, as { kind: "loopback", port, token } or { kind: "server", host, port, wss_port }
var target_slot = connection__make_slot();

// the identity slot is declared in globals.js (dispatch.js fills it); the slot itself is built here, where its type lives
g_identity_slot = connection__make_slot();

// one keygen per passphrase, because a repeat request for the same identity must be a no-op.
// the size is part of the key: the same passphrase at a new size is a different identity,
// so it has to be part of what makes a request a repeat
var identity_requested_for = null;
var identity_requested_bits = 0;

// resolved by the websocket close and error handlers. the driver awaits this while a
// socket exists, whether it is mid-handshake or long connected
var connection_closed_resolvers = [];

// skips the retry countdown, used when the connect button is pressed or new details arrive.
// a nudge that lands while the driver is still watching the old socket is kept, not lost:
// an identity switch closes the socket and nudges in the same breath, before the close is seen
var driver_nudge_resolver = null;
var is_driver_nudge_pending = false;

var is_connection_driver_running = false;

// webview only: true when connect was pressed before node announced its loopback port
var ui_connect_requested = false;

// ---- connection driver ----

/**
 * @brief a slot holds one async value: set() stores it and wakes everyone who called wait()
 *
 * @return object the slot, with set(new_value) and wait(), which returns a promise of the value
 */
function connection__make_slot()
{
    let slot = {
        value: null,
        is_set: false,
        waiters: []
    };

    slot.set = function(new_value)
    {
        slot.value = new_value;
        slot.is_set = true;

        let woken = slot.waiters;
        slot.waiters = [];

        for (let i = 0; i < woken.length; i++)
        {
            woken[i](new_value);
        }
    };

    slot.wait = function()
    {
        if (slot.is_set)
        {
            return Promise.resolve(slot.value);
        }

        return new Promise(function(resolve) { slot.waiters.push(resolve); });
    };

    return slot;
}

/**
 * @brief asks the data-processing worker for the identity keypair: derived from the passphrase when one is given (199 characters or more), random otherwise
 *        the same request twice is ignored; without a worker yet the request is dropped, and the
 *        next settings push retries it
 *
 * @param string|null passphrase_or_null -> the passphrase, null or anything short for a random key
 *
 * @return void
 */
function connection__request_identity(passphrase_or_null)
{
    let requested_key = (typeof passphrase_or_null === "string" && passphrase_or_null.length >= 199)
        ? passphrase_or_null : "(random)";

    if (identity_requested_for === requested_key && identity_requested_bits === g_rsa_key_bits)
    {
        return;
    }

    // settings can arrive before the workers exist; the next settings push retries this
    if (g_data_processing_worker == null)
    {
        console.log("connect-path: no worker yet, keypair request dropped");
        return;
    }

    identity_requested_for = requested_key;
    identity_requested_bits = g_rsa_key_bits;
    g_identity_slot.is_set = false;
    g_is_rsa_key_generated = false;

    console.log("connect-path: keypair requested (" + (requested_key === "(random)" ? "random" : "from passphrase") + ", " + g_rsa_key_bits + " bits)");

    g_data_processing_worker.postMessage({
        type: "mainthread__generate_rsa_keypair",
        from_identity_string: requested_key !== "(random)",
        identity_passphrase_string: requested_key === "(random)" ? null : requested_key,
        rsa_key_bits: g_rsa_key_bits
    });

    // the derive can take minutes on a phone, so the status says what the wait actually is
    connection__report_connection_status("connecting", "generating the identity key (takes a while on first start)");
}

/**
 * @brief wakes everyone awaiting connection__connection_closed(): the socket is gone
 *
 * @return void
 */
function connection__signal_connection_closed()
{
    let woken = connection_closed_resolvers;
    connection_closed_resolvers = [];

    for (let i = 0; i < woken.length; i++)
    {
        woken[i]();
    }
}

/**
 * @brief a promise for the next connection close; the driver awaits it between attempts
 *
 * @return promise resolves when the connection closes
 */
function connection__connection_closed()
{
    return new Promise(function(resolve) { connection_closed_resolvers.push(resolve); });
}

/**
 * @brief wakes the driver out of its retry wait, or remembers the nudge for the wait that has not started yet
 *
 * @return void
 */
function connection__nudge_connection_driver()
{
    if (driver_nudge_resolver != null)
    {
        let resolver = driver_nudge_resolver;
        driver_nudge_resolver = null;
        resolver();
        return;
    }

    is_driver_nudge_pending = true;
}

/**
 * @brief the countdown between retries: shows the failure text every second and leaves early on a nudge
 *
 * @param number seconds -> how long to wait
 *
 * @return promise resolves when the countdown ended or a nudge cut it short
 */
async function connection__driver_retry_wait(seconds)
{
    if (is_driver_nudge_pending == true)
    {
        is_driver_nudge_pending = false;
        console.log("connect-path: retry wait skipped by a pending nudge");
        return;
    }

    let nudged = new Promise(function(resolve) { driver_nudge_resolver = resolve; });

    for (let remaining = seconds; remaining > 0; remaining--)
    {
        if (g_last_connect_attempt_failed == true)
        {
            let status_element = document.getElementById("another-buttons-loading-container-p");

            if (status_element != null)
            {
                status_element.innerHTML = "connection failed<br>retrying in " + remaining + "s";
            }
        }

        let done = await Promise.race([utils__sleep(1000).then(function() { return false; }), nudged.then(function() { return true; })]);

        if (done == true)
        {
            console.log("connect-path: retry wait skipped by nudge");
            break;
        }
    }

    driver_nudge_resolver = null;
}

/**
 * @brief the connection driver, the one loop that dials
 *        waits for a target and an identity, attempts once, awaits the close, then waits out a
 *        countdown and tries again; runs once per page and never returns
 *
 * @return promise never resolves
 */
async function connection__connection_driver()
{
    if (is_connection_driver_running == true)
    {
        return;
    }

    is_connection_driver_running = true;
    console.log("connect-path: driver started");

    while (true)
    {
        let target = await target_slot.wait();

        // the node host parks the connection during the idle handover
        if (g_node_connection_wanted == false)
        {
            await utils__sleep(1000);
            continue;
        }

        let identity = null;

        if (target.kind === "server")
        {
            console.log("connect-path: target " + target.host + ":" + target.port + ", waiting for keypair");
            identity = await g_identity_slot.wait();
        }
        else
        {
            console.log("connect-path: target loopback :" + target.port);
        }

        let closed = connection__connection_closed();

        g_last_disconnect_reason = "";
        connection__report_connection_status("connecting");

        // a dial consumes any nudge; one kept from before must not skip a later countdown
        is_driver_nudge_pending = false;
        await connection__attempt_connection(target, identity);

        // wait for the socket to die. a dial that could not even create a socket never
        // fires close, so an unauthenticated wait also has a deadline
        while (true)
        {
            let outcome = await Promise.race([
                closed.then(function() { return "closed"; }),
                utils__sleep(45000).then(function() { return "deadline"; })
            ]);

            if (outcome === "closed")
            {
                break;
            }

            if (g_is_authenticated == false)
            {
                // the loopback socket is fine, node just has not logged in yet
                // redialing now would kill a healthy attach, so keep waiting
                if (target.kind === "loopback")
                {
                    console.log("connect-path: loopback attached, still waiting for node's login");
                    continue;
                }

                console.log("connect-path: no login within 45s, treating the attempt as failed");
                break;
            }
        }

        // a fast reconnect re-dials at once with the same identity: no countdown, no button
        if (g_is_authenticated == false && g_fast_reconnect.in_progress == true)
        {
            continue;
        }

        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            // manual mode means one attempt per button press: state the failure and
            // hold until the connect button nudges, with no countdown
            if (is_driver_nudge_pending == true)
            {
                is_driver_nudge_pending = false;
                console.log("connect-path: dialing on the pending nudge");
                continue;
            }

            connection__set_connect_button_pending(false);
            connection__report_connection_status("idle",
                (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
            await new Promise(function(resolve) { driver_nudge_resolver = resolve; });
            continue;
        }

        // dialing node on this device is free, so the loopback redials fast; the 30s
        // pace is for a remote server that may be down
        let retry_seconds = (target.kind === "loopback") ? 2 : 30;

        if (g_is_authenticated == false)
        {
            g_connection_status.next_retry_at = new Date().valueOf() + retry_seconds * 1000;
            connection__report_connection_status("waiting_retry",
                (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
        }

        await connection__driver_retry_wait(retry_seconds);
    }
}

/**
 * @brief the one place that may start a connection, and the whole dial policy
 *        "button" and "attach" always dial, "settings", "resume" and "retry" only under
 *        autoconnect, and "resume" also reattaches when node already holds a session
 *
 * @param string trigger -> "button", "attach", "settings", "resume" or "retry"
 *
 * @return void
 */
function connection__request_connect(trigger)
{
    let is_wanted =
        (trigger === "button")
        || (trigger === "attach")
        || (g_is_autoconnect_without_user_action_active == true)
        || (ui_connect_requested == true)
        || (trigger === "resume" && g_connection_status.state === "connected");

    console.log("connect-path: connection__request_connect(" + trigger + ") -> " + (is_wanted ? "dial" : "ignored"));

    if (is_wanted == false)
    {
        return;
    }

    // derive the target from where we run and what the page knows
    if (android_host__is_ui_only_runtime())
    {
        if (g_loopback_port <= 0)
        {
            // node has not announced its port yet; the settings push that follows finishes this
            if (trigger === "button")
            {
                ui_connect_requested = true;
                document.getElementById("another-buttons-loading-container-p").innerHTML = "waiting for app runtime...";
            }
            return;
        }

        ui_connect_requested = false;
        target_slot.set({ kind: "loopback", port: g_loopback_port, token: g_loopback_token });

        // a button press keeps the page exactly as it is, only the button itself
        // fades; every other trigger connects behind the spinner page
        if (trigger !== "button")
        {
            connection__hold_back_connect_page();
        }
    }
    else if (g_are_server_details_predefined == true)
    {
        // a partial first-run json has no host yet, and that must never become a target
        if (typeof g_autoconnect_details.host !== "string" || g_autoconnect_details.host.length == 0
            || (parseInt(g_autoconnect_details.port) > 0) == false)
        {
            console.log("connect-path: no server details yet, waiting");
            return;
        }

        target_slot.set({
            kind: "server",
            host: g_autoconnect_details.host,
            port: g_autoconnect_details.port,
            wss_port: g_autoconnect_details.wss_port
        });
    }
    else
    {
        target_slot.set({
            kind: "server",
            host: document.getElementById("input-ip-address").value,
            port: document.getElementById("input-port-number").value
        });
    }

    connection__nudge_connection_driver();
}

/**
 * @brief the connect button; it keeps its own name for the onclick wiring
 *
 * @return void
 */
function connection__submit_connection_target_from_ui()
{
    connection__request_connect("button");
}

/**
 * @brief turns a node socket error code into something a person can act on
 *        the codes are the difference between your wifi being off and the server rejecting you;
 *        without them an unreachable network used to be reported as a login rejection
 *
 * @param string error_code -> the node error code, "ECONNREFUSED" and the like
 *
 * @return string the text for the connect page
 */
function connection__describe_socket_error(error_code)
{
    if (g_device_has_network === false)
    {
        return "no network connection (wifi and mobile data are off)";
    }

    if (error_code === "ENETUNREACH" || error_code === "EHOSTUNREACH"
        || error_code === "EAI_AGAIN" || error_code === "ENOTFOUND")
    {
        return "no route to the server (network down, or the address is unreachable)";
    }

    if (error_code === "ECONNREFUSED")
    {
        return "the server refused the connection (is it running on that port?)";
    }

    if (error_code === "ETIMEDOUT")
    {
        return "the server did not answer in time (wrong address, or a firewall)";
    }

    if (typeof navigator !== "undefined" && navigator.onLine === false)
    {
        return "no network connection (wifi off?)";
    }

    return "could not reach the server (wrong address, server down, or no network)";
}

/**
 * @brief records a connection status change in g_connection_status, tells the listeners and repaints the connect page
 *
 * @param string state -> the new state
 * @param string reason -> the text that explains it, "" for none
 *
 * @return void
 */
function connection__report_connection_status(state, reason)
{
    g_connection_status.state = state;
    g_connection_status.reason = (reason != null) ? reason : "";

    for (let i = 0; i < g_connection_status_listeners.length; i++)
    {
        try { g_connection_status_listeners[i](g_connection_status); } catch (e) { }
    }

    connection__render_connection_status();
}

/**
 * @brief paints the login page extras from g_connection_status
 *        the ticker recalls it every second so the countdown and the "ago" times stay live; safe
 *        headless, because the shim absorbs the dom
 *
 * @return void
 */
function connection__render_connection_status()
{
    let reason_element = document.getElementById("connection-status-reason");
    let countdown_element = document.getElementById("connection-status-countdown");
    let lastseen_element = document.getElementById("connection-status-lastseen");

    if (reason_element == null || countdown_element == null || lastseen_element == null)
    {
        return;
    }

    let reason_text = g_connection_status.reason;

    // the page knows the device's connectivity; node cannot see it
    if (typeof navigator !== "undefined" && navigator.onLine === false)
    {
        reason_text = "no network connection (wifi off?)";
    }

    reason_element.textContent = (g_connection_status.state === "connected") ? "" : reason_text;

    let countdown_text = "";

    // only the retry countdown belongs here, because the loading line above already
    // says "connecting to <host>" and printing it twice read as two separate states
    if (g_connection_status.state === "waiting_retry" && g_connection_status.next_retry_at > 0)
    {
        let seconds_left = Math.max(0, Math.ceil((g_connection_status.next_retry_at - new Date().valueOf()) / 1000));
        countdown_text = "next attempt in " + seconds_left + "s";
    }

    countdown_element.textContent = countdown_text;

    let lastseen_text = "";

    if (g_connection_status.state !== "connected" && g_connection_status.last_connected_at > 0)
    {
        let minutes_ago = Math.round((new Date().valueOf() - g_connection_status.last_connected_at) / 60000);
        lastseen_text = (minutes_ago < 1) ? "last connected under a minute ago"
            : "last connected " + minutes_ago + " min ago";
    }

    lastseen_element.textContent = lastseen_text;
}

/**
 * @brief one repaint per second keeps the countdown and the "ago" times moving
 *
 * @return void
 */
function connection__start_connection_status_ticker()
{
    window.setInterval(connection__render_connection_status, 1000);
}

// ---- dial and heartbeat ----

/**
 * @brief the heartbeat loop's sleep; android_host__exit_deep_idle resolves it early so the first check after idle goes out at once
 *
 * @param number ms -> the sleep in milliseconds
 *
 * @return promise resolves after the sleep or on the early wake
 */
function connection__connection_check_sleep(ms)
{
    return new Promise(function(resolve)
    {
        let timer_id = setTimeout(function()
        {
            g_connection_check.sleep_resolve = null;
            resolve();
        }, ms);

        // early-wake hook used by android_host__exit_deep_idle: cancel this sleep's timer so a stale
        // (long) timeout can never fire later and clobber a newer sleep's resolver
        g_connection_check.sleep_resolve = function()
        {
            clearTimeout(timer_id);
            g_connection_check.sleep_resolve = null;
            resolve();
        };
    });
}

/**
 * @brief the heartbeat loop: sends client_connection_check on an interval, and when no reply arrives within the lost threshold, alerts the user and resets the app (identity kept)
 *        one throw in here must not kill the loop, since it carries both the heartbeat and the loss detector
 *
 * @return promise resolves when the loop is told to stop
 */
async function connection__websocket_connection_check()
{
    while (g_should_connection_check_be_running == true)
    {
        // one throw in here must not kill the loop: it carries BOTH the heartbeat and
        // the loss detector, and a dead loop is an unnoticed dead connection
        try
        {
            let timestamp_now = new Date().valueOf();

            if (g_connection_check.last_response_timestamp == 0)
            {
                g_connection_check.last_response_timestamp = timestamp_now;
            }

            if ((g_connection_check.last_response_timestamp + g_connection_check.lost_threshold_ms) < timestamp_now)
            {
                let silence_ms = timestamp_now - g_connection_check.last_response_timestamp;
                console.error("loss detector: no heartbeat response for " + silence_ms + "ms");

                // the resume path ends this loop; the login that follows starts a fresh one
                if (connection__try_start_fast_reconnect("no heartbeat reply for " + silence_ms + " ms") == true)
                {
                    break;
                }

                utils__custom_alert("connection with server lost");
                console.error("loss detector: resetting");
                connection__reset_chat_app_keep_identity();
            }

            g_session.ping_sent_at = timestamp_now; // the response handler turns this into the round-trip time

            let message_object = {
                message: {
                    type: "client_connection_check",
                }
            };

            connection__send_message_object(message_object);
        }
        catch (checker_error)
        {
            console.error("connection check iteration failed: " + (checker_error != null && checker_error.stack ? checker_error.stack : checker_error));
        }

        await connection__connection_check_sleep(g_connection_check.interval_ms);
    }
}

/**
 * @brief fast reconnect: the socket is gone but the page keeps everything (lists, chat, channel, keys) and the driver re-dials at once with the same identity, asking the server to adopt the session
 *
 * @param string reason -> why the socket went, for the log
 *
 * @return boolean false when a resume is not possible, so the caller runs the classic wipe instead
 */
function connection__try_start_fast_reconnect(reason)
{
    // node owns the real connection in the webview, and only a logged-in session can be resumed
    if (g_server_policy.is_fast_reconnect_allowed == false || g_is_authenticated == false
        || g_is_identity_switch_in_progress == true || android_host__is_ui_only_runtime() == true)
    {
        return false;
    }

    // one attempt per loss: when the resume socket itself dies the caller takes the classic path
    if (g_fast_reconnect.in_progress == true)
    {
        return false;
    }

    g_fast_reconnect.in_progress = true;
    g_fast_reconnect.resumed = false;
    g_fast_reconnect.pending_lists = null;

    // the driver keys off this flag; the page itself is left exactly as it is
    g_is_authenticated = false;
    g_should_connection_check_be_running = false;
    g_connection_check.last_response_timestamp = 0;
    channel_tree__stop_avatar_prefetch();

    // the worker goes back to handshake mode; it keeps the identity and the channel keys
    g_data_processing_worker.postMessage({ type: "mainthread_reset_data" });

    console.log("%cfast reconnect: " + reason + " - re-dialing with the same identity", "color: #ffa500; font-weight: bold; font-size: 14px;");
    utils__custom_log("fast reconnect attempt (" + reason + ")");

    g_fast_reconnect.deadline_timer = setTimeout(function()
    {
        connection__fast_reconnect_failed("no adopted session within " + (FAST_RECONNECT_DEADLINE_MS / 1000) + " s");
    }, FAST_RECONNECT_DEADLINE_MS);

    // wakes the driver out of its wait on the old socket; it re-dials instead of parking
    connection__signal_connection_closed();
    return true;
}

/**
 * @brief the one failure of a fast reconnect: the classic "connection lost" toast and the full wipe
 *
 * @param string reason -> why it failed, for the log
 *
 * @return void
 */
function connection__fast_reconnect_failed(reason)
{
    if (g_fast_reconnect.in_progress == false && g_fast_reconnect.resumed == false)
    {
        return;
    }

    g_fast_reconnect.in_progress = false;
    g_fast_reconnect.resumed = false;
    g_fast_reconnect.pending_lists = null;
    clearTimeout(g_fast_reconnect.deadline_timer);
    g_fast_reconnect.deadline_timer = null;

    console.log("%cfast reconnect failed: " + reason + " - back to the connect screen", "color: #ff4040; font-weight: bold; font-size: 14px;");
    utils__custom_log("fast reconnect failed (" + reason + ")");

    if (g_is_running_in_android_webview == false)
    {
        utils__custom_alert("connection with server lost");
    }

    g_is_authenticated = false;
    connection__reset_chat_app_keep_identity();
}

/**
 * @brief the server adopted the session: everything on this page is still valid
 *        the complete lists follow as on a login and are held back until the last one, see
 *        connection__fast_reconnect_buffer_list
 *
 * @return void
 */
function connection__fast_reconnect_succeeded()
{
    g_fast_reconnect.resumed = true;
    g_fast_reconnect.pending_lists = {};
    console.log("fast reconnect: the server adopted the session, waiting for the refreshed lists");
}

/**
 * @brief holds back one of the four lists (channel, client, icon, tag) a resume refreshes, so they are applied together in one repaint and nothing on screen flickers
 *
 * @param string kind -> "channel", "client", "icon" or "tag"
 * @param object value -> the list message
 *
 * @return boolean true when the list was buffered, false when no resume is in progress
 */
function connection__fast_reconnect_buffer_list(kind, value)
{
    if (g_fast_reconnect.resumed == false)
    {
        return false;
    }

    if (g_fast_reconnect.pending_lists == null)
    {
        g_fast_reconnect.pending_lists = {};
    }

    g_fast_reconnect.pending_lists[kind] = value;

    if (g_fast_reconnect.pending_lists.channel != null && g_fast_reconnect.pending_lists.client != null
        && g_fast_reconnect.pending_lists.icon != null && g_fast_reconnect.pending_lists.tag != null)
    {
        connection__fast_reconnect_apply_lists();
    }

    return true;
}

/**
 * @brief applies the four refreshed lists in one task: the old lists go in the same task the new ones are built, one repaint, already complete
 *
 * @return void
 */
function connection__fast_reconnect_apply_lists()
{
    let lists = g_fast_reconnect.pending_lists;

    g_fast_reconnect.pending_lists = null;
    g_fast_reconnect.resumed = false;
    clearTimeout(g_fast_reconnect.deadline_timer);
    g_fast_reconnect.deadline_timer = null;

    // the old lists go in the same task the new ones are built: one repaint, already complete
    g_map_client_id_to_array_index.clear();
    g_client_list.length = 0;
    g_channel_list.length = 0;
    g_tags.length = 0;
    g_icons.length = 0;
    g_is_client_list_retrieved = false;
    g_is_channel_list_retrieved = false;

    let elements_count = document.getElementsByClassName("connected-client").length;
    for (let i = 0; i < elements_count; i++)
    {
        document.getElementsByClassName("connected-client")[0].remove();
    }

    server_msg.process_channel_list_from_server(lists.channel);
    server_msg.process_client_list_from_server(lists.client);
    server_msg.process_icon_list_from_server(lists.icon);
    server_msg.process_tag_list_from_server(lists.tag);

    console.log("%cFAST RECONNECT HAPPENED - session resumed, lists refreshed, nothing was lost", "color: #00e000; font-weight: bold; font-size: 20px;");
    utils__custom_log("fast reconnect happened");
}

/**
 * @brief tears the session down to the connect screen while keeping the rsa identity
 *        closes the socket, clears the client and channel DOM and resets nearly all
 *        connection, chat and voice state
 *
 * @return void
 */
function connection__reset_chat_app_keep_identity()
{
    // no connection, nobody is typing - drop the notices and the ticker with them
    g_typing.state = {};
    chat__render_typing_indicator();

    // whatever triggered the reset, the socket must really be gone: a reconnect stalls for as long
    // as an old authenticated socket lives on. closing a closed socket is a no-op, and the onclose
    // it fires re-enters this reset harmlessly (g_is_authenticated is already false)
    g_websocket_worker.postMessage({ type: "close" });

    UI.clear_chat_button_onclick();
    UI.clear_chat_button_onclick();

    // the list goes with the map. they used to be emptied 55 lines apart, and a frame
    // arriving in between found an empty map next to a full list
    g_map_client_id_to_array_index.clear();
    g_client_list.length = 0;

    // the session is over, so the stored login frame is a lie. the loopback replays it
    // to any ui that attaches, which showed "connected" and played the sound on every
    // reconnect attempt against a server that was not there
    g_node_cached_auth_frame = null;

    document.getElementById("another-buttons-sub-container").style.display = "";
    document.getElementById("another-buttons-sub-loading-container").style.display = "none";
    document.getElementById("another-buttons-loading-container-p").innerHTML = "loading...";
    connection__set_connect_button_pending(false);
    g_is_microphone_available = false;

    document.getElementById('verification-system').style.display = "block";
    document.getElementById('communication-system-container').style.display = "none";

    // after the page switch, otherwise the mic is repainted while the old screen is still up
    voice__update_microphone_button();

    // back on the connect screen: the in-flow "server settings" button owns this
    // state, a second gear in the bar would only confuse - it returns on connect
    if (g_is_running_in_android_webview)
    {
        document.getElementById("android-settings-button").style.display = "none";
    }

    let elements_count = document.getElementsByClassName("connected-client").length;
    for (let i = 0; i < elements_count; i++)
    {
        document.getElementsByClassName("connected-client")[0].remove();
    }

    g_data_processing_worker.postMessage({
        type: "mainthread_reset_data"
    });

    g_base64_picture_string_to_send = "" ;
    g_is_websocket_connected = false;
    g_local_username = "";
    g_local_client_id = 0;
    g_alert_push_to_talk_key_shown_once = false;
    g_alert_streaming_music_shown_once = false;
    g_stop_song_stream_message_received = false;
    g_selected_server_chat_message_id = null;
    g_is_authenticated = false;
    g_should_connection_check_be_running = false;

    // the avatar prefetch timer must not outlive the session - its client ids are meaningless
    // after a reconnect (they are slot indices), and it would keep asking a dead socket
    channel_tree__stop_avatar_prefetch();

    // if the connection died while backgrounded-idle, re-enter idle automatically after reconnect
    g_is_deep_idle_pending = (g_is_deep_idle == true) || g_is_deep_idle_pending;
    g_is_deep_idle = false;
    g_connection_check.interval_ms = 10 * 1000;
    g_connection_check.lost_threshold_ms = 35 * 1000; // keep in sync with the startup value
    g_current_channel_id = 0;
    g_current_chat_context_id = "chat-context-channel-0";
    g_chat_message_receiver_type = "channel";
    g_chat_message_receiver_id = "main";
    g_is_client_list_retrieved = false;
    g_is_channel_list_retrieved = false;
    g_is_webrtc_datachannel_connected = false;

    // emptied in place, not reassigned: a fresh array would leave anything still
    // holding the old one reading stale clients. the client list is already cleared above
    g_icons.length = 0;
    g_tags.length = 0;
    g_channel_list.length = 0;

    g_offline_client_list = [];

    g_is_chat_hidden = false;
    layout__layout_apply(); // re-sync the grid (panel visibility) with the reset flag; no-op on touch
    g_local_message_id = 0;
    g_selected_font = "custom-font-usage-default";
    g_selected_font_color = "#ffffff";
    g_selected_font_size = 12;
    g_is_microphone_enabled_on_touch_device = false;  // for touch devices

    g_connection_check.last_response_timestamp = 0;

    g_selected_channel_id = null;
    g_selected_client_id = null;
    g_current_channel_keys = null;
    g_chat_context_array = [
        {
            type: "channel",
            chat_context_id: "chat-context-channel-0",
            last_known_message_sender_username: ""
        }
    ];

    // for voice chat

    g_peer_connection_with_server = null;

    g_iceconfig = null;

    g_is_voice_chat_allowed_by_server = false;
    g_is_client_microphone_allowed_by_server = false;
    g_is_microphone_enabled = false;
    g_is_microphone_active = false;
    g_last_sent_value_microphone_usage = false;

    if (g_local_audio_stream != null)
    {
        if (g_local_audio_stream.getTracks() != null)
        {
            g_local_audio_stream.getTracks()[0].enabled = false;
            // g_local_audio_stream = null; careful
        }
    }
}

/**
 * @brief the spinner that covers the held-back connect page, so the user never stares at a blank screen
 *        opaque, in the theme's color when one paints body/html; a dark fallback keeps page one solid otherwise
 *
 * @param boolean is_visible -> true to show it
 *
 * @return void
 */
function connection__set_connect_holdback_loader_visible(is_visible)
{
    let loader = document.getElementById("connect-holdback-loader");

    if (loader != null)
    {
        // opaque, in the theme's color when one paints body/html; some themes (bluebell)
        // paint only inner containers, then a dark fallback keeps page one solid
        if (is_visible == true && typeof getComputedStyle === "function")
        {
            let page_background = getComputedStyle(document.body).backgroundColor;

            if (page_background === "rgba(0, 0, 0, 0)" || page_background === "transparent")
            {
                page_background = getComputedStyle(document.documentElement).backgroundColor;
            }

            if (page_background === "rgba(0, 0, 0, 0)" || page_background === "transparent")
            {
                page_background = "#12151c";
            }

            loader.style.backgroundColor = page_background;
        }

        loader.style.display = (is_visible == true) ? "flex" : "none";
    }
}

/**
 * @brief hides the connect page behind the spinner while node is still connecting for the webview
 *        a deadline reveals the page anyway, because nothing arrives at all if node is wedged
 *
 * @return void
 */
function connection__hold_back_connect_page()
{
    if (g_loopback_port <= 0 || g_is_authenticated == true)
    {
        return;
    }

    if (g_is_holding_back_connect_page == false)
    {
        g_is_holding_back_connect_page = true;
        g_connect_holdback_started = new Date().valueOf();
        document.getElementById("verification-system").style.visibility = "hidden";
        connection__set_connect_holdback_loader_visible(true);

        setTimeout(connection__connect_holdback_check, 250);
    }

    // nothing arrives at all if node is wedged, so the page cannot stay blank forever
    g_connect_holdback_deadline = new Date().valueOf() + 2500;
}

/**
 * @brief node reporting "still working on it" keeps the spinner up a little longer, capped so a looping connect attempt can never hide the page forever
 *
 * @return void
 */
function connection__extend_connect_page_holdback()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    let now = new Date().valueOf();

    if (now - g_connect_holdback_started > 12000)
    {
        return;
    }

    g_connect_holdback_deadline = now + 4000;
}

/**
 * @brief the holdback timer: reveals the connect page once the deadline passed, else looks again in 250 ms
 *
 * @return void
 */
function connection__connect_holdback_check()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    if (new Date().valueOf() >= g_connect_holdback_deadline)
    {
        connection__reveal_connect_page();
        return;
    }

    setTimeout(connection__connect_holdback_check, 250);
}

/**
 * @brief shows the held-back connect page and drops the spinner
 *
 * @return void
 */
function connection__reveal_connect_page()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    g_is_holding_back_connect_page = false;
    document.getElementById("verification-system").style.visibility = "";
    connection__set_connect_holdback_loader_visible(false);
}

/**
 * @brief fades the connect button while an attempt runs; hiding rows caused a layout flicker
 *
 * @param boolean is_pending -> true while an attempt is in flight
 *
 * @return void
 */
function connection__set_connect_button_pending(is_pending)
{
    let button = document.getElementById("connect-button");

    if (button != null && button.classList != null)
    {
        button.classList.toggle("connect-attempt-pending", is_pending == true);
    }
}

/**
 * @brief one dial, driven only by connection__connection_driver(): builds the ws/wss connection string and has the websocket worker open the socket with the right opening frame
 *
 * @param object target -> the host, port and keys to dial
 * @param object identity -> the identity to present
 *
 * @return promise resolves when the attempt has ended, either way
 */
async function connection__attempt_connection(target, identity)
{
    if (android_host__is_ui_only_runtime())
    {
        // the page stays exactly as it is; only the button signals the running attempt
        connection__set_connect_button_pending(true);
    }
    else
    {
        document.getElementById("another-buttons-sub-container").style.display = "none";
        document.getElementById("another-buttons-sub-loading-container").style.display = "";
        document.getElementById("another-buttons-loading-container-p").innerHTML = "connecting to server...";
    }

    // a new attempt is in flight - the failure text only shows after this one resolves
    g_last_connect_attempt_failed = false;

    // loopback: connect to node on this device, token as the opening frame, no dh.
    // stun/turn still point at the real server, so keep the real host for webrtc
    if (target.kind === "loopback")
    {
        g_host = g_autoconnect_details.host;
        port = g_autoconnect_details.port;

        let loopback_connection_string = "ws://127.0.0.1:" + target.port + "/";
        console.log("connection_string -> " + loopback_connection_string + " (loopback)");

        // name the SERVER being joined, not the local hop to node - and never print an
        // unknown one ("connecting to :0" when the settings have not landed yet)
        document.getElementById("another-buttons-loading-container-p").innerHTML =
            (typeof g_host === "string" && g_host.length > 0)
                ? "connecting to " + chat__sanitize_string(g_host) + ":" + chat__sanitize_string("" + port)
                : "connecting...";

        g_websocket_worker.postMessage({
            type: "create_websocket_object",
            value: {
                connection_string: loopback_connection_string,
                onopen_data: target.token
            }
        });

        return;
    }

    g_host = target.host;
    port = target.port;

    keys__init_keys_object();

    while (g_keys_init_status == false)
    {
        await utils__sleep(100);
    }

    let protocol_part_of_connection_string = "ws://";
    let connection_port = port;

    if (document.location.protocol == "https:")
    {
        protocol_part_of_connection_string = "wss://";
        // an https page must use wss, on the separate stunnel wss port (not the plain ws port)
        if (target.wss_port)
        {
            connection_port = target.wss_port;
        }
    }

    let connection_string = protocol_part_of_connection_string + '' + g_host + ':' + connection_port + '/';
    console.log("connection_string -> " + connection_string);

    // say WHERE we are connecting: only now are the real host/port known
    document.getElementById("another-buttons-loading-container-p").innerHTML =
        "connecting to " + chat__sanitize_string(g_host) + ":" + chat__sanitize_string("" + connection_port)
        + "<br>using " + ((protocol_part_of_connection_string == "wss://") ? "secure websocket (wss)" : "websocket (ws)");

    let dh_public_mix = keys__get_public_mix().toString();

    let message_object = {
        message: {
            type: "public_key_info",
            value: identity.public_key_string,
            verification_string: VERIFICATION_MESSAGE,
            dh_public_mix: dh_public_mix
        }
    };

    let message_json_string = connection__process_message_before_sending(message_object);
    let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

    g_websocket_worker.postMessage({
        type: "create_websocket_object",
        value: {
            connection_string: connection_string,
            onopen_data: data
        }
    });
}

/**
 * @brief hands encrypted data to the websocket worker for sending; also counts the bytes into g_session.bytes_sent for the session-info card
 *
 * @param string data_to_send -> the frame to send
 *
 * @return void
 */
function connection__websocket_worker_send(data_to_send)
{
    if (data_to_send != null && data_to_send.length > 0) { g_session.bytes_sent += data_to_send.length; }

    g_websocket_worker.postMessage({
        type: "send",
        value: data_to_send
    });
}

/**
 * @brief builds, encrypts and ships one client -> server message, the process/encrypt/send triple that used to be pasted at every call site
 *
 * @param object message_object -> { message: { type, ... } }
 *
 * @return void
 */
function connection__send_message_object(message_object)
{
    let message_json_string = connection__process_message_before_sending(message_object);
    let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);
    connection__websocket_worker_send(data);
}

/**
 * @brief logs the outgoing message type and serialises the message
 *
 * @param object message_object -> { message: { type, ... } }
 *
 * @return string the message as json
 */
function connection__process_message_before_sending(message_object)
{
    let outgoing_type = message_object.message.type;

    if (outgoing_type != "ice_candidate" && outgoing_type != "sdp_answer"
        && outgoing_type != "client_connection_check" && outgoing_type != "create_new_webrtc_datachannel_connection")
    {
        utils__custom_log('[S] msg.message.type : ' + outgoing_type);
    }
    return JSON.stringify(message_object);
}

/**
 * @brief the saved server configurations, kept on this device only
 *
 * @return array the bookmarks as [{ name, host, port }], [] when none or unreadable
 */
function connection__read_server_bookmarks()
{
    try
    {
        let stored = JSON.parse(utils__storage_get("lemon_server_bookmarks"));
        return Array.isArray(stored) ? stored : [];
    }
    catch (e) { return []; }
}

/**
 * @brief persists the server bookmarks to localStorage
 *
 * @param array bookmarks -> the bookmarks as [{ name, host, port }]
 *
 * @return void
 */
function connection__write_server_bookmarks(bookmarks)
{
    utils__storage_set("lemon_server_bookmarks", JSON.stringify(bookmarks));
}

/**
 * @brief repaints the bookmark dropdown; the option value is the index into the stored array
 *
 * @return void
 */
function connection__render_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null) { return; }

    let bookmarks = connection__read_server_bookmarks();
    select_element.innerHTML = "";

    let placeholder = document.createElement("option");
    placeholder.value = "";
    placeholder.textContent = (bookmarks.length > 0) ? "saved servers" : "no saved servers";
    select_element.appendChild(placeholder);

    for (let x = 0; x < bookmarks.length; x++)
    {
        let option = document.createElement("option");
        option.value = "" + x;
        option.textContent = bookmarks[x].name + " (" + bookmarks[x].host + ":" + bookmarks[x].port + ")";
        select_element.appendChild(option);
    }
}

/**
 * @brief wires the bookmark dropdown and its buttons on the connect page: picking one loads it into the address and port fields, ready to connect
 *
 * @return void
 */
function connection__wire_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null) { return; }

    connection__render_server_bookmarks();

    // picking one loads it into the address and port fields, ready to connect
    select_element.onchange = function()
    {
        if (this.value === "") { return; }

        let bookmark = connection__read_server_bookmarks()[parseInt(this.value)];

        if (bookmark == null) { return; }

        document.getElementById("input-ip-address").value = bookmark.host;
        document.getElementById("input-port-number").value = bookmark.port;
        document.getElementById("server-bookmark-name").value = bookmark.name;
    };

    // saving under a name that already exists overwrites it, so this doubles as "update"
    document.getElementById("server-bookmark-save").onclick = function()
    {
        let name_element = document.getElementById("server-bookmark-name");
        let name = name_element.value.trim();
        let typed_host = document.getElementById("input-ip-address").value.trim();
        let typed_port = document.getElementById("input-port-number").value.trim();

        if (name.length === 0) { utils__custom_alert("give this server a name first"); return; }
        if (typed_host.length === 0 || typed_port.length === 0) { utils__custom_alert("fill in the address and port first"); return; }

        let bookmarks = connection__read_server_bookmarks();
        let existing = -1;

        for (let x = 0; x < bookmarks.length; x++)
        {
            if (bookmarks[x].name.toLowerCase() === name.toLowerCase()) { existing = x; }
        }

        if (existing === -1) { bookmarks.push({ name: name, host: typed_host, port: typed_port }); }
        else { bookmarks[existing] = { name: name, host: typed_host, port: typed_port }; }

        connection__write_server_bookmarks(bookmarks);
        connection__render_server_bookmarks();
    };

    document.getElementById("server-bookmark-delete").onclick = function()
    {
        let selected = document.getElementById("server-bookmark-select").value;

        if (selected === "") { utils__custom_alert("pick a saved server to delete"); return; }

        let bookmarks = connection__read_server_bookmarks();
        bookmarks.splice(parseInt(selected), 1);

        connection__write_server_bookmarks(bookmarks);
        connection__render_server_bookmarks();
        document.getElementById("server-bookmark-name").value = "";
    };
}
