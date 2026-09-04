            // connection-driver.js is the connection driver, the one place that decides when to dial
            // three questions have three owners: who am i is answered by is_ui_only_runtime() (fixed
            // at load), what identity by g_identity_slot (set by the keygen result), and where to by
            // g_target_slot (set by settings, page config or the connect button)
            // writers write and the driver awaits, so nothing polls a flag before it is set, no gate
            // fails silently, and only one attempt can ever be in flight

            // a slot holds one async value: set() stores it and wakes everyone who called wait()
            function make_slot()
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

            // where to connect, as { kind: "loopback", port, token } or { kind: "server", host, port, wss_port }
            var g_target_slot = make_slot();

            // the local keypair as { public_key_string, identity_string }. the private key stays in the worker
            var g_identity_slot = make_slot();

            // one keygen per passphrase, because a repeat request for the same identity must be a no-op.
            // the size is part of the key: the same passphrase at a new size is a different identity,
            // so it has to be part of what makes a request a repeat
            var g_identity_requested_for = null;
            var g_identity_requested_bits = 0;

            function request_identity(passphrase_or_null)
            {
                let requested_key = (typeof passphrase_or_null === "string" && passphrase_or_null.length >= 199)
                    ? passphrase_or_null : "(random)";

                if (g_identity_requested_for === requested_key && g_identity_requested_bits === g_rsa_key_bits)
                {
                    return;
                }

                // settings can arrive before the workers exist; the next settings push retries this
                if (g_data_processing_worker == null)
                {
                    console.log("connect-path: no worker yet, keypair request dropped");
                    return;
                }

                g_identity_requested_for = requested_key;
                g_identity_requested_bits = g_rsa_key_bits;
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
                report_connection_status("connecting", "generating the identity key (takes a while on first start)");
            }

            // resolved by the websocket close and error handlers. the driver awaits this while a
            // socket exists, whether it is mid-handshake or long connected
            var g_connection_closed_resolvers = [];

            function signal_connection_closed()
            {
                let woken = g_connection_closed_resolvers;
                g_connection_closed_resolvers = [];

                for (let i = 0; i < woken.length; i++)
                {
                    woken[i]();
                }
            }

            function connection_closed()
            {
                return new Promise(function(resolve) { g_connection_closed_resolvers.push(resolve); });
            }

            // skips the retry countdown, used when the connect button is pressed or new details arrive.
            // a nudge that lands while the driver is still watching the old socket is kept, not lost:
            // an identity switch closes the socket and nudges in the same breath, before the close is seen
            var g_driver_nudge_resolver = null;
            var g_is_driver_nudge_pending = false;

            function nudge_connection_driver()
            {
                if (g_driver_nudge_resolver != null)
                {
                    let resolver = g_driver_nudge_resolver;
                    g_driver_nudge_resolver = null;
                    resolver();
                    return;
                }

                g_is_driver_nudge_pending = true;
            }

            // the countdown between retries. it shows the failure text and leaves early on a nudge
            async function driver_retry_wait(seconds)
            {
                if (g_is_driver_nudge_pending == true)
                {
                    g_is_driver_nudge_pending = false;
                    console.log("connect-path: retry wait skipped by a pending nudge");
                    return;
                }

                let nudged = new Promise(function(resolve) { g_driver_nudge_resolver = resolve; });

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

                    let done = await Promise.race([sleep(1000).then(function() { return false; }), nudged.then(function() { return true; })]);

                    if (done == true)
                    {
                        console.log("connect-path: retry wait skipped by nudge");
                        break;
                    }
                }

                g_driver_nudge_resolver = null;
            }

            var g_is_connection_driver_running = false;

            async function connection_driver()
            {
                if (g_is_connection_driver_running == true)
                {
                    return;
                }

                g_is_connection_driver_running = true;
                console.log("connect-path: driver started");

                while (true)
                {
                    let target = await g_target_slot.wait();

                    // the node host parks the connection during the idle handover
                    if (g_node_connection_wanted == false)
                    {
                        await sleep(1000);
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

                    let closed = connection_closed();

                    g_last_disconnect_reason = "";
                    report_connection_status("connecting");

                    // a dial consumes any nudge; one kept from before must not skip a later countdown
                    g_is_driver_nudge_pending = false;
                    await attempt_connection(target, identity);

                    // wait for the socket to die. a dial that could not even create a socket never
                    // fires close, so an unauthenticated wait also has a deadline
                    while (true)
                    {
                        let outcome = await Promise.race([
                            closed.then(function() { return "closed"; }),
                            sleep(45000).then(function() { return "deadline"; })
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
                    if (g_is_authenticated == false && g_is_fast_reconnect_in_progress == true)
                    {
                        continue;
                    }

                    if (g_is_authenticated == false
                        && (g_is_reconnect_active == false || g_is_autoconnect_without_user_action_active == false))
                    {
                        // manual mode means one attempt per button press: state the failure and
                        // hold until the connect button nudges, with no countdown
                        if (g_is_driver_nudge_pending == true)
                        {
                            g_is_driver_nudge_pending = false;
                            console.log("connect-path: dialing on the pending nudge");
                            continue;
                        }

                        set_connect_button_pending(false);
                        report_connection_status("idle",
                            (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
                        await new Promise(function(resolve) { g_driver_nudge_resolver = resolve; });
                        continue;
                    }

                    // dialing node on this device is free, so the loopback redials fast; the 30s
                    // pace is for a remote server that may be down
                    let retry_seconds = (target.kind === "loopback") ? 2 : 30;

                    if (g_is_authenticated == false)
                    {
                        g_connection_status.next_retry_at = new Date().valueOf() + retry_seconds * 1000;
                        report_connection_status("waiting_retry",
                            (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
                    }

                    await driver_retry_wait(retry_seconds);
                }
            }

            // the one place that may start a connection. every trigger routes through here and
            // this is the entire dial policy: "button" means the user asked, so it always dials
            // "attach" means a ui attached to node, which is downstream of an authorized decision,
            // so it always dials too. "settings", "resume" and "retry" dial only under autoconnect,
            // and "resume" additionally reattaches when node already holds a session, because
            // rendering it is not connecting. nothing else in the codebase may set the target or
            // nudge the driver
            function request_connect(trigger)
            {
                let is_wanted =
                    (trigger === "button")
                    || (trigger === "attach")
                    || (g_is_autoconnect_without_user_action_active == true)
                    || (g_ui_connect_requested == true)
                    || (trigger === "resume" && g_connection_status.state === "connected");

                console.log("connect-path: request_connect(" + trigger + ") -> " + (is_wanted ? "dial" : "ignored"));

                if (is_wanted == false)
                {
                    return;
                }

                // derive the target from where we run and what the page knows
                if (is_ui_only_runtime())
                {
                    if (g_loopback_port <= 0)
                    {
                        // node has not announced its port yet; the settings push that follows finishes this
                        if (trigger === "button")
                        {
                            g_ui_connect_requested = true;
                            document.getElementById("another-buttons-loading-container-p").innerHTML = "waiting for app runtime...";
                        }
                        return;
                    }

                    g_ui_connect_requested = false;
                    g_target_slot.set({ kind: "loopback", port: g_loopback_port, token: g_loopback_token });

                    // a button press keeps the page exactly as it is, only the button itself
                    // fades; every other trigger connects behind the spinner page
                    if (trigger !== "button")
                    {
                        hold_back_connect_page();
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

                    g_target_slot.set({
                        kind: "server",
                        host: g_autoconnect_details.host,
                        port: g_autoconnect_details.port,
                        wss_port: g_autoconnect_details.wss_port
                    });
                }
                else
                {
                    g_target_slot.set({
                        kind: "server",
                        host: document.getElementById("input-ip-address").value,
                        port: document.getElementById("input-port-number").value
                    });
                }

                nudge_connection_driver();
            }

            // the connect button. it keeps its own name for the onclick wiring
            function submit_connection_target_from_ui()
            {
                request_connect("button");
            }

            // webview only: true when connect was pressed before node announced its loopback port
            var g_ui_connect_requested = false;

            // the live status for the login page. the local driver writes it; in the webview,
            // node's reports arriving over the loopback overwrite it, because node owns the real connection
            var g_connection_status = {
                state: "idle",          // idle | connecting | waiting_retry | connected
                reason: "",             // why the last attempt failed, human readable
                next_retry_at: 0,       // ms timestamp of the next automatic attempt, 0 = none
                last_connected_at: 0    // ms timestamp the server connection last existed, 0 = never
            };

            // set through the export seam. every listener receives every status change; under node
            // two exist, the loopback ui and the java bridge
            var g_connection_status_listeners = [];

            // filled by the close and error handlers, consumed by the driver for the retry status
            var g_last_disconnect_reason = "";

            // android reports the device's real network state over the bridge; null means never told
            // a browser has navigator.onLine, but the android webview lies about it, so this is the
            // only trustworthy answer to whether the phone has any network
            var g_device_has_network = null;

            // turns a node socket error code into something a person can act on. the codes are the
            // difference between your wifi being off and the server rejecting you; without them an
            // unreachable network used to be reported as a login rejection
            function describe_socket_error(error_code)
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

            function report_connection_status(state, reason)
            {
                g_connection_status.state = state;
                g_connection_status.reason = (reason != null) ? reason : "";

                for (let i = 0; i < g_connection_status_listeners.length; i++)
                {
                    try { g_connection_status_listeners[i](g_connection_status); } catch (e) { }
                }

                render_connection_status();
            }

            // paints the login page extras from g_connection_status; the ticker recalls it every
            // second so the countdown and the "ago" times stay live. it is safe headless, because
            // the shim absorbs the dom
            function render_connection_status()
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

            // one repaint per second keeps the countdown and the "ago" times moving
            function start_connection_status_ticker()
            {
                window.setInterval(render_connection_status, 1000);
            }
