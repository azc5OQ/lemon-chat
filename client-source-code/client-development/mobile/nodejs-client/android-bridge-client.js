// this file connects the chat client in Node.js to the Java part of the Android app.
// the connection is a network connection inside the phone (127.0.0.1). its port and password (token) are in bridge.json.
// the Java part sends one JSON command per line, for example which server to join or whether file logging is on.
// the client sends its state back the same way, for example whether it is connected and how many messages are unread.
// each connection keeps its own received text, its waiting replies and the text it still has to send
const net = require("net");
const fs = require("fs");
const path = require("path");
const TextDecoder = require("util").TextDecoder;
const limits = require("./transport-limits.js");

/**
 * @brief reads bounded UTF8 control lines from socket chunks
 *
 * @param on_line callback for each complete nonempty line
 *
 * @return chunk reader that throws for invalid UTF8 or oversized lines
 */
function create_line_reader(on_line)
{
    const buffer = Buffer.allocUnsafe(limits.controlLineBytes);
    let bytes = 0;
    const decoder = new TextDecoder("utf-8", { fatal: true });
    return function(data)
    {
        let start = 0;
        while (start < data.length)
        {
            const newline = data.indexOf(10, start);
            const end = newline < 0 ? data.length : newline;
            const part = data.subarray(start, end);
            bytes += part.length;
            if (bytes > limits.controlLineBytes)
            {
                throw new RangeError("Bridge control line exceeds capacity");
            }
            part.copy(buffer, bytes - part.length);
            if (newline < 0)
            {
                return;
            }
            const line = decoder.decode(buffer.subarray(0, bytes));
            bytes = 0;
            if (line.trim().length > 0)
            {
                on_line(line);
            }
            start = newline + 1;
        }
    };
}

/**
 * @brief checks the fields allowed in a native command
 *
 * @param command parsed command to validate
 *
 * @return true when the command has a supported type and valid fields
 */
function valid_command(command)
{
    if (command === null || typeof command !== "object" || Array.isArray(command))
    {
        return false;
    }
    switch (command.type)
    {
        case "settings":
            return typeof command.json === "string";
        case "connect":
        case "disconnect":
            if (typeof command.command_id !== "string")
            {
                return false;
            }
            if (command.command_id.length === 0 || command.command_id.length > 128)
            {
                return false;
            }
            return true;
        case "ui_visible":
            if (typeof command.visible !== "boolean")
            {
                return false;
            }
            if (command.preserve_idle !== undefined)
            {
                if (typeof command.preserve_idle !== "boolean")
                {
                    return false;
                }
            }
            if (command.force_idle !== undefined)
            {
                if (typeof command.force_idle !== "boolean")
                {
                    return false;
                }
            }
            if (command.force_idle === true)
            {
                if (command.visible === true || command.preserve_idle === true)
                {
                    return false;
                }
            }
            return true;
        case "network":
            return typeof command.available === "boolean";
        case "log_enabled":
            return typeof command.enabled === "boolean";
        case "log":
            if (typeof command.line !== "string")
            {
                return false;
            }
            if (command.line.length > 8192)
            {
                return false;
            }
            return true;
        case "call_action":
        case "come_from_idle":
            if (typeof command.call_id !== "string")
            {
                return false;
            }
            if (!/^[a-f0-9]{32}$/.test(command.call_id))
            {
                return false;
            }
            if (typeof command.session_id !== "string")
            {
                return false;
            }
            if (command.session_id.length === 0 || command.session_id.length > 128)
            {
                return false;
            }
            if (command.action === "accept")
            {
                return true;
            }
            else if (command.action === "decline")
            {
                return true;
            }
            else if (command.type === "come_from_idle")
            {
                return true;
            }
            return false;
        default:
            return false;
    }
}

/**
 * @brief connects the Node client to the native control service
 *
 * @param bundle shared client exports
 * @param initial_loopback_info local page endpoint when already available
 * @param options optional bridge configuration path and retry delay
 *
 * @return control handle for endpoint updates and shutdown
 */
function start(bundle, initial_loopback_info, options)
{
    options = options || {};
    let current = null;
    let retry_timer = null;
    let debounce_timer = null;
    let stopped = false;
    let loopback_info = initial_loopback_info || null;
    let unread_total = 0;
    let phase = null;
    let pending_call = null;
    let previous_state = "";
    let last_connect_intent = null;
    const pending_notifications = [];

    /**
     * @brief queues an event for the current native control connection
     *
     * @param object event -> native event to serialize
     *
     * @return boolean -> true when the event entered the bounded queue
     */
    function send_event(event)
    {
        const owner = current;
        if (owner === null || owner.connected !== true || owner.socket.destroyed)
        {
            return false;
        }
        const line = JSON.stringify(event) + "\n";
        const bytes = Buffer.byteLength(line, "utf8");
        if (bytes > limits.controlLineBytes || owner.bytes + owner.socket.writableLength + bytes > limits.controlQueueBytes)
        {
            owner.socket.destroy();
            return false;
        }
        owner.queue.push({ line, bytes });
        owner.bytes += bytes;
        flush(owner);
        return true;
    }

    /**
     * @brief writes queued control events until the socket needs to drain
     *
     * @param object owner -> control socket owner
     *
     * @return void
     */
    function flush(owner)
    {
        if (owner !== current || owner.blocked || owner.socket.destroyed)
        {
            return;
        }
        while (owner.queue.length > 0)
        {
            const item = owner.queue.shift();
            owner.bytes -= item.bytes;
            if (owner.socket.write(item.line) === false)
            {
                owner.blocked = true;
                owner.write_timer = setTimeout(function()
                {
                    if (current === owner && owner.blocked)
                    {
                        owner.socket.destroy();
                    }
                }, limits.controlWriteTimeoutMs);
                return;
            }
        }
    }

    /**
     * @brief reports connection and member state when it changes
     *
     * @param boolean force -> true to report even when the state signature is unchanged
     *
     * @return void
     */
    function report_state(force)
    {
        const state = bundle.read_state();
        const authenticated = typeof bundle.get_ui_session_snapshot === "function" ? bundle.get_ui_session_snapshot().authenticated === true : state.g_is_authenticated === true;
        const status = {
            type: "status",
            connected: authenticated,
            logged_in: authenticated && typeof state.g_local_username === "string" && state.g_local_username.length > 0,
            username: state.g_local_username || ""
        };
        const members = state.g_client_list.map(function(client)
        {
            return client.client_id + ":" + client.username;
        }).join(",");
        const signature = JSON.stringify([status, members]);
        if (force || previous_state !== signature)
        {
            if (send_event(status) && send_event({ type: "member_change", members }))
            {
                previous_state = signature;
            }
        }
    }

    /**
     * @brief restores endpoint, status, call and notification state after native reconnect
     *
     * @return void
     */
    function replay()
    {
        if (loopback_info !== null)
        {
            send_event({ type: "loopback", port: loopback_info.loopback_port, token: loopback_info.loopback_token });
        }
        report_state(true);
        send_event({ type: "unread", count: unread_total });
        if (phase !== null)
        {
            send_event({ type: "status_text", state: phase.state, reason: phase.reason });
        }
        const session = typeof bundle.get_ui_session_snapshot === "function" ? bundle.get_ui_session_snapshot() : null;
        const current_call = session && session.authenticated ? session.current_call : null;
        if (current_call && current_call.expires_at > Date.now())
        {
            pending_call = Object.assign({}, current_call, { type: "call", caller: current_call.caller_username });
            send_event(pending_call);
        }
        while (pending_notifications.length > 0)
        {
            if (session && pending_notifications[0].session_id !== session.session_id)
            {
                pending_notifications.shift();
                continue;
            }
            if (!send_event(pending_notifications[0]))
            {
                break;
            }
            pending_notifications.shift();
        }
    }

    bundle.set_on_message_processed(function()
    {
        clearTimeout(debounce_timer);
        debounce_timer = setTimeout(function()
        {
            report_state(false);
        }, 300);
    });
    if (typeof bundle.set_connection_status_listener === "function")
    {
        bundle.set_connection_status_listener(function(status)
        {
            phase = { state: status.state, reason: status.reason };
            send_event({ type: "status_text", state: phase.state, reason: phase.reason });
        });
    }
    if (typeof bundle.set_unread_listener === "function")
    {
        bundle.set_unread_listener(function(total)
        {
            unread_total = total;
            send_event({ type: "unread", count: total });
        });
    }
    if (typeof bundle.set_incoming_call_listener === "function")
    {
        bundle.set_incoming_call_listener(function(caller, channel_id, record)
        {
            if (!record || typeof record.call_id !== "string")
            {
                return;
            }
            pending_call = Object.assign({}, record, { type: "call", caller, channel_id });
            send_event(pending_call);
        });
    }
    if (typeof bundle.set_notification_listener === "function")
    {
        bundle.set_notification_listener(function(event)
        {
            if (!send_event(event))
            {
                // keep recent notification events for the next native control connection
                if (pending_notifications.length === 256)
                {
                    pending_notifications.shift();
                }
                pending_notifications.push(event);
            }
        });
    }

    /**
     * @brief validates and applies a command from the current native socket
     *
     * @param object owner -> control socket owner
     * @param string line -> complete JSON command line
     *
     * @return void
     */
    function handle_line(owner, line)
    {
        if (owner !== current || owner.socket.destroyed)
        {
            return;
        }
        try
        {
            const command = JSON.parse(line);
            if (!valid_command(command))
            {
                console.warn("bridge: rejected invalid command");
                return;
            }
            switch (command.type)
            {
                case "settings":
                    bundle.android_js_bridge.accept_current_settings_from_android(command.json);
                    break;
                case "connect":
                case "disconnect":
                {
                    if (last_connect_intent == null || last_connect_intent.command_id !== command.command_id)
                    {
                        const accepted = command.type === "connect" ? bundle.node_request_connect() === true : bundle.node_disconnect_by_user() === true;
                        last_connect_intent = { command_id: command.command_id, action: command.type, accepted };
                    }
                    send_event(
                    {
                        type: "connect_intent_result",
                        command_id: command.command_id,
                        accepted: last_connect_intent.action === command.type && last_connect_intent.accepted
                    });
                    break;
                }
                case "call_action":
                case "come_from_idle":
                    const accepted = bundle.node_handle_call_action(command.type === "come_from_idle" || command.action === "accept" ? "accept_call" : "decline_call", command) === true;
                    if (accepted)
                    {
                        pending_call = null;
                    }
                    send_event(
                    {
                        type: "call_action_result",
                        call_id: command.call_id,
                        session_id: command.session_id,
                        accepted
                    });
                    break;
                case "ui_visible":
                    bundle.node_set_ui_attached(command.visible, true, command.preserve_idle === true, command.force_idle === true);
                    break;
                case "network":
                    bundle.node_set_device_network(command.available);
                    break;
                case "log_enabled":
                    if (typeof global.lemonchat_set_file_logging === "function")
                    {
                        global.lemonchat_set_file_logging(command.enabled);
                    }
                    break;
                case "log":
                    // keep console output out of native events
                    break;
            }
        }
        catch (error)
        {
            // log failures without received data or exception details
            console.warn("bridge: command rejected or handler failed");
        }
    }

    /**
     * @brief schedules one native control reconnect while the bridge is running
     *
     * @return void
     */
    function schedule_retry()
    {
        if (!stopped && retry_timer === null)
        {
            retry_timer = setTimeout(function()
            {
                retry_timer = null;
                connect();
            }, options.retry_ms || 3000);
        }
    }

    /**
     * @brief opens a scoped native control socket and installs its callbacks
     *
     * @return void
     */
    function connect()
    {
        if (stopped || current !== null)
        {
            return;
        }
        let config;
        try
        {
            config = JSON.parse(fs.readFileSync(options.config_path || path.join(__dirname, "bridge.json"), "utf8"));
            if (!Number.isInteger(config.port) || config.port < 1 || config.port > 65535 || typeof config.token !== "string" || !/^[a-f0-9]{32,128}$/i.test(config.token))
            {
                throw new Error("Invalid bridge configuration");
            }
        }
        catch (error)
        {
            console.warn("bridge: configuration unavailable; retrying");
            schedule_retry();
            return;
        }
        const socket = net.connect({ host: "127.0.0.1", port: config.port });
        const owner = { socket, connected: false, queue: [], bytes: 0, blocked: false, write_timer: null };
        current = owner;
        const read = create_line_reader(function(line)
        {
            handle_line(owner, line);
        });
        const connect_timer = setTimeout(function()
        {
            if (current === owner && !owner.connected)
            {
                socket.destroy();
            }
        }, limits.handshakeTimeoutMs);
        socket.on("connect", function()
        {
            if (current !== owner)
            {
                socket.destroy();
                return;
            }
            clearTimeout(connect_timer);
            owner.connected = true;
            socket.setNoDelay(true);
            socket.write(config.token + "\n");
            replay();
        });
        socket.on("data", function(data)
        {
            if (current !== owner)
            {
                return;
            }
            try
            {
                read(data);
            }
            catch (error)
            {
                console.warn("bridge: invalid or oversized UTF-8 control line");
                socket.destroy();
            }
        });
        socket.on("drain", function()
        {
            clearTimeout(owner.write_timer);
            owner.blocked = false;
            flush(owner);
        });
        socket.on("error", function()
        {
            console.warn("bridge: transport failure");
        });
        socket.on("close", function()
        {
            clearTimeout(connect_timer);
            clearTimeout(owner.write_timer);
            owner.queue = [];
            if (current === owner)
            {
                current = null;
                previous_state = "";
                schedule_retry();
            }
        });
    }

    connect();
    return {
        /**
         * @brief stores and announces the current local page endpoint
         *
         * @param object info -> loopback port and authentication token
         *
         * @return void
         */
        set_loopback_info: function(info)
        {
            loopback_info = info;
            send_event({ type: "loopback", port: info.loopback_port, token: info.loopback_token });
        },
        /**
         * @brief stops control retries and closes the active native socket
         *
         * @return void
         */
        stop: function()
        {
            stopped = true;
            clearTimeout(retry_timer);
            clearTimeout(debounce_timer);
            if (current !== null)
            {
                current.socket.destroy();
            }
        }
    };
}

module.exports = { start, create_line_reader, valid_command };
