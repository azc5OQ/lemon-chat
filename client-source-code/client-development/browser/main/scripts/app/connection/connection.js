// manages server connections, identity changes, recovery and saved servers

var port = 0;

var FAST_RECONNECT_DEADLINE_MS = 60000;
var pending_upload_send_cancel = null;
var connection_desired_generation = 0;
var connection_attempt_cancellations = new Set();
var connection_local_request_sequence = 0;
var connection_local_uploads = new Map();
var connection_loopback_ready = false;
var connection_ui_connect_pending = false;
var connection_ui_disconnect_pending = false;

/**
 * @brief sends the pending Connect or Disconnect when the local socket is ready
 */
function connection__flush_local_connect_intent()
{
    if (!g_connection_status.runtime_failure_reason && connection_loopback_ready && connection_ui_disconnect_pending)
    {
        connection_ui_disconnect_pending = false;
        connection__send_local_command("disconnect");
        return;
    }
    if (!g_connection_status.runtime_failure_reason && connection_loopback_ready && connection_ui_connect_pending)
    {
        connection_ui_connect_pending = false;
        connection__send_local_command("connect");
    }
}

/**
 * @brief cancels connection attempts and upload waits for the previous target
 */
function connection__invalidate_pending_attempts()
{
    connection_desired_generation++;
    for (const cancel of Array.from(connection_attempt_cancellations))
    {
        cancel();
    }
    connection__cancel_upload_send();
}

/**
 * @brief checks whether an attempt still has permission to connect
 *
 * @param generation attempt generation to check
 *
 * @return true when the attempt is current and connecting is allowed
 */
function connection__attempt_is_current(generation)
{
    return generation === connection_desired_generation && !is_connection_ended_by_user && g_node_connection_wanted !== false && !g_connection_status.runtime_failure_reason;
}

/**
 * @brief waits for the identity unless the connection attempt is cancelled
 *
 * @param generation attempt generation to wait for
 *
 * @return Promise resolving to the identity or null when cancelled
 */
function connection__wait_for_identity(generation)
{
    return new Promise(function(resolve)
    {
        const finish = function(identity)
        {
            connection_attempt_cancellations.delete(cancel);
            const index = g_identity_slot.waiters.indexOf(finish);
            if (index >= 0)
            {
                g_identity_slot.waiters.splice(index, 1);
            }
            resolve(identity);
        };
        const cancel = function()
        {
            finish(null);
        };
        if (!connection__attempt_is_current(generation))
        {
            finish(null);
        }
        else if (g_identity_slot.is_set)
        {
            finish(g_identity_slot.value);
        }
        else
        {
            connection_attempt_cancellations.add(cancel);
            g_identity_slot.waiters.push(finish);
        }
    });
}

/**
 * @brief sends a command to the local Node owner
 *
 * @param action local command name
 * @param arguments_value command arguments or an empty object
 */
function connection__send_local_command(action, arguments_value)
{
    connection__send_message_object(
    {
        message:
        {
            type: "local_command",
            request_id: String(++connection_local_request_sequence),
            action: action,
            arguments: arguments_value || {}
        }
    });
}

var VERIFICATION_MESSAGE = "welcome";

// the current server or local Node connection target
var target_slot = connection__make_slot();

// holds the identity supplied by the worker
g_identity_slot = connection__make_slot();

// the passphrase and key size identify duplicate requests

var identity_requested_for = null;
var identity_requested_bits = 0;

// stops retries after the user disconnects

var is_connection_ended_by_user = false;

// the server selected in the connect form or bookmarks

var connection_chosen_target = null;

// tracks whether the configured server fields are visible

var is_written_in_target_shown_in_window = false;

// waiters resolved when the socket closes or fails

var connection_closed_resolvers = [];

// retains a connect request until the loop can consume it

var connect_loop_wake_resolver = null;
var is_connect_loop_wake_pending = false;

var is_connect_loop_running = false;

// retains a WebView connect request until Node provides its port
var ui_connect_requested = false;

/**
 * @brief creates a shared value that callers can await
 *
 * @return object slot with set and wait methods
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

        return new Promise(function(resolve)
        {
            slot.waiters.push(resolve);
        });
    };

    return slot;
}

var identity_request_sequence = 0;
var identity_active_request = null;
var identity_pending_request = null;

/**
 * @brief starts identity generation in the data worker
 *
 * @param request passphrase, key size and identity revision
 */
function connection__start_identity_request(request)
{
    identity_active_request = request;
    g_data_processing_worker.postMessage(
    {
        type: "mainthread__generate_rsa_keypair",
        from_identity_string: request.passphrase !== null,
        identity_passphrase_string: request.passphrase,
        rsa_key_bits: request.bits,
        identity_request_id: request.id,
        identity_revision: request.revision
    });
}

/**
 * @brief accepts the current identity result or starts its pending replacement
 *
 * @param result completed worker identity request
 *
 * @return true when the result is current and has no queued replacement
 */
function connection__accept_identity_result(result)
{
    if (identity_active_request == null || result.identity_request_id !== identity_active_request.id || result.identity_revision !== identity_active_request.revision || result.rsa_key_bits !== identity_active_request.bits)
    {
        return false;
    }
    identity_active_request = null;
    if (identity_pending_request != null)
    {
        const pending = identity_pending_request;
        identity_pending_request = null;
        connection__start_identity_request(pending);
        return false;
    }
    g_identity_revision = result.identity_revision;
    g_identity_rsa_key_bits = result.rsa_key_bits;
    return true;
}

/**
 * @brief requests an identity derived from a passphrase or generated randomly
 *
 * @param string|null passphrase_or_null -> passphrase with at least 199 characters or null for a random identity
 * @param requested_bits requested RSA key size
 * @param requested_revision native identity revision
 *
 * @return void
 */
function connection__request_identity(passphrase_or_null, requested_bits, requested_revision)
{
    const passphrase = typeof passphrase_or_null === "string" && passphrase_or_null.length >= 199 ? passphrase_or_null : null;
    const bits = G_ALLOWED_RSA_KEY_BITS.indexOf(requested_bits) >= 0 ? requested_bits : g_rsa_key_bits;
    const revision = Number.isSafeInteger(requested_revision) && requested_revision >= 0 ? requested_revision : g_identity_revision;
    const requested_key = passphrase === null ? "(random)" : passphrase;
    const latest_revision = identity_pending_request != null ? identity_pending_request.revision : (identity_active_request != null ? identity_active_request.revision : g_identity_revision);
    if (identity_requested_for === requested_key && identity_requested_bits === bits && latest_revision === revision && (identity_active_request != null || g_is_rsa_key_generated))
    {
        if (g_is_rsa_key_generated && identity_active_request == null && identity_pending_request == null && typeof g_node_frame_listener === "function")
        {
            // acknowledge a repeated import of the current identity
            g_node_frame_listener(JSON.stringify(
            {
                message:
                {
                    type: "local_identity_result",
                    identity_revision: g_identity_revision,
                    public_key_string: g_rsa_public_key_string,
                    rsa_key_bits: g_identity_rsa_key_bits,
                    success: true
                }
            }));
        }
        return;
    }
    if (g_data_processing_worker == null)
    {
        return;
    }
    const keep_connection_intent = target_slot.is_set && !is_connection_ended_by_user && g_node_connection_wanted !== false && (g_is_authenticated || (g_connection_status.state === "connecting" && g_worker_connection_generation > 0 && !g_worker_connection_ended));
    // invalidate the previous session before replacing its identity
    connection__invalidate_pending_attempts();
    g_is_identity_switch_in_progress = true;
    if (g_is_authenticated || g_connection_status.state === "connecting")
    {
        g_is_authenticated = false;
        connection__reset_chat_app_keep_identity();
    }
    identity_requested_for = requested_key;
    identity_requested_bits = bits;
    g_identity_slot.is_set = false;
    g_is_rsa_key_generated = false;
    const request =
    {
        id: ++identity_request_sequence,
        passphrase: passphrase,
        bits: bits,
        revision: revision
    };
    if (identity_active_request != null)
    {
        identity_pending_request = request;
    }
    else
    {
        connection__start_identity_request(request);
    }
    connection__report_connection_status("connecting", "generating the identity key (takes a while on first start)");
    if (keep_connection_intent)
    {
        // keep an existing connection request when changing its identity

        connection__wake_connect_loop();
    }
}

/**
 * @brief checks whether the page requests a secure WebSocket
 *
 * @return boolean true when window.__SERVER_CONFIG__.is_secure_websocket_used is true
 */
function connection__is_secure_websocket_requested_by_page()
{
    return typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.is_secure_websocket_used === true;
}

/**
 * @brief reports that the imported identity is ready
 *
 * @return void
 */
function connection__identity_imported()
{
    g_is_identity_import_pending = false;
    utils__custom_alert("Identity imported. This device now connects as that identity.");
    utils__custom_log("identity imported");
    sounds__play("identity_imported");
}

/**
 * @brief releases a failed identity request so it can be retried
 */
function connection__identity_generation_failed()
{
    g_is_identity_import_pending = false;
    identity_requested_for = null;
    identity_requested_bits = 0;
    g_is_rsa_key_generated = false;
    g_is_identity_switch_in_progress = false;
    g_identity_slot.is_set = false;
    const loading = document.getElementById("another-buttons-sub-loading-container");
    const controls = document.getElementById("another-buttons-sub-container");
    if (loading != null)
    {
        loading.style.display = "none";
    }
    if (controls != null)
    {
        controls.style.display = "";
    }
    connection__set_connect_button_pending(false);
    connection__report_connection_status("idle", "Could not create the identity. Secure randomness and the encryption runtime must be available. Retry creating or importing the identity.");
    if (typeof G_HAS_DOM !== "undefined" && G_HAS_DOM)
    {
        utils__custom_alert("Could not create the identity. Retry creating or importing it after checking that secure randomness and the encryption runtime are available.");
    }
}

// play the disconnect sound once per socket
var connection_sound_closed_generation = -1;
var G_SERVER_CLOSE_KICKED = 4001;
var G_SERVER_CLOSE_BANNED = 4002;

/**
 * @brief plays the connection outcome once for the ended socket
 *
 * @param close_code server close code when available
 */
function connection__play_disconnect_sound(close_code)
{
    if (!g_is_authenticated || g_is_identity_switch_in_progress || android_host__is_ui_only_runtime() || connection_sound_closed_generation === g_worker_connection_generation)
    {
        return;
    }
    connection_sound_closed_generation = g_worker_connection_generation;
    if (close_code === G_SERVER_CLOSE_KICKED)
    {
        sounds__play("you_were_kicked_from_the_server");
    }
    else if (close_code === G_SERVER_CLOSE_BANNED)
    {
        sounds__play("you_were_banned_from_the_server");
    }
    else
    {
        sounds__play(is_connection_ended_by_user ? "disconnected" : "connection_lost");
    }
}

/**
 * @brief resolves callers waiting for the socket to close
 *
 * @return void
 */
function connection__signal_connection_closed()
{
    connection_crypto__clear_handshake();
    let woken = connection_closed_resolvers;
    connection_closed_resolvers = [];

    for (let i = 0; i < woken.length; i++)
    {
        woken[i]();
    }
}

/**
 * @brief waits for the next socket closure
 *
 * @return Promise resolved when the socket closes
 */
function connection__connection_closed()
{
    return new Promise(function(resolve)
    {
        connection_closed_resolvers.push(resolve);
    });
}

/**
 * @brief wakes the connect loop or retains the request until it waits
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
 * @brief waits between retries unless another connect request arrives
 *
 * @param number seconds -> retry delay in seconds
 *
 * @return Promise resolved when the wait ends
 */
async function connection__connect_loop_retry_wait(seconds)
{
    if (is_connect_loop_wake_pending == true)
    {
        is_connect_loop_wake_pending = false;
        console.log("connect-path: retry wait skipped by a pending connect request");
        return;
    }

    let nudged = new Promise(function(resolve)
    {
        connect_loop_wake_resolver = resolve;
    });

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

        let done = await Promise.race([utils__sleep(1000).then(function()
        {
            return false;
        }), nudged.then(function()
        {
            return true;
        })]);

        if (done == true)
        {
            console.log("connect-path: retry wait skipped by nudge");
            break;
        }
    }

    connect_loop_wake_resolver = null;
}

/**
 * @brief waits for a connect request after the user disconnects
 *
 * @return Promise resolved when connecting is allowed
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
    connection__report_connection_status("idle", g_last_disconnect_reason || "disconnected");
    await new Promise(function(resolve)
    {
        connect_loop_wake_resolver = resolve;
    });
    is_connection_ended_by_user = false;
}

/**
 * @brief runs connection attempts and waits between retries
 *
 * @return Promise that remains active for this runtime
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

        // wait for an explicit request after Disconnect
        if (is_connection_ended_by_user == true)
        {
            await connection__wait_after_user_disconnect();
            continue;
        }

        // keep Node disconnected while its owner requests idle
        if (g_node_connection_wanted == false)
        {
            await utils__sleep(1000);
            continue;
        }

        let identity = null;
        const attempt_generation = connection_desired_generation;

        if (target.kind === "server")
        {
            console.log("connect-path: target " + target.host + ":" + target.port + ", waiting for keypair");
            identity = await connection__wait_for_identity(attempt_generation);
        }
        else
        {
            console.log("connect-path: target loopback :" + target.port);
        }

        if (!connection__attempt_is_current(attempt_generation) || target !== target_slot.value)
        {
            continue;
        }
        let close_resolver;
        const closed = new Promise(function(resolve)
        {
            close_resolver = resolve;
            connection_closed_resolvers.push(resolve);
        });

        g_last_disconnect_reason = "";
        connection__report_connection_status("connecting");

        // consume the pending request for this attempt
        is_connect_loop_wake_pending = false;
        const started = await connection__attempt_connection(target, identity, attempt_generation);
        if (started === false)
        {
            connection_closed_resolvers = connection_closed_resolvers.filter(function(resolve)
            {
                return resolve !== close_resolver;
            });
            continue;
        }

        // limit the wait when an attempt never reaches login

        while (true)
        {
            let outcome = await Promise.race([
                closed.then(function()
                {
                    return "closed";
                }),
                utils__sleep(45000).then(function()
                {
                    return "deadline";
                })
            ]);

            if (outcome === "closed")
            {
                break;
            }

            if (g_is_authenticated == false)
            {
                // keep the local socket while Node completes login

                if (target.kind === "loopback")
                {
                    console.log("connect-path: loopback attached, still waiting for node's login");
                    continue;
                }

                console.log("connect-path: no login within 45s, treating the attempt as failed");
                break;
            }
        }

        // skip retries after the user disconnects
        if (is_connection_ended_by_user == true)
        {
            await connection__wait_after_user_disconnect();
            continue;
        }

        // keep retries within the original recovery deadline
        // yield between recovery attempts
        if (g_is_authenticated == false && g_fast_reconnect.in_progress == true)
        {
            await connection__connect_loop_retry_wait(2);
            if (is_connection_ended_by_user == true)
            {
                continue;
            }
            if (g_fast_reconnect.in_progress == true)
            {
                continue;
            }
        }

        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            // manual mode waits for another Connect request

            if (is_connect_loop_wake_pending == true)
            {
                is_connect_loop_wake_pending = false;
                console.log("connect-path: connecting on the pending connect request");
                continue;
            }

            connection__set_connect_button_pending(false);
            connection__report_connection_status("idle", (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
            await new Promise(function(resolve)
            {
                connect_loop_wake_resolver = resolve;
            });
            continue;
        }

        // retry local sockets after 2 seconds and remote servers after 30 seconds

        let retry_seconds = (target.kind === "loopback") ? 2 : 30;

        if (g_is_authenticated == false)
        {
            g_connection_status.next_retry_at = new Date().valueOf() + retry_seconds * 1000;
            connection__report_connection_status("waiting_retry", (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
        }

        await connection__connect_loop_retry_wait(retry_seconds);
    }
}

/**
 * @brief applies connection policy and queues an allowed attempt
 *
 * @param string trigger -> source of the connection request
 * @param object bookmark_target -> server selected by a bookmark request
 *
 * @return void
 */
function connection__request_connect(trigger, bookmark_target)
{
    if (g_connection_status.runtime_failure_reason)
    {
        connection__render_connection_status();
        connection__reveal_connect_page();
        return;
    }
    const explicit_request = trigger === "button" || trigger === "bookmark";
    if (is_connection_ended_by_user && !explicit_request)
    {
        return;
    }
    if (explicit_request)
    {
        is_connection_ended_by_user = false;
        g_node_connection_wanted = true;
        connection_ui_disconnect_pending = false;
        if (android_host__is_ui_only_runtime())
        {
            const native_connect = typeof Android !== "undefined" && Android != null && typeof Android.JavaExportConnect === "function";
            connection_ui_connect_pending = !native_connect;
            if (native_connect)
            {
                Android.JavaExportConnect();
            }
            connection__flush_local_connect_intent();
            if (target_slot.is_set && target_slot.value.kind === "loopback" && !g_worker_connection_ended)
            {
                return;
            }
        }
    }
    const previous_target = target_slot.value;
    let is_wanted =
        (trigger === "button") || (trigger === "bookmark") || (trigger === "attach") || (g_is_autoconnect_without_user_action_active == true) || (ui_connect_requested == true) || (trigger === "resume" && g_connection_status.state === "connected");

    console.log("connect-path: connection__request_connect(" + trigger + ") -> " + (is_wanted ? "dial" : "ignored"));

    if (is_wanted == false)
    {
        return;
    }

    // use edited server fields until the configured address is restored

    if (G_HAS_DOM && trigger === "button" && g_are_server_details_predefined == true && is_written_in_target_shown_in_window == true)
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

    // choose the target for this runtime
    if (android_host__is_ui_only_runtime())
    {
        if (g_loopback_port <= 0)
        {
            // wait for Node to provide its local port
            if (trigger === "button")
            {
                ui_connect_requested = true;
                document.getElementById("another-buttons-loading-container-p").innerHTML = "waiting for app runtime...";
            }
            return;
        }

        ui_connect_requested = false;
        target_slot.set({ kind: "loopback", port: g_loopback_port, token: g_loopback_token });

        // keep the page visible for a button request

        if (trigger !== "button")
        {
            connection__hold_back_connect_page();
        }
    }
    else if (trigger === "bookmark" && bookmark_target != null && typeof bookmark_target.host === "string" && bookmark_target.host.length > 0)
    {
        // keep the selected bookmark as the connection target

        connection_chosen_target = { host: bookmark_target.host, port: bookmark_target.port };
        target_slot.set({ kind: "server", host: bookmark_target.host, port: bookmark_target.port });
    }
    else if (g_are_server_details_predefined == true && connection_chosen_target == null)
    {
        // ignore settings that do not yet contain a host
        if (typeof g_autoconnect_details.host !== "string" || g_autoconnect_details.host.length == 0 || (parseInt(g_autoconnect_details.port) > 0) == false)
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

    if (JSON.stringify(previous_target) !== JSON.stringify(target_slot.value))
    {
        connection__invalidate_pending_attempts();
    }
    connection__wake_connect_loop();
}

/**
 * @brief handles the connect button
 *
 * @return void
 */
function connection__submit_connection_target_from_ui()
{
    // ignore Connect while already authenticated
    if (g_is_authenticated == true)
    {
        return;
    }

    connection__request_connect("button");
}

/**
 * @brief describes a socket error for the connect page
 *
 * @param string error_code -> Node socket error code
 *
 * @return string connection failure text
 */
function connection__describe_socket_error(error_code)
{
    if (g_device_has_network === false)
    {
        return "no network connection (wifi and mobile data are off)";
    }

    if (error_code === "ENETUNREACH" || error_code === "EHOSTUNREACH" || error_code === "EAI_AGAIN" || error_code === "ENOTFOUND")
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
 * @brief stores connection status, notifies listeners and updates the page
 *
 * @param string state -> new connection state
 * @param string reason -> explanation of the state
 *
 * @return void
 */
function connection__report_connection_status(state, reason)
{
    if (g_connection_status.runtime_failure_reason)
    {
        state = "runtime_failed";
        reason = g_connection_status.runtime_failure_reason;
    }
    g_connection_status.state = state;
    g_connection_status.reason = (reason != null) ? reason : "";

    for (let i = 0; i < g_connection_status_listeners.length; i++)
    {
        try
        {
            g_connection_status_listeners[i](g_connection_status);
        }
        catch (e)
        {
        }
    }

    connection__render_connection_status();
}

/**
 * @brief stops connection attempts and displays the native runtime failure
 *
 * @param reason failure message to display
 */
function connection__handle_runtime_failure(reason)
{
    const already_failed = !!g_connection_status.runtime_failure_reason;
    const failure_reason = typeof reason === "string" && reason ? reason : "The app runtime stopped. Force-stop Lemon Chat, then open it again.";
    g_connection_status.runtime_failure_reason = failure_reason;
    g_last_disconnect_reason = failure_reason;
    g_connection_status.next_retry_at = 0;
    connection_ui_connect_pending = false;
    connection_ui_disconnect_pending = false;
    connection_loopback_ready = false;
    ui_connect_requested = false;
    is_connect_loop_wake_pending = false;
    is_connection_ended_by_user = true;
    g_node_connection_wanted = false;
    g_loopback_port = 0;
    g_loopback_token = "";
    if (!already_failed)
    {
        connection__invalidate_pending_attempts();
        if (g_websocket_worker != null && g_data_processing_worker != null)
        {
            connection__reset_chat_app_keep_identity();
        }
        g_is_authenticated = false;
        g_is_websocket_connected = false;
        g_worker_connection_ended = true;
        connection__signal_connection_closed();
        if (connect_loop_wake_resolver != null)
        {
            const resolve = connect_loop_wake_resolver;
            connect_loop_wake_resolver = null;
            resolve();
        }
    }
    connection__report_connection_status("runtime_failed", failure_reason);
    connection__reveal_connect_page();
}

/**
 * @brief updates the connect page from the current status
 *
 * @return void
 */
function connection__render_connection_status()
{
    if (g_connection_status.runtime_failure_reason)
    {
        g_connection_status.state = "runtime_failed";
        g_connection_status.reason = g_connection_status.runtime_failure_reason;
        g_connection_status.next_retry_at = 0;
    }
    let reason_element = document.getElementById("connection-status-reason");
    let countdown_element = document.getElementById("connection-status-countdown");
    let lastseen_element = document.getElementById("connection-status-lastseen");

    if (reason_element == null || countdown_element == null || lastseen_element == null)
    {
        return;
    }

    let reason_text = g_connection_status.reason;

    // read network availability from the browser
    if (!g_connection_status.runtime_failure_reason && typeof navigator !== "undefined" && navigator.onLine === false)
    {
        reason_text = "no network connection (wifi off?)";
    }

    reason_element.textContent = (g_connection_status.state === "connected") ? "" : reason_text;

    let countdown_text = "";

    // show the retry countdown below the connection status

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
        lastseen_text = (minutes_ago < 1) ? "last connected under a minute ago" : "last connected " + minutes_ago + " min ago";
    }

    lastseen_element.textContent = lastseen_text;
}

/**
 * @brief starts the timer that refreshes connection status each second
 *
 * @return void
 */
function connection__start_connection_status_ticker()
{
    window.setInterval(connection__render_connection_status, 1000);
}

/**
 * @brief waits between heartbeat checks with support for an early wake
 *
 * @param number ms -> delay in milliseconds
 *
 * @return Promise resolved when the wait ends
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

        // cancel the old sleep timer when waking early

        g_connection_check.sleep_resolve = function()
        {
            clearTimeout(timer_id);
            g_connection_check.sleep_resolve = null;
            resolve();
        };
    });
}

// respect the server timeout setting in both heartbeat timers
// the Android backup timer adds 15 seconds
/**
 * @brief checks the server timeout policy and heartbeat deadline
 *
 * @param timestamp_now current timestamp in milliseconds
 * @param extra_wait_ms additional grace period in milliseconds
 *
 * @return true when the connection has exceeded its heartbeat deadline
 */
function connection__has_keepalive_timed_out(timestamp_now, extra_wait_ms = 0)
{
    return g_worker_connection_active === false && g_server_policy.is_connection_check_enforced !== false && g_connection_check.last_response_timestamp > 0 && g_connection_check.last_response_timestamp + g_connection_check.lost_threshold_ms + extra_wait_ms < timestamp_now;
}

// send heartbeats even when timeout enforcement is disabled
// replies still update connection latency
/**
 * @brief sends heartbeats and detects connection loss until stopped
 *
 * @return Promise resolved when heartbeat checking stops
 */
async function connection__websocket_connection_check()
{
    while (g_should_connection_check_be_running == true)
    {
        // keep heartbeat and loss detection running after an error

        try
        {
            let timestamp_now = new Date().valueOf();

            if (g_connection_check.last_response_timestamp == 0)
            {
                g_connection_check.last_response_timestamp = timestamp_now;
            }

            if (connection__has_keepalive_timed_out(timestamp_now))
            {
                connection__play_disconnect_sound();
                let silence_ms = timestamp_now - g_connection_check.last_response_timestamp;
                console.error("loss detector: no valid server message for " + silence_ms + "ms");

                // the next login starts a new heartbeat loop
                if (connection__try_start_fast_reconnect("no valid server message for " + silence_ms + " ms") == true)
                {
                    break;
                }

                utils__custom_alert("connection with server lost");
                console.error("loss detector: resetting");
                connection__reset_chat_app_keep_identity();
            }

            g_session.ping_sent_at = timestamp_now; // used to measure response latency

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
 * @brief starts recovery while retaining the session and identity
 *
 * @param string reason -> connection loss reason
 *
 * @return boolean false when the session cannot be resumed
 */
function connection__try_start_fast_reconnect(reason)
{
    // only the runtime that owns an authenticated session can resume it
    if (g_server_policy.is_fast_reconnect_allowed == false || g_is_authenticated == false || is_connection_ended_by_user == true || g_is_identity_switch_in_progress == true || android_host__is_ui_only_runtime() == true)
    {
        return false;
    }

    // reuse the original recovery deadline
    if (g_fast_reconnect.in_progress == true)
    {
        return false;
    }

    connection__cancel_upload_send();
    g_fast_reconnect.in_progress = true;
    if (typeof server_info__reset === "function")
    {
        server_info__reset();
    }
    // retry interrupted website lookups after login

    link_previews__reset();
    g_fast_reconnect.resumed = false;
    g_fast_reconnect.pending_lists = null;

    // request recovery without clearing the page
    g_is_authenticated = false;
    groups__apply_server_mode();
    if (typeof server_emojis__reset === "function")
    {
        server_emojis__reset();
    }
    g_should_connection_check_be_running = false;
    g_connection_check.last_response_timestamp = 0;
    g_worker_connection_ended = true;
    channel_tree__stop_avatar_prefetch();

    // restart the handshake while retaining identity and channel keys
    keys__cancel_channel_key_creation();
    g_data_processing_worker.postMessage({ type: "mainthread_reset_data", preserve_channel_keys: true });

    console.log("%cfast reconnect: " + reason + " - connecting again with the same identity", "color: #ffa500; font-weight: bold; font-size: 14px;");
    utils__custom_log("fast reconnect attempt (" + reason + ")");

    if (G_HAS_DOM)
    {
        utils__custom_alert("Connection lost. Reconnecting…", "fast-reconnect");
    }

    g_fast_reconnect.deadline_timer = setTimeout(function()
    {
        connection__fast_reconnect_failed("no adopted session within " + (FAST_RECONNECT_DEADLINE_MS / 1000) + " s");
    }, FAST_RECONNECT_DEADLINE_MS);

    // wake the loop waiting on the previous socket
    connection__signal_connection_closed();
    return true;
}

/**
 * @brief reports failed recovery and resets the session
 *
 * @param string reason -> recovery failure reason
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
 * @brief marks the session resumed and waits for refreshed lists
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
 * @brief buffers resumed session lists until all required lists arrive
 *
 * @param string kind -> channel, client, icon or group list
 * @param object value -> list message
 *
 * @return boolean true when the list was buffered
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

    if (g_fast_reconnect.pending_lists.channel != null && g_fast_reconnect.pending_lists.client != null && (!g_server_policy.is_permission_system_enabled || (g_fast_reconnect.pending_lists.icon != null && g_fast_reconnect.pending_lists.group != null)))
    {
        connection__fast_reconnect_apply_lists();
    }

    return true;
}

/**
 * @brief applies all lists received for the resumed session
 *
 * @return void
 */
function connection__fast_reconnect_apply_lists()
{
    chat__remember_peer_state();
    let lists = g_fast_reconnect.pending_lists;

    g_fast_reconnect.pending_lists = null;
    g_fast_reconnect.resumed = false;
    clearTimeout(g_fast_reconnect.deadline_timer);
    g_fast_reconnect.deadline_timer = null;

    // replace all lists before repainting
    g_map_client_id_to_array_index.clear();
    g_client_list.length = 0;
    g_channel_list.length = 0;
    g_groups.length = 0;
    g_icons.length = 0;
    g_icon_upload_queue.length = 0;
    g_icon_upload_in_flight_base64 = null;
    g_is_client_list_retrieved = false;
    g_is_channel_list_retrieved = false;

    let elements_count = document.getElementsByClassName("connected-client").length;
    for (let i = 0; i < elements_count; i++)
    {
        document.getElementsByClassName("connected-client")[0].remove();
    }

    server_msg.process_channel_list_from_server(lists.channel);
    server_msg.process_client_list_from_server(lists.client);
    if (g_server_policy.is_permission_system_enabled)
    {
        server_msg.process_icon_list_from_server(lists.icon);
        server_msg.process_group_list_from_server(lists.group);
    }

    console.log("%cFAST RECONNECT HAPPENED - session resumed, lists refreshed, nothing was lost", "color: #00e000; font-weight: bold; font-size: 20px;");
    utils__custom_log("fast reconnect happened");

    if (G_HAS_DOM)
    {
        utils__hide_custom_alert("", "fast-reconnect");
    }
}

/**
 * @brief clears connection, chat and media state while retaining the identity
 *
 * @param boolean preserve_loopback -> true to keep the local Node socket and worker connection alive
 *
 * @return void
 */
function connection__reset_chat_app_keep_identity(preserve_loopback = false)
{
    unreceipted_messages_by_sender = {};
    seen_receipts_already_sent = {};
    seen_state_by_context = {};
    chat_peer_local_state.clear();
    chat_last_delivery_by_context.clear();
    node_current_call = null;
    dispatch_failed_deliveries.clear();
    dispatch_processed_deliveries.clear();
    if (typeof dispatch_discarded_deliveries !== "undefined")
    {
        dispatch_discarded_deliveries.clear();
    }
    dispatch_accepted_deliveries.clear();
    if (G_HAS_DOM)
    {
        utils__hide_custom_alert("", "fast-reconnect");
        utils__hide_custom_alert("", "channel-context");
    }
    connection__cancel_upload_send();
    clearTimeout(g_fast_reconnect.deadline_timer);
    g_fast_reconnect.deadline_timer = null;
    g_fast_reconnect.in_progress = false;
    g_fast_reconnect.resumed = false;
    g_fast_reconnect.pending_lists = null;
    if (!preserve_loopback)
    {
        g_worker_connection_ended = true;
    }
    if (typeof server_info__reset === "function")
    {
        server_info__reset();
    }
    link_previews__reset();
    g_is_local_client_admin = false;
    g_server_policy.is_permission_system_enabled = true;
    g_server_policy.is_stored_client_list_allowed = false;
    groups_window.stored_users_allowed = null;
    groups_window.access = null;
    g_server_policy.is_link_preview_allowed = false;
    g_server_policy.is_connection_check_enforced = true;
    // clear typing notices and their timer
    g_typing.state = {};
    chat__render_typing_indicator();

    // close any socket that still belongs to this session

    if (!preserve_loopback)
    {
        g_websocket_worker.postMessage({type: "close"});
    }

    chat_files__clear_pending_chat_file();
    chat_files__clear_pending_chat_picture();
    UI.clear_chat_button_onclick();

    // clear the client list and its index together

    g_map_client_id_to_array_index.clear();
    g_client_list.length = 0;

    // discard login data from the ended session

    g_node_cached_auth_frame = null;

    document.getElementById("another-buttons-sub-container").style.display = "";
    document.getElementById("another-buttons-sub-loading-container").style.display = "none";
    document.getElementById("another-buttons-loading-container-p").innerHTML = "loading...";
    connection__set_connect_button_pending(false);
    g_is_microphone_available = false;

    // restore the connect page when the theme uses one

    if (connection__is_connect_window_mode() == false)
    {
        document.getElementById('verification-system').style.display = "block";
    }
    document.getElementById('communication-system-container').style.display = "none";
    connection__mark_connected_ui_hidden(true);

    // refresh the microphone after switching pages
    voice__update_microphone_button();

    // refresh the Disconnect menu entry when available
    if (typeof head_menu__refresh_connections_items === "function")
    {
        head_menu__refresh_connections_items();
    }
    connection__refresh_connect_button_state();

    // hide the toolbar settings button on the connect page

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
    if (!preserve_loopback)
    {
        g_data_processing_worker.postMessage({type: "mainthread_reset_data"});
    }

    g_base64_picture_string_to_send = "" ;
    g_is_websocket_connected = false;
    g_local_username = "";
    g_local_client_id = 0;
    g_alert_push_to_talk_key_shown_once = false;
    g_alert_streaming_music_shown_once = false;
    g_stop_song_stream_message_received = false;
    g_selected_server_chat_message_id = null;
    g_is_authenticated = false;
    groups__apply_server_mode();
    if (typeof server_emojis__reset === "function")
    {
        server_emojis__reset();
    }
    g_should_connection_check_be_running = false;

    // stop avatar requests for the previous session

    channel_tree__stop_avatar_prefetch();

    // restore background idle after the next login
    g_is_deep_idle_pending = (g_is_deep_idle == true) || g_is_deep_idle_pending;
    g_is_deep_idle = false;
    g_connection_check.interval_ms = 10 * 1000;
    g_connection_check.lost_threshold_ms = 35 * 1000; // matches the startup timeout
    g_current_channel_id = 0;
    g_current_chat_context_id = "chat-context-channel-0";
    chat_replies__refresh();
    g_chat_message_receiver_type = "channel";
    g_chat_message_receiver_id = "main";
    g_is_client_list_retrieved = false;
    g_is_channel_list_retrieved = false;
    audio_transport__reset();
    g_media_keys_from_host = null;
    g_media_server_key_layers_from_host = null;
    g_audio_transport.mode = "webrtc";
    g_is_webrtc_datachannel_connected = false;

    // clear shared arrays in place

    g_icons.length = 0;
    g_icon_upload_queue.length = 0;
    g_icon_upload_in_flight_base64 = null;
    g_groups.length = 0;
    g_channel_list.length = 0;

    g_offline_client_list = [];

    g_is_chat_hidden = false;
    layout__layout_apply(); // refresh panel visibility
    g_local_message_id = 0;
    g_selected_font = "custom-font-usage-default";
    g_selected_font_color = "#ffffff";
    g_selected_font_size = 12;
    g_is_microphone_enabled_on_touch_device = false;  // reset touch microphone state

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

    // clear voice buffers

    g_peer_connection_with_server = null;

    g_iceconfig = null;

    g_is_voice_chat_allowed_by_server = false;
    g_is_client_microphone_allowed_by_server = false;
    g_is_microphone_enabled = false;

    // stop streams from the ended session
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

        }
    }
}

/**
 * @brief shows or hides the connection loading overlay
 *
 * @param boolean is_visible -> true to show the overlay
 *
 * @return void
 */
function connection__set_connect_holdback_loader_visible(is_visible)
{
    let loader = document.getElementById("connect-holdback-loader");

    if (loader != null)
    {
        // use a dark fallback when the theme background is transparent

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
 * @brief hides the connect page while Node connects, subject to a deadline
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

    // reveal the page if Node does not respond
    g_connect_holdback_deadline = new Date().valueOf() + 2500;
}

/**
 * @brief extends the loading overlay without exceeding its maximum deadline
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
 * @brief reveals the connect page when its loading deadline expires
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
 * @brief shows the connect page and removes the loading overlay
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
 * @brief disables Connect while a session is authenticated
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
 * @brief updates the connect button's progress appearance
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
 * @brief opens one socket with the selected target and identity
 *
 * @param object target -> server or local Node connection settings
 * @param object identity -> identity used for login
 * @param number|undefined desired_generation -> connection intent to apply, or the current intent when omitted
 *
 * @return Promise<boolean|undefined> resolves when the attempt ends
 */
async function connection__attempt_connection(target, identity, desired_generation)
{
    if (desired_generation == null)
    {
        desired_generation = connection_desired_generation;
    }
    if (!connection__attempt_is_current(desired_generation))
    {
        return false;
    }
    let worker_channel = null;

    g_worker_connection_ended = false;
    g_worker_connection_generation++;
    g_data_processing_worker.postMessage(
    {
        type: "mainthread__begin_connection",
        connection_generation: g_worker_connection_generation
    });
    g_worker_connection_active = target.kind !== "loopback" && typeof process === "undefined" && typeof MessageChannel === "function";
    if (g_worker_connection_active)
    {
        worker_channel = new MessageChannel();
        g_data_processing_worker.postMessage(
        {
            type: "mainthread__connect_socket_port",
            port: worker_channel.port1,
            connection_generation: g_worker_connection_generation
        }, [worker_channel.port1]);
    }

    if (android_host__is_ui_only_runtime())
    {
        // keep the page visible while the button shows progress
        connection__set_connect_button_pending(true);
    }
    else
    {
        document.getElementById("another-buttons-sub-container").style.display = "none";
        document.getElementById("another-buttons-sub-loading-container").style.display = "";
        document.getElementById("another-buttons-loading-container-p").innerHTML = "connecting to server...";
    }

    // clear the previous attempt's failure text
    g_last_connect_attempt_failed = false;

    // authenticate the local socket with its token
    // keep the remote host for STUN and TURN
    if (target.kind === "loopback")
    {
        connection_loopback_ready = false;
        g_host = g_autoconnect_details.host;
        port = g_autoconnect_details.port;
        g_audio_transport.url = "ws://" + (g_host.indexOf(":") >= 0 && g_host[0] !== "[" ? "[" + g_host + "]" : g_host) + ":" + port + "/";

        let loopback_connection_string = "ws://127.0.0.1:" + target.port + "/";
        console.log("connection_string -> " + loopback_connection_string + " (loopback)");

        // show the remote server address only when it is known

        document.getElementById("another-buttons-loading-container-p").innerHTML = (typeof g_host === "string" && g_host.length > 0) ? "connecting to " + chat__sanitize_string(g_host) + ":" + chat__sanitize_string("" + port) : "connecting...";

        g_websocket_worker.postMessage({
            type: "create_websocket_object",
            connection_generation: g_worker_connection_generation,
            value: {
                connection_string: loopback_connection_string,
                onopen_data: target.token
            }
        });

        return true;
    }

    g_host = target.host;
    port = target.port;

    g_keys_init_status = false;
    keys__init_keys_object();

    while (g_keys_init_status == false)
    {
        await utils__sleep(100);
        if (!connection__attempt_is_current(desired_generation))
        {
            if (worker_channel != null)
            {
                worker_channel.port1.close();
                worker_channel.port2.close();
            }
            return false;
        }
    }

    if (!connection__attempt_is_current(desired_generation))
    {
        return false;
    }

    let protocol_part_of_connection_string = "ws://";
    let connection_port = port;

    // select a secure WebSocket when required
    if (document.location.protocol == "https:" || connection__is_secure_websocket_requested_by_page() == true)
    {
        protocol_part_of_connection_string = "wss://";
        // use the separate secure WebSocket port for HTTPS
        if (target.wss_port)
        {
            connection_port = target.wss_port;
        }
    }

    let connection_string = protocol_part_of_connection_string + '' + g_host + ':' + connection_port + '/';
    g_audio_transport.url = connection_string;
    console.log("connection_string -> " + connection_string);

    // show the selected server and transport
    document.getElementById("another-buttons-loading-container-p").innerHTML = "connecting to " + chat__sanitize_string(g_host) + ":" + chat__sanitize_string("" + connection_port) + "<br>using " + ((protocol_part_of_connection_string == "wss://") ? "secure websocket (wss)" : "websocket (ws)");

    let dh_public_mix = keys__get_public_mix().toString();
    connection_crypto__begin(identity.public_key_string, dh_public_mix);

    let message_object = {
        message: {
            type: "public_key_info",
            value: identity.public_key_string,
            verification_string: VERIFICATION_MESSAGE,
            crypto_version: 2,
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
    return true;
}

/**
 * @brief sends an encrypted frame through the socket worker and counts its bytes
 *
 * @param string data_to_send -> encrypted frame to send
 *
 * @return void
 */
function connection__websocket_worker_send(data_to_send)
{
    if (data_to_send != null && data_to_send.length > 0)
    {
        g_session.bytes_sent += data_to_send.length;
    }

    g_websocket_worker.postMessage({
        type: "send",
        connection_generation: g_worker_connection_generation,
        value: data_to_send
    });
}

/**
 * @brief cancels a pending upload chunk
 */
function connection__cancel_upload_send()
{
    for (const finish of Array.from(connection_local_uploads.values()))
    {
        finish(false);
    }
    if (pending_upload_send_cancel != null)
    {
        pending_upload_send_cancel();
    }
    if (g_websocket_worker != null)
    {
        g_websocket_worker.postMessage(
        {
            type: "cancel_upload",
            connection_generation: g_worker_connection_active ? g_worker_connection_generation : undefined
        });
    }
}

/**
 * @brief sends an upload chunk when the socket has capacity
 *
 * @param object message_object -> file upload message
 *
 * @return Promise resolving to true when queued or false when cancelled
 */
function connection__send_upload_message(message_object)
{
    if (android_host__is_ui_only_runtime())
    {
        if (connection_local_uploads.size > 0 || g_worker_connection_ended)
        {
            return Promise.resolve(false);
        }
        const request_id = String(++connection_local_request_sequence);
        const generation = g_worker_connection_generation;
        return new Promise(function(resolve)
        {
            const finish = function(accepted)
            {
                if (!connection_local_uploads.delete(request_id))
                {
                    return;
                }
                clearTimeout(timer);
                resolve(accepted && generation === g_worker_connection_generation && !g_worker_connection_ended);
            };
            const timer = setTimeout(function()
            {
                finish(false);
            }, 125000);
            connection_local_uploads.set(request_id, finish);
            connection__send_message_object(
            {
                message:
                {
                    type: "local_upload_request",
                    request_id: request_id,
                    request: message_object
                }
            });
        });
    }
    if (pending_upload_send_cancel != null)
    {
        return Promise.resolve(false);
    }
    const data = keys__encrypt_all_message_data_and_convert_to_base64(connection__process_message_before_sending(message_object));
    const generation = g_worker_connection_generation;
    return new Promise(function(resolve)
    {
        let channel;
        if (typeof process !== "undefined")
        {
            channel =
            {
                port1:
                {
                    onmessage: null,
                    close: function()
                    {
                        channel.port1.onmessage = null;
                    }
                },
                port2:
                {
                    postMessage: function(data)
                    {
                        if (channel.port1.onmessage != null)
                        {
                            channel.port1.onmessage({data: data});
                        }
                    },
                    close: function()
                    {
                    }
                }
            };
        }
        else
        {
            channel = new MessageChannel();
        }
        let finished = false;
        const finish = function(sent)
        {
            if (finished)
            {
                return;
            }
            finished = true;
            clearTimeout(timer);
            channel.port1.close();
            pending_upload_send_cancel = null;
            const accepted = sent && generation === g_worker_connection_generation && !g_worker_connection_ended;
            if (accepted)
            {
                g_session.bytes_sent += data.length;
            }
            resolve(accepted);
        };
        const timer = setTimeout(function()
        {
            connection__cancel_upload_send();
        }, 125000);
        pending_upload_send_cancel = function()
        {
            finish(false);
        };
        channel.port1.onmessage = function(event)
        {
            finish(event.data.sent === true);
        };
        try
        {
            g_websocket_worker.postMessage(
            {
                type: "send",
                value: data,
                connection_generation: generation,
                reply_port: channel.port2
            }, [channel.port2]);
        }
        catch (error)
        {
            channel.port2.close();
            finish(false);
        }
    });
}

/**
 * @brief serializes, encrypts and sends a client message
 *
 * @param object message_object -> message to send
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
 * @brief logs the outgoing message type and serializes its body
 *
 * @param object message_object -> message to serialize
 *
 * @return string serialized message
 */
function connection__process_message_before_sending(message_object)
{
    let outgoing_type = message_object.message.type;

    if (outgoing_type != "ice_candidate" && outgoing_type != "sdp_answer" && outgoing_type != "client_connection_check" && outgoing_type != "create_new_webrtc_datachannel_connection")
    {
        utils__custom_log('[S] msg.message.type : ' + outgoing_type);
    }
    return JSON.stringify(message_object);
}

/**
 * @brief checks whether the theme uses a connect dialog instead of a page
 *
 * @return boolean true when the active theme uses a separate connect dialog
 */
function connection__is_connect_window_mode()
{
    if (!G_HAS_DOM || g_is_running_in_android_webview == true || typeof document === "undefined" || typeof document.getElementsByClassName !== "function")
    {
        return false;
    }

    let theme = connection__current_theme_name();

    return (theme !== "bluebell");
}

/**
 * @brief reads the active theme name
 *
 * @return string theme name or default when no active sheet is found
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
 * @brief shows the connect dialog
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
 * @brief hides the connect dialog without interrupting its connection
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
 * @brief updates the body flag used to style a hidden connected interface
 *
 * @param is_hidden true when the connected interface is hidden
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
 * @brief applies the theme's connect dialog or page layout
 *
 * @return void
 */
function connection__apply_connect_window_mode()
{
    if (!G_HAS_DOM)
    {
        return;
    }

    let box = (typeof document !== "undefined") ? document.getElementById("container-2") : null;
    let page = (typeof document !== "undefined") ? document.getElementById("verification-system") : null;

    if (box == null || page == null || typeof box.setAttribute !== "function")
    {
        return;
    }

    box.setAttribute("data-recenter-on-open", "false");

    // show saved servers in the form only when no bookmarks menu is used

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

    // keep the dialog above its following spacer

    box.style.zIndex = "5";

    if (page.style.display == "none")
    {
        return;
    }

    connection__show_predefined_details_in_connect_window();
    connection__refresh_connect_button_state();
    connection__refresh_connect_window_sections();

    // wait for page dimensions before centering
    if (window.innerWidth == 0 || window.innerHeight == 0 || box.offsetWidth == 0)
    {
        window.setTimeout(connection__apply_connect_window_mode, 250);
        return;
    }

    // center the dialog before allowing it to be dragged
    box.style.position = "fixed";
    box.style.margin = "0";
    box.style.left = Math.max(0, Math.round((utils__viewport_css_width() - box.offsetWidth) / 2)) + "px";
    box.style.top = Math.max(0, Math.round((utils__viewport_css_height() - box.offsetHeight) / 2)) + "px";
}

/**
 * @brief shows configured server details and editable keys in the connect dialog
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

    // fill configured server fields once so later edits remain intact

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

    // build editable rows for the configured keys once

    if (keys_grid.getAttribute("data-written-in-keys-built") !== "true" && typeof UI !== "undefined" && typeof UI.add_key_row === "function")
    {
        if (Array.isArray(g_autoconnect_details.keys))
        {
            for (let i = 0; i < g_autoconnect_details.keys.length; i++)
            {
                let entry = connection_crypto__password_entry(g_autoconnect_details.keys[i]);
                UI.add_key_row(entry.password, entry.type);
            }
        }

        keys_grid.setAttribute("data-written-in-keys-built", "true");
    }

    // show Connect after the user disconnects

    document.getElementById("connect-button").style.visibility = "visible";
    document.getElementById("add-key-button").style.visibility = "visible";
}

/**
 * @brief shows the first twelve hexadecimal digits of the public key fingerprint
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
 * @brief disconnects and stops retries until another connection is requested
 *
 * @return void
 */
function connection__disconnect_by_user()
{
    if (android_host__is_ui_only_runtime())
    {
        connection_ui_connect_pending = false;
        const native_disconnect = typeof Android !== "undefined" && Android != null && typeof Android.JavaExportDisconnect === "function";
        connection_ui_disconnect_pending = !native_disconnect;
        if (native_disconnect)
        {
            Android.JavaExportDisconnect();
        }
        connection__flush_local_connect_intent();
        return;
    }
    connection__invalidate_pending_attempts();
    if (typeof process !== "undefined")
    {
        g_node_connection_wanted = false;
    }
    if (G_HAS_DOM)
    {
        utils__hide_custom_alert("", "fast-reconnect");
    }
    is_connection_ended_by_user = true;
    is_connect_loop_wake_pending = false;
    g_last_disconnect_reason = "disconnected";

    if (g_fast_reconnect.in_progress || g_fast_reconnect.resumed)
    {
        // cancel recovery before its deadline can report another loss

        g_is_authenticated = false;
        connection__reset_chat_app_keep_identity();
        connection__show_connect_window();
        // end the retry delay without requesting a connection

        if (connect_loop_wake_resolver != null)
        {
            let resolver = connect_loop_wake_resolver;
            connect_loop_wake_resolver = null;
            resolver();
        }
        return;
    }

    // show the connect window before closing the socket
    if (connection__is_connect_window_mode() == true)
    {
        connection__show_connect_window();
    }

    if (g_is_authenticated == true || g_connection_status.state === "connecting")
    {
        // let the socket close handler reset the session

        g_websocket_worker.postMessage({ type: "close" });
        return;
    }

    // wake the loop so it observes the disconnected state
    connection__wake_connect_loop();
}

/**
 * @brief reads server bookmarks from local storage
 *
 * @return array saved bookmarks or an empty array
 */
function connection__read_server_bookmarks()
{
    try
    {
        let stored = JSON.parse(utils__storage_get("lemon_server_bookmarks"));
        return Array.isArray(stored) ? stored : [];
    }
    catch (e)
    {
        return [];
    }
}

/**
 * @brief writes server bookmarks to local storage
 *
 * @param array bookmarks -> server names, hosts and ports to save
 *
 * @return void
 */
function connection__write_server_bookmarks(bookmarks)
{
    utils__storage_set("lemon_server_bookmarks", JSON.stringify(bookmarks));
}

/**
 * @brief updates the server bookmark dropdown
 *
 * @return void
 */
function connection__render_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null)
    {
        return;
    }

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

    // refresh the bookmarks menu when available
    if (typeof head_menu__render_bookmarks === "function")
    {
        head_menu__render_bookmarks();
    }
}

/**
 * @brief saves or replaces a named server bookmark
 *
 * @param string name -> bookmark name
 * @param string host -> server address
 * @param string port -> server port
 *
 * @return boolean true when the bookmark was saved
 */
function connection__save_server_bookmark(name, host, port)
{
    let bookmarks = connection__read_server_bookmarks();
    let existing = -1;

    name = (name || "").trim();
    host = (host || "").trim();
    port = (port || "").trim();

    if (name.length === 0)
    {
        utils__custom_alert("give this server a name first");
        return false;
    }
    if (host.length === 0 || port.length === 0)
    {
        utils__custom_alert("fill in the address and port first");
        return false;
    }

    for (let x = 0; x < bookmarks.length; x++)
    {
        if (bookmarks[x].name.toLowerCase() === name.toLowerCase())
        {
            existing = x;
        }
    }

    if (existing === -1)
    {
        bookmarks.push({ name: name, host: host, port: port });
    }
    else
    {
        bookmarks[existing] = { name: name, host: host, port: port };
    }

    connection__write_server_bookmarks(bookmarks);
    connection__render_server_bookmarks();
    return true;
}

/**
 * @brief connects the bookmark dropdown and buttons to their actions
 *
 * @return void
 */
function connection__wire_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null)
    {
        return;
    }

    connection__render_server_bookmarks();

    // load the selected server into the form
    select_element.onchange = function()
    {
        if (this.value === "")
        {
            return;
        }

        let bookmark = connection__read_server_bookmarks()[parseInt(this.value)];

        if (bookmark == null)
        {
            return;
        }

        document.getElementById("input-ip-address").value = bookmark.host;
        document.getElementById("input-port-number").value = bookmark.port;
        document.getElementById("server-bookmark-name").value = bookmark.name;
    };

    // replace an existing bookmark with the same name
    document.getElementById("server-bookmark-save").onclick = function()
    {
        connection__save_server_bookmark(document.getElementById("server-bookmark-name").value, document.getElementById("input-ip-address").value, document.getElementById("input-port-number").value);
    };

    document.getElementById("server-bookmark-delete").onclick = function()
    {
        let selected = document.getElementById("server-bookmark-select").value;

        if (selected === "")
        {
            utils__custom_alert("pick a saved server to delete");
            return;
        }

        let bookmarks = connection__read_server_bookmarks();
        bookmarks.splice(parseInt(selected), 1);

        connection__write_server_bookmarks(bookmarks);
        connection__render_server_bookmarks();
        document.getElementById("server-bookmark-name").value = "";
    };
}
