// connection.js is included in template.html with the other client files, and in the node bundle
// that the android app runs.
// this file opens and keeps the connection to the server. it decides when a connection attempt
// starts: the connect button, a page with built-in server settings, settings sent by the android
// app, the return from idle, a retry, or attaching to a session the node client already holds.
// only one attempt runs at a time. the file knows which identity (key pair) and which server
// address the attempt uses.
// it also runs the heartbeat loop (the regular message that tells the server the client is still
// there), the fast reconnect (a second attempt right after a short connection loss, keeping the
// identity), the hold on the connect page (the page stays hidden for a moment while an automatic
// attempt runs, so it does not appear for a moment), and the saved server bookmarks (the server list on the
// connect page).
// other files do not connect on their own: ui.js (connect button) and android-host.js (settings
// and resume from the android app) ask for a connection with connection__request_connect and a
// trigger name, and dispatch.js passes on what the websocket worker reports (opened, closed,
// error, server messages) by calling the connection__* functions in this file.

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

// set by "disconnect" in the connections menu (connection__disconnect_by_user): the connect loop then waits for the next
// connect request instead of connecting again on its own, and a fast reconnect is refused while it is set
var is_connection_ended_by_user = false;

// the server the user chose instead of the one written into the page, { host, port }, or null: the last bookmark chosen in
// the bookmarks menu, or an address and port changed in the connect window and connected to with its button. a page with
// the server written into it connects to that server instead of its own from then on, until the page is loaded again or
// the window's values are changed back to the page's own
var connection_chosen_target = null;

// true once the connect window of a page with the server written into it shows the address and the port in its fields
// (connection__show_predefined_details_in_connect_window); the connect button then reads the fields
var is_written_in_target_shown_in_window = false;

// resolved by the websocket close and error handlers. the connect loop (connection__connect_loop,
// below) waits for this while a socket exists, whether the handshake is still running or the
// connection has been open for a long time
var connection_closed_resolvers = [];

// a request to connect at once, which skips the retry countdown; it comes from the connect button
// or from new connection details. a request that
// arrives while the connect loop still waits for the old socket to close is kept, not lost: an
// identity switch closes the socket and sends the request at the same time, before the close has
// been seen
var connect_loop_wake_resolver = null;
var is_connect_loop_wake_pending = false;

var is_connect_loop_running = false;

// webview only: true when connect was pressed before node announced its loopback port
var ui_connect_requested = false;

// ---- the connect loop ----

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
 * @brief a promise for the next connection close; the connect loop waits for it between attempts
 *
 * @return promise resolves when the connection closes
 */
function connection__connection_closed()
{
    return new Promise(function(resolve) { connection_closed_resolvers.push(resolve); });
}

/**
 * @brief wakes the connect loop out of its retry wait, or remembers the connect request for a wait that has not started yet
 *
 * @return void
 */
function connection__wake_connect_loop()
{
    if (connect_loop_wake_resolver != null)
    {
        let resolver = connect_loop_wake_resolver;
        connect_loop_wake_resolver = null;
        resolver();
        return;
    }

    is_connect_loop_wake_pending = true;
}

/**
 * @brief the countdown between retries: shows the failure text every second and ends early when a connect request arrives
 *
 * @param number seconds -> how long to wait
 *
 * @return promise resolves when the countdown ended or a connect request ended it early
 */
async function connection__connect_loop_retry_wait(seconds)
{
    if (is_connect_loop_wake_pending == true)
    {
        is_connect_loop_wake_pending = false;
        console.log("connect-path: retry wait skipped by a pending connect request");
        return;
    }

    let nudged = new Promise(function(resolve) { connect_loop_wake_resolver = resolve; });

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

    connect_loop_wake_resolver = null;
}

/**
 * @brief the wait after "disconnect" in the connections menu: the loop stays here until a connect request arrives
 *        a request that arrived already (the connect button pressed while connected ends the old connection first)
 *        ends the wait at once
 *
 * @return promise resolves when the loop may connect again
 */
async function connection__wait_after_user_disconnect()
{
    if (is_connect_loop_wake_pending == true)
    {
        is_connect_loop_wake_pending = false;
        is_connection_ended_by_user = false;
        return;
    }

    connection__set_connect_button_pending(false);
    connection__report_connection_status("idle", "disconnected");
    await new Promise(function(resolve) { connect_loop_wake_resolver = resolve; });
    is_connection_ended_by_user = false;
}

/**
 * @brief the connect loop, the one loop that starts connection attempts
 *        waits for a target and an identity, attempts once, waits for the close, then waits out a
 *        countdown and tries again; runs once per page and never returns
 *
 * @return promise never resolves
 */
async function connection__connect_loop()
{
    if (is_connect_loop_running == true)
    {
        return;
    }

    is_connect_loop_running = true;
    console.log("connect-path: connect loop started");

    while (true)
    {
        let target = await target_slot.wait();

        // "disconnect" was chosen: nothing connects until the next connect request
        if (is_connection_ended_by_user == true)
        {
            await connection__wait_after_user_disconnect();
            continue;
        }

        // the node host keeps the connection closed on purpose while the app goes idle (g_node_connection_wanted)
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

        // an attempt uses up any pending connect request; one kept from before must not skip a later countdown
        is_connect_loop_wake_pending = false;
        await connection__attempt_connection(target, identity);

        // wait for the socket to close. an attempt that could not even create a socket never
        // reports a close, so a wait without a login also has a deadline
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
                // a new attempt now would end a working reattach, so keep waiting
                if (target.kind === "loopback")
                {
                    console.log("connect-path: loopback attached, still waiting for node's login");
                    continue;
                }

                console.log("connect-path: no login within 45s, treating the attempt as failed");
                break;
            }
        }

        // the user ended the connection: no countdown and no fast reconnect, the loop waits for the next connect request
        if (is_connection_ended_by_user == true)
        {
            await connection__wait_after_user_disconnect();
            continue;
        }

        // a fast reconnect connects again at once with the same identity: no countdown, no button
        if (g_is_authenticated == false && g_fast_reconnect.in_progress == true)
        {
            continue;
        }

        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            // manual mode means one attempt per button press: state the failure and
            // wait until the connect button sends a connect request, with no countdown
            if (is_connect_loop_wake_pending == true)
            {
                is_connect_loop_wake_pending = false;
                console.log("connect-path: connecting on the pending connect request");
                continue;
            }

            connection__set_connect_button_pending(false);
            connection__report_connection_status("idle",
                (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
            await new Promise(function(resolve) { connect_loop_wake_resolver = resolve; });
            continue;
        }

        // connecting to node on this device costs nothing, so the loopback retries after 2 s; the
        // 30 s wait is for a remote server that may be down
        let retry_seconds = (target.kind === "loopback") ? 2 : 30;

        if (g_is_authenticated == false)
        {
            g_connection_status.next_retry_at = new Date().valueOf() + retry_seconds * 1000;
            connection__report_connection_status("waiting_retry",
                (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
        }

        await connection__connect_loop_retry_wait(retry_seconds);
    }
}

/**
 * @brief the one place that may start a connection, and the whole policy of when to connect
 *        "button", "bookmark" and "attach" always connect, "settings", "resume" and "retry" only under
 *        autoconnect, and "resume" also reattaches when node already holds a session
 *
 * @param string trigger -> "button", "bookmark", "attach", "settings", "resume" or "retry"
 * @param object bookmark_target -> with "bookmark": the server of the bookmark as { host, port }; ignored otherwise
 *
 * @return void
 */
function connection__request_connect(trigger, bookmark_target)
{
    let is_wanted =
        (trigger === "button")
        || (trigger === "bookmark")
        || (trigger === "attach")
        || (g_is_autoconnect_without_user_action_active == true)
        || (ui_connect_requested == true)
        || (trigger === "resume" && g_connection_status.state === "connected");

    console.log("connect-path: connection__request_connect(" + trigger + ") -> " + (is_wanted ? "dial" : "ignored"));

    if (is_wanted == false)
    {
        return;
    }

    // the window of a page with the server written into it shows the address and the port after a chosen disconnect, and the
    // user may change them: a press of the connect button then takes the window's values as the target from now on, like a
    // bookmark's. values changed back to the page's own make the page's details (with their wss port) the target again
    if (trigger === "button" && g_are_server_details_predefined == true && is_written_in_target_shown_in_window == true)
    {
        let address_in_window = document.getElementById("input-ip-address").value.trim();
        let port_in_window = document.getElementById("input-port-number").value.trim();

        if (address_in_window === String(g_autoconnect_details.host) && port_in_window === String(g_autoconnect_details.port))
        {
            connection_chosen_target = null;
        }
        else
        {
            connection_chosen_target = { host: address_in_window, port: port_in_window };
        }
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
    else if (trigger === "bookmark" && bookmark_target != null && typeof bookmark_target.host === "string" && bookmark_target.host.length > 0)
    {
        // a bookmark names its own server; the fields show it (head-menu.js), and a page with the server written into it
        // keeps connecting to the bookmark from now on (the retry countdown and the connect button use the same target)
        connection_chosen_target = { host: bookmark_target.host, port: bookmark_target.port };
        target_slot.set({ kind: "server", host: bookmark_target.host, port: bookmark_target.port });
    }
    else if (g_are_server_details_predefined == true && connection_chosen_target == null)
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

    connection__wake_connect_loop();
}

/**
 * @brief the connect button; it keeps its own name for the onclick wiring
 *
 * @return void
 */
function connection__submit_connection_target_from_ui()
{
    // the button is disabled while connected (connection__refresh_connect_button_state); a stray press changes nothing
    if (g_is_authenticated == true)
    {
        return;
    }

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
 *        headless, because the stand-in window object takes the dom calls
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

// ---- connection attempt and heartbeat ----

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

        // early wake used by android_host__exit_deep_idle: cancel this sleep's timer so an old
        // (long) timeout can never run later and overwrite a newer sleep's resolver
        g_connection_check.sleep_resolve = function()
        {
            clearTimeout(timer_id);
            g_connection_check.sleep_resolve = null;
            resolve();
        };
    });
}

// The regular heartbeat loop and Android's backup timer must both respect the
// server's setting. Android waits another 15 seconds before its backup takes over.
function connection__has_keepalive_timed_out(timestamp_now, extra_wait_ms = 0)
{
    return g_worker_connection_active === false
        && g_server_policy.is_connection_check_enforced !== false
        && g_connection_check.last_response_timestamp > 0
        && g_connection_check.last_response_timestamp + g_connection_check.lost_threshold_ms + extra_wait_ms < timestamp_now;
}

// Keep sending checks even when timeout enforcement is disabled. Their replies
// still measure latency, and a real socket closure still follows the usual cleanup.
async function connection__websocket_connection_check()
{
    while (g_should_connection_check_be_running == true)
    {
        // one exception in here must not end the loop: it runs BOTH the heartbeat and
        // the loss detection, and a stopped loop means a lost connection that nobody notices
        try
        {
            let timestamp_now = new Date().valueOf();

            if (g_connection_check.last_response_timestamp == 0)
            {
                g_connection_check.last_response_timestamp = timestamp_now;
            }

            if (connection__has_keepalive_timed_out(timestamp_now))
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
 * @brief fast reconnect: the socket is gone but the page keeps everything (lists, chat, channel, keys) and the connect loop connects again at once with the same identity, asking the server to adopt the session
 *
 * @param string reason -> why the socket went, for the log
 *
 * @return boolean false when a resume is not possible, so the caller runs the full reset instead
 */
function connection__try_start_fast_reconnect(reason)
{
    // node owns the real connection in the webview, and only a logged-in session can be resumed
    if (g_server_policy.is_fast_reconnect_allowed == false || g_is_authenticated == false || is_connection_ended_by_user == true
        || g_is_identity_switch_in_progress == true || android_host__is_ui_only_runtime() == true)
    {
        return false;
    }

    // one attempt per loss: when the resume socket itself closes, the caller runs the full reset
    if (g_fast_reconnect.in_progress == true)
    {
        return false;
    }

    g_fast_reconnect.in_progress = true;
    if (typeof server_info__reset === "function")
    {
        server_info__reset();
    }
    // Pending website replies belong to the old connection. Ask again after
    // login so an interrupted lookup does not leave its message without a card.
    link_previews__reset();
    g_fast_reconnect.resumed = false;
    g_fast_reconnect.pending_lists = null;

    // the connect loop reads this flag; the page itself is left exactly as it is
    g_is_authenticated = false;
    g_should_connection_check_be_running = false;
    g_connection_check.last_response_timestamp = 0;
    g_worker_connection_ended = true;
    channel_tree__stop_avatar_prefetch();

    // the worker goes back to handshake mode; it keeps the identity and the channel keys
    keys__cancel_channel_key_creation();
    g_data_processing_worker.postMessage({ type: "mainthread_reset_data", preserve_channel_keys: true });

    console.log("%cfast reconnect: " + reason + " - connecting again with the same identity", "color: #ffa500; font-weight: bold; font-size: 14px;");
    utils__custom_log("fast reconnect attempt (" + reason + ")");

    g_fast_reconnect.deadline_timer = setTimeout(function()
    {
        connection__fast_reconnect_failed("no adopted session within " + (FAST_RECONNECT_DEADLINE_MS / 1000) + " s");
    }, FAST_RECONNECT_DEADLINE_MS);

    // wakes the connect loop out of its wait on the old socket; it connects again instead of staying idle
    connection__signal_connection_closed();
    return true;
}

/**
 * @brief the one failure of a fast reconnect: the usual "connection lost" notice and the full reset
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
    g_worker_connection_ended = true;
    if (typeof server_info__reset === "function")
    {
        server_info__reset();
    }
    link_previews__reset();
    g_server_policy.is_link_preview_allowed = false;
    g_server_policy.is_connection_check_enforced = true;
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

    // the connect form comes back on its own only where it is a page of its own (android, the member list themes); as a
    // window it stays closed until "connect" in the connections menu, so a lost connection leaves the background and the head menu
    if (connection__is_connect_window_mode() == false)
    {
        document.getElementById('verification-system').style.display = "block";
    }
    document.getElementById('communication-system-container').style.display = "none";
    connection__mark_connected_ui_hidden(true);

    // after the page switch, otherwise the mic is repainted while the old screen is still up
    voice__update_microphone_button();

    // the "disconnect" entry of the head menu follows (head-menu.js, which the node bundle does not have)
    if (typeof head_menu__refresh_connections_items === "function")
    {
        head_menu__refresh_connections_items();
    }
    connection__refresh_connect_button_state();

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

    keys__cancel_channel_key_creation();
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
    // after a reconnect (they are slot indices), and it would keep sending requests on a closed socket
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
    audio_transport__reset();
    g_media_keys_from_host = null;
    g_media_server_key_layers_from_host = null;
    g_audio_transport.mode = "webrtc";
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
    keys__clear_channel_keys();
    chat_files__refresh_receive_preferences_controls();
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

    // a stream we sent or watched is gone with the connection
    if (typeof video__reset_on_disconnect === "function")
    {
        video__reset_on_disconnect();
    }
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
    connection__apply_connect_window_mode();
}

/**
 * @brief the connect button is disabled while a connection exists: the window may be open then ("connections" -> "connect"
 *        shows where the connection went), and a press would be meaningless. enabled again by the reset after a disconnect
 *
 * @return void
 */
function connection__refresh_connect_button_state()
{
    let button = document.getElementById("connect-button");

    if (button == null)
    {
        return;
    }

    button.disabled = (g_is_authenticated == true);
    button.title = (g_is_authenticated == true) ? "already connected" : "";
}

/**
 * @brief fades the connect button while an attempt runs; hiding rows caused a layout flicker
 *
 * @param boolean is_pending -> true while an attempt is running
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
 * @brief one connection attempt, started only by connection__connect_loop(): builds the ws/wss connection string and has the websocket worker open the socket with the right opening frame
 *
 * @param object target -> the host, port and keys to connect to
 * @param object identity -> the identity to present
 *
 * @return promise resolves when the attempt has ended, either way
 */
async function connection__attempt_connection(target, identity)
{
    let worker_channel = null;

    g_worker_connection_ended = false;
    g_worker_connection_active = target.kind !== "loopback"
        && typeof process === "undefined" && typeof MessageChannel === "function";
    if (g_worker_connection_active)
    {
        worker_channel = new MessageChannel();
        g_worker_connection_generation++;
        g_data_processing_worker.postMessage(
        {
            type: "mainthread__connect_socket_port",
            port: worker_channel.port1,
            connection_generation: g_worker_connection_generation
        }, [worker_channel.port1]);
    }

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

    // a new attempt is running - the failure text only shows after this one resolves
    g_last_connect_attempt_failed = false;

    // loopback: connect to node on this device, token as the opening frame, no dh.
    // stun/turn still point at the real server, so keep the real host for webrtc
    if (target.kind === "loopback")
    {
        g_host = g_autoconnect_details.host;
        port = g_autoconnect_details.port;
        g_audio_transport.url = "ws://" + (g_host.indexOf(":") >= 0 && g_host[0] !== "[" ? "[" + g_host + "]" : g_host) + ":" + port + "/";

        let loopback_connection_string = "ws://127.0.0.1:" + target.port + "/";
        console.log("connection_string -> " + loopback_connection_string + " (loopback)");

        // name the SERVER being joined, not the local connection to node - and never print an
        // unknown one ("connecting to :0" when the settings have not arrived yet)
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

    g_keys_init_status = false;
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
    g_audio_transport.url = connection_string;
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
        port: worker_channel == null ? null : worker_channel.port2,
        connection_generation: g_worker_connection_generation,
        value: {
            connection_string: connection_string,
            onopen_data: data
        }
    }, worker_channel == null ? [] : [worker_channel.port2]);
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
        connection_generation: g_worker_connection_active ? g_worker_connection_generation : undefined,
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

// ---- the connect window ----
// the connect form (#verification-system holds it, #container-2 is the box) is a window in the desktop themes and in the
// mobile theme: a dialog with a title bar, movable by it (main.js), closeable. on android and in the two member list
// themes (simpledark, bluebell) it stays a page of its own, the way it has always been: there the stylesheets hide the
// title bar and the head menu entries, and the page shows itself again after every disconnect

/**
 * @brief whether the connect form is a closeable window (desktop themes, the mobile theme) rather than a page of its own (android, the member list themes simpledark and bluebell)
 *        false as well in the node runtime, which has no page
 *
 * @return boolean
 */
function connection__is_connect_window_mode()
{
    if (g_is_running_in_android_webview == true || typeof document === "undefined" || typeof document.getElementsByClassName !== "function")
    {
        return false;
    }

    let theme = connection__current_theme_name();

    return (theme !== "simpledark" && theme !== "bluebell");
}

/**
 * @brief the name of the theme in use: the one theme sheet without a media attribute (UI.apply_theme in ui.js switches the sheets that way)
 *
 * @return string the theme name, "default" when none is found
 */
function connection__current_theme_name()
{
    let sheets = document.getElementsByClassName("style-theme-html-element");

    for (let i = 0; i < sheets.length; i++)
    {
        if (sheets[i].hasAttribute("media") == false && typeof sheets[i].id === "string" && sheets[i].id.indexOf("style-theme-") == 0)
        {
            return sheets[i].id.substring("style-theme-".length);
        }
    }

    return "default";
}

/**
 * @brief shows the connect window: "connect" in the connections menu, a bookmark, and the first show at start-up (connection__reveal_connect_page)
 *
 * @return void
 */
function connection__show_connect_window()
{
    let page = document.getElementById("verification-system");

    if (page == null)
    {
        return;
    }

    page.style.display = "block";
    connection__apply_connect_window_mode();
}

/**
 * @brief hides the connect window (its close button); whatever the connection is doing goes on behind it
 *
 * @return void
 */
function connection__hide_connect_window()
{
    let page = document.getElementById("verification-system");

    if (page != null)
    {
        page.style.display = "none";
    }
}

/**
 * @brief marks on the body whether the connected interface is hidden, for the theme sheets: the hybrid theme paints a page
 *        background of its own while only the connect window, or nothing, is shown. the page starts with the mark set
 *        (template.html); the login path clears it (dispatch.js), a disconnect sets it again
 *
 * @param is_hidden true while no connected interface is shown
 *
 * @return void
 */
function connection__mark_connected_ui_hidden(is_hidden)
{
    if (typeof document === "undefined" || document.body == null || document.body.classList == null)
    {
        return;
    }

    document.body.classList.toggle("is-connected-ui-hidden", is_hidden == true);
}

/**
 * @brief gives the connect form the shape of the mode in use: a window pinned to the middle of the screen, or a page in its own flow
 *        called on every show and on every theme switch, because the mode depends on the theme. the generic centering of
 *        main.js is switched off for this box, because that one centers on every show and knows nothing about the modes
 *
 * @return void
 */
function connection__apply_connect_window_mode()
{
    let box = (typeof document !== "undefined") ? document.getElementById("container-2") : null;
    let page = (typeof document !== "undefined") ? document.getElementById("verification-system") : null;

    if (box == null || page == null || typeof box.setAttribute !== "function")
    {
        return;
    }

    box.setAttribute("data-recenter-on-open", "false");

    // the saved servers: as a window, the bookmarks menu holds them, so the row inside the form is hidden; as a page the
    // row stays, there is no menu. a page with the server written into it has no use for the row either way (main.js hides it)
    let bookmarks_row = document.getElementById("server-bookmarks-container");

    if (bookmarks_row != null && g_are_server_details_predefined == false)
    {
        bookmarks_row.style.display = (connection__is_connect_window_mode() == true) ? "none" : "";
    }

    if (connection__is_connect_window_mode() == false)
    {
        box.style.position = "";
        box.style.margin = "";
        box.style.left = "";
        box.style.top = "";
        box.style.zIndex = "";
        return;
    }

    // the window must paint above the spacer that follows it in the page: the default theme positions every div, so a
    // later sibling without this would cover the title bar and swallow the clicks on it and on the close mark
    box.style.zIndex = "5";

    if (page.style.display == "none")
    {
        return;
    }

    connection__show_predefined_details_in_connect_window();
    connection__refresh_connect_button_state();
    connection__refresh_connect_window_sections();

    // a page without a size yet (a tab that is still hidden while it loads) is centered once it has one
    if (window.innerWidth == 0 || window.innerHeight == 0 || box.offsetWidth == 0)
    {
        window.setTimeout(connection__apply_connect_window_mode, 250);
        return;
    }

    // measured as laid out, then pinned to the middle of the screen; a drag by the title bar moves it from there (main.js)
    box.style.position = "fixed";
    box.style.margin = "0";
    box.style.left = Math.max(0, Math.round((window.innerWidth - box.offsetWidth) / 2)) + "px";
    box.style.top = Math.max(0, Math.round((window.innerHeight - box.offsetHeight) / 2)) + "px";
}

/**
 * @brief a page with the server written into it (window.__SERVER_CONFIG__, main.js) hides the address and port fields, because
 *        they are never read; in the window they are shown anyway, filled with that server and not editable, so the user sees
 *        where the connection goes and, after a disconnect, where it went
 *
 * @return void
 */
function connection__show_predefined_details_in_connect_window()
{
    if (g_are_server_details_predefined != true || typeof g_autoconnect_details !== "object" || g_autoconnect_details == null)
    {
        return;
    }

    let address_field = document.getElementById("input-ip-address");
    let port_field = document.getElementById("input-port-number");
    let grid = document.getElementById("connect-form-grid");
    let keys_grid = document.getElementById("connect-keys-grid");

    if (address_field == null || port_field == null || grid == null || keys_grid == null)
    {
        return;
    }

    // the fields are filled once, with the server the page names, and left alone from then on: after a chosen disconnect
    // they show where the connection went (a bookmark writes them itself, head-menu.js), and the user may change them; the
    // connect button takes changed values as the target (connection__request_connect)
    if (is_written_in_target_shown_in_window == false && typeof g_autoconnect_details.host === "string" && g_autoconnect_details.host.length > 0)
    {
        address_field.value = g_autoconnect_details.host;
        port_field.value = (g_autoconnect_details.port != null) ? String(g_autoconnect_details.port) : "";
        address_field.disabled = false;
        port_field.disabled = false;
        is_written_in_target_shown_in_window = true;
    }

    grid.style.display = "";
    grid.style.visibility = "visible";

    // the keys the page wrote in, one row each (ui.js), built once: the window shows, after a chosen disconnect too, which
    // keys the page logs in with and of what type, and the user may change or remove them. keys.js reads the rows from then
    // on, also when none is left; the mark on the grid tells it so
    if (keys_grid.getAttribute("data-written-in-keys-built") !== "true" && typeof UI !== "undefined" && typeof UI.add_key_row === "function")
    {
        if (Array.isArray(g_autoconnect_details.keys))
        {
            for (let i = 0; i < g_autoconnect_details.keys.length; i++)
            {
                UI.add_key_row(String(g_autoconnect_details.keys[i]));
            }
        }

        keys_grid.setAttribute("data-written-in-keys-built", "true");
    }

    // such a page connects by itself and keeps its connect button and its key button invisible (main.js); after a chosen
    // disconnect the connect button is the way back
    document.getElementById("connect-button").style.visibility = "visible";
    document.getElementById("add-key-button").style.visibility = "visible";
}

/**
 * @brief fills the identity section of the connect window: a short fingerprint of this client's public key (the first twelve hex
 *        digits of its sha-256). called on every show and when a key pair arrives
 *
 * @return void
 */
function connection__refresh_connect_window_sections()
{
    let summary = (typeof document !== "undefined") ? document.getElementById("connect-identity-summary") : null;

    if (summary != null)
    {
        if (g_is_rsa_key_generated == true && typeof g_rsa_public_key_string === "string" && g_rsa_public_key_string.length > 0 && typeof _sha256 !== "undefined")
        {
            let digest = _sha256.create();
            digest.update(g_rsa_public_key_string);
            let hex = digest.hex().substring(0, 12);
            summary.textContent = hex.substring(0, 4) + " " + hex.substring(4, 8) + " " + hex.substring(8, 12);
        }
        else
        {
            summary.textContent = "making a key pair ...";
        }
    }
}

/**
 * @brief "disconnect" in the connections menu, and the connect button while connected: ends the connection and keeps the
 *        connect loop from connecting again on its own. nothing connects until a connect request arrives (the connect
 *        button, a bookmark); under autoconnect the retry countdown does not run either
 *
 * @return void
 */
function connection__disconnect_by_user()
{
    is_connection_ended_by_user = true;
    is_connect_loop_wake_pending = false;
    g_last_disconnect_reason = "disconnected";

    // the window comes back, so the user sees where the connection went; the reset that follows the close leaves it open
    if (connection__is_connect_window_mode() == true)
    {
        connection__show_connect_window();
    }

    if (g_is_authenticated == true || g_connection_status.state === "connecting")
    {
        // the socket closes; the close handler in dispatch.js runs the usual reset, and the connect loop, which waits for
        // that close, then waits for the next connect request (a fast reconnect is refused while the flag is set)
        g_websocket_worker.postMessage({ type: "close" });
        return;
    }

    // no socket: the loop sits in its retry countdown or waits for the button; woken, it reaches the same wait
    connection__wake_connect_loop();
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

    // the bookmarks menu in the head menu shows the same list (head-menu.js, which the node bundle does not have)
    if (typeof head_menu__render_bookmarks === "function")
    {
        head_menu__render_bookmarks();
    }
}

/**
 * @brief saves a server under a name, or replaces the bookmark of that name; the window's row and the bookmarks menu both call it
 *
 * @param string name -> the name typed for the server
 * @param string host -> the address
 * @param string port -> the port, as typed
 *
 * @return boolean true when the bookmark was written
 */
function connection__save_server_bookmark(name, host, port)
{
    let bookmarks = connection__read_server_bookmarks();
    let existing = -1;

    name = (name || "").trim();
    host = (host || "").trim();
    port = (port || "").trim();

    if (name.length === 0) { utils__custom_alert("give this server a name first"); return false; }
    if (host.length === 0 || port.length === 0) { utils__custom_alert("fill in the address and port first"); return false; }

    for (let x = 0; x < bookmarks.length; x++)
    {
        if (bookmarks[x].name.toLowerCase() === name.toLowerCase()) { existing = x; }
    }

    if (existing === -1) { bookmarks.push({ name: name, host: host, port: port }); }
    else { bookmarks[existing] = { name: name, host: host, port: port }; }

    connection__write_server_bookmarks(bookmarks);
    connection__render_server_bookmarks();
    return true;
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
        connection__save_server_bookmark(document.getElementById("server-bookmark-name").value,
            document.getElementById("input-ip-address").value, document.getElementById("input-port-number").value);
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
