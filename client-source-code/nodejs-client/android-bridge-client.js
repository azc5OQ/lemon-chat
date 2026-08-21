// Connects to the java side of the service over loopback TCP and speaks JSON lines.
//
// java is the server (it writes bridge.json with {port, token} before starting node), node is the
// client. commands in: "settings" (connect with this json), "disconnect" (park the socket).
// events out: "status", "member_change", "chat_activity" - the things a notification keys off.
//
// the token is required as the first line, because 127.0.0.1 is reachable by every app on the device.

let net = require("net");
let fs = require("fs");
let path = require("path");

let BRIDGE_RETRY_MS = 3000;
let DEBOUNCE_MS = 300;

function start(bundle, loopback_info)
{
    let socket = null;
    let receive_buffer = "";
    let debounce_timer = null;

    let previous = {
        connected: false,
        logged_in: false,
        username: "",
        members: "",
        activity: ""
    };

    function send_event(event_object)
    {
        if (socket != null && socket.writable)
        {
            socket.write(JSON.stringify(event_object) + "\n");
        }
    }

    // diff state against the last report and emit what changed. force sends status and members
    // unconditionally - the hello on (re)connect, since a fresh runtime equals the reset diff state
    // and a pure diff would say nothing at all
    function report_state_changes(force)
    {
        let state = bundle.read_state();

        let connected = (state.g_is_authenticated === true);
        let logged_in = (typeof state.g_local_username === "string" && state.g_local_username.length > 0);

        if (force === true || connected !== previous.connected || logged_in !== previous.logged_in
            || state.g_local_username !== previous.username)
        {
            previous.connected = connected;
            previous.logged_in = logged_in;
            previous.username = state.g_local_username;
            send_event({ type: "status", connected: connected, logged_in: logged_in, username: state.g_local_username });
        }

        let members = state.g_client_list
            .map(function(c) { return c.client_id + ":" + c.username; })
            .join(",");

        if (force === true || members !== previous.members)
        {
            previous.members = members;
            send_event({ type: "member_change", members: members });
        }

        // unread counts plus last sender per context: enough for "new message from X"
        let activity = state.g_client_list
            .filter(function(c) { return (c.unread_count || 0) > 0; })
            .map(function(c) { return c.username + "=" + c.unread_count; })
            .join(",")
            + "|" + state.g_chat_context_array
                .map(function(c) { return c.last_known_message_sender_username || ""; })
                .join(",");

        if (activity !== previous.activity)
        {
            let is_first_report = (previous.activity === "");
            previous.activity = activity;

            // skip the first report: it is the login snapshot, not a new message
            if (is_first_report === false)
            {
                let unread = state.g_client_list
                    .filter(function(c) { return (c.unread_count || 0) > 0; })
                    .map(function(c) { return { username: c.username, unread: c.unread_count }; });

                let last_sender = "";
                for (let i = 0; i < state.g_chat_context_array.length; i++)
                {
                    if (state.g_chat_context_array[i].last_known_message_sender_username)
                    {
                        last_sender = state.g_chat_context_array[i].last_known_message_sender_username;
                    }
                }

                send_event({ type: "chat_activity", unread: unread, last_sender: last_sender });
            }
        }
    }

    bundle.set_on_message_processed(function()
    {
        if (debounce_timer != null)
        {
            clearTimeout(debounce_timer);
        }
        debounce_timer = setTimeout(report_state_changes, DEBOUNCE_MS);
    });

    // connection phase for the webview, routed through java: while node derives the identity
    // key its whole thread is frozen and the loopback cannot deliver anything
    if (typeof bundle.set_connection_status_listener === "function")
    {
        bundle.set_connection_status_listener(function(status)
        {
            send_event({ type: "status_text", state: status.state, reason: status.reason });
        });
    }

    // the launcher icon badge. node is the only side that is always alive - the webview may be
    // gone (idle, closed, or a headless boot start), which is exactly when a badge matters
    let previous_unread_total = -1;

    if (typeof bundle.set_unread_listener === "function")
    {
        bundle.set_unread_listener(function(total)
        {
            if (total === previous_unread_total)
            {
                return;
            }

            previous_unread_total = total;
            send_event({ type: "unread", count: total });
        });
    }

    // somebody is calling and there is no webview to show it, which is the case after a boot start
    if (typeof bundle.set_incoming_call_listener === "function")
    {
        bundle.set_incoming_call_listener(function(caller, channel_id)
        {
            send_event({ type: "call", caller: caller, channel_id: channel_id });
        });
    }

    function handle_command(line)
    {
        let command = null;

        try
        {
            command = JSON.parse(line);
        }
        catch (bad_json)
        {
            console.error("bridge: unparseable command: " + line);
            return;
        }

        if (command.type === "settings")
        {
            console.log("bridge: settings received, connecting");
            bundle.node_set_connection_wanted(true);
            bundle.android_js_bridge.accept_current_settings_from_android(command.json);
        }
        else if (command.type === "disconnect")
        {
            console.log("bridge: disconnect command, parking the socket");
            bundle.node_set_connection_wanted(false);
        }
        else if (command.type === "come_from_idle")
        {
            // a call was accepted. node owns the connection, so it leaves idle itself - the
            // webview may not even exist (boot start), and it is not the authority anyway
            console.log("bridge: coming back from idle into channel " + command.channel_id);
            bundle.android_js_bridge.send_come_from_idle_mode_request_android(command.channel_id);
        }
        else if (command.type === "log")
        {
            // a line the webview printed. we print it too, so both end up in the same log file
            console.log("[webview] " + command.line);
        }
        else if (command.type === "log_enabled")
        {
            if (typeof global.lemonchat_set_file_logging === "function")
            {
                global.lemonchat_set_file_logging(command.enabled === true);
            }
        }
        else if (command.type === "ui_visible")
        {
            // java telling us if the user is actually looking at the app. we cannot tell on our
            // own, because the webview stays connected even when the app is in the background
            console.log("bridge: ui " + (command.visible ? "visible" : "hidden"));

            if (typeof bundle.node_set_ui_attached === "function")
            {
                // the second argument marks java as the authority from here on
                bundle.node_set_ui_attached(command.visible === true, true);
            }
        }
        else if (command.type === "network")
        {
            // java watches connectivity for us; node cannot see it and the webview lies about it
            console.log("bridge: device network " + (command.available ? "available" : "GONE"));

            if (typeof bundle.node_set_device_network === "function")
            {
                bundle.node_set_device_network(command.available === true);
            }
        }
        else
        {
            console.error("bridge: unknown command type: " + command.type);
        }
    }

    function connect_to_bridge()
    {
        let bridge_config = null;

        try
        {
            bridge_config = JSON.parse(fs.readFileSync(path.join(__dirname, "bridge.json"), "utf8"));
        }
        catch (no_config)
        {
            console.error("bridge: cannot read bridge.json (" + (no_config.code || no_config.message) + "), retrying");
            setTimeout(connect_to_bridge, BRIDGE_RETRY_MS);
            return;
        }

        console.log("bridge: dialing 127.0.0.1:" + bridge_config.port);

        socket = net.connect({ host: "127.0.0.1", port: bridge_config.port }, function()
        {
            console.log("bridge: connected to java on port " + bridge_config.port);
            socket.write(bridge_config.token + "\n");

            // announce the loopback ui endpoint so java can point the webview at it
            if (loopback_info != null)
            {
                send_event({ type: "loopback", port: loopback_info.loopback_port, token: loopback_info.loopback_token });
            }

            // hello: send the full current state, then diff from here. activity resets too, so the
            // next real message is reported but the login snapshot is not re-announced
            previous.activity = "";
            report_state_changes(true);
        });

        socket.on("data", function(data)
        {
            receive_buffer += data.toString("utf8");

            let newline_index;
            while ((newline_index = receive_buffer.indexOf("\n")) !== -1)
            {
                let line = receive_buffer.substring(0, newline_index).trim();
                receive_buffer = receive_buffer.substring(newline_index + 1);

                if (line.length > 0)
                {
                    handle_command(line);
                }
            }
        });

        socket.on("error", function(bridge_error)
        {
            // never silent: this is the wire everything else depends on
            console.error("bridge: connect to 127.0.0.1:" + bridge_config.port + " failed: "
                + (bridge_error != null ? (bridge_error.code || bridge_error.message) : "?"));
        });

        socket.on("close", function(had_error)
        {
            console.log("bridge: socket closed (had_error=" + had_error + ", was_connecting=" + (socket != null ? socket.connecting : "?") + ")");
            socket = null;
            receive_buffer = "";
            setTimeout(connect_to_bridge, BRIDGE_RETRY_MS);
        });
    }

    connect_to_bridge();
}

module.exports = { start: start };
