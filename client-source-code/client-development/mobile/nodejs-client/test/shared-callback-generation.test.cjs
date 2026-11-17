"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const EventEmitter = require("node:events");
const root = path.resolve(__dirname, "../../../browser/main/scripts/app");

function noop()
{
}

function extract(relative, name)
{
    const source = fs.readFileSync(path.join(root, relative), "utf8");
    const match = source.match(new RegExp("^(?:async )?function " + name + "\\([^\\n]*\\)\\r?\\n\\{[\\s\\S]*?^\\}", "m"));
    assert.ok(match, name);
    return match[0];
}

function fixture()
{
    const timers = [];
    const sockets = [];
    const errors = [];
    const received = [];
    const resets = [];
    const context =
    {
        console:
        {
            log: noop,
            warn: noop,
            error(message)
            {
                errors.push(message);
            }
        },
        setTimeout(callback)
        {
            timers.push(callback);
            return timers.length;
        },
        setInterval: noop,
        clearTimeout: noop,
        node_dispatch_generation: null,
        node_dispatch_frames: null,
        dispatch_failed_deliveries: new Set(),
        dispatch_accepted_deliveries: new Set(),
        dispatch_processed_deliveries: new Set(),
        websocket_upload_timer: null,
        websocket_upload_pending: null,
        websocket_instance: null,
        websocket_generation: 0,
        websocket_data_port: null,
        websocket_connection_generation: 0,
        g_worker_connection_generation: 0,
        g_worker_connection_ended: false,
        g_node_message_listeners: [],
        g_node_frame_listener: null,
        DBG_WORKER_BOOT_LOG: false,
        g_session: { bytes_received: 0 },
        g_is_authenticated: true,
        g_is_identity_switch_in_progress: false,
        g_is_running_in_android_webview: true,
        g_last_disconnect_reason: "",
        g_connection_status: {},
        g_fast_reconnect: { in_progress: false },
        G_SERVER_CLOSE_KICKED: 4001,
        G_SERVER_CLOSE_BANNED: 4002,
        is_connection_ended_by_user: false,
        document:
        {
            getElementById()
            {
                return { value: "", style: {} };
            }
        },
        connection__connect_loop: noop,
        connection__play_disconnect_sound: noop,
        connection__signal_connection_closed: noop,
        audio_transport__reset: noop,
        utils__custom_log: noop,
        connection__try_start_fast_reconnect()
        {
            return false;
        },
        android_host__is_ui_only_runtime()
        {
            return false;
        },
        connection__reset_chat_app_keep_identity()
        {
            resets.push("reset");
            context.g_websocket_worker.postMessage({ type: "close" });
        },
        workers__data_processing_worker_onmessage(event)
        {
            if (event.data.type === "mainthread__process_received_websocket_message")
            {
                received.push(event.data.value);
            }
        },
        WebSocket: class
        {
            constructor(url)
            {
                this.url = url;
                this.closeCalls = 0;
                this.readyState = 1;
                this.listeners = {};
                sockets.push(this);
            }

            addEventListener(name, callback)
            {
                this.listeners[name] = callback;
            }

            close()
            {
                this.closeCalls++;
            }

            send()
            {
            }
        }
    };
    vm.createContext(context);
    vm.runInContext("var global = globalThis;\n" + [
        ["mobile/android-host.js", "android_host__dispatch_safely"],
        ["mobile/android-host.js", "android_host__make_loopback_worker"],
        ["mobile/android-host.js", "android_host__make_discarding_worker"],
        ["mobile/android-host.js", "android_host__init_node_runtime"],
        ["workers/workers.js", "workers__deliver_socket_event"],
        ["workers/workers.js", "workers__finish_upload_send"],
        ["workers/workers.js", "workers__websocket_worker_onmessage"],
        ["msg/dispatch.js", "dispatch__mainthread_onmessage"],
        ["msg/dispatch.js", "dispatch__mainthread_onmessage_impl"]
    ].map(function(entry)
    {
        return extract(entry[0], entry[1]);
    }).join("\n"), context);
    context.android_host__init_node_runtime();

    function drain()
    {
        let count = 0;
        while (timers.length > 0)
        {
            assert.ok(count++ < 20, "bounded timer queue");
            timers.shift()();
        }
        assert.deepEqual(errors, []);
    }

    function open(label, port)
    {
        context.g_worker_connection_generation++;
        context.g_websocket_worker.postMessage(
        {
            type: "create_websocket_object",
            connection_generation: context.g_worker_connection_generation,
            port: port || null,
            value: { connection_string: "ws://" + label + ".invalid/", onopen_data: "synthetic" }
        });
    }

    drain();
    open("a");
    drain();
    return { context, sockets, timers, received, resets, open, drain };
}

{
    const f = fixture();
    f.open("b");
    f.sockets[0].onclose({ code: 1000, reason: "", wasClean: true });
    f.drain();
    assert.equal(f.sockets.length, 2);
    assert.equal(f.resets.length, 0);
    assert.equal(f.sockets[1].closeCalls, 0);
    console.log("PASS: queued A close cannot reset or close replacement B.");
}

{
    const f = fixture();
    f.open("b");
    f.sockets[0].listeners.message({ data: "synthetic-frame-from-a" });
    f.drain();
    assert.deepEqual(f.received, []);
    assert.equal(f.context.websocket_instance, f.sockets[1]);
    console.log("PASS: queued A input is rejected after B replaces A.");
}

{
    const f = fixture();
    f.open("b");
    f.drain();
    f.sockets[0].onclose({ code: 1000, reason: "", wasClean: true });
    f.sockets[0].listeners.message({ data: "late-a" });
    f.drain();
    assert.deepEqual(f.resets, []);
    assert.deepEqual(f.received, []);
    assert.equal(f.sockets[1].closeCalls, 0);
    console.log("CONTROL: A callbacks first observed after replacement are rejected by websocket_generation.");
}

{
    const f = fixture();
    f.sockets[0].listeners.message({ data: "current-a" });
    f.drain();
    assert.deepEqual(f.received, ["current-a"]);
    console.log("CONTROL: the current Node socket delivers its input normally.");
}

{
    const f = fixture();
    const published = [];
    f.context.websocket_data_port =
    {
        postMessage(message)
        {
            published.push(message);
        }
    };
    f.context.websocket_connection_generation = 1;
    f.sockets[0].onclose({ code: 1000, reason: "", wasClean: true });
    assert.equal(published.length, 1);
    assert.equal(published[0].connection_generation, 1);
    f.context.g_worker_connection_generation = 2;
    f.context.dispatch__mainthread_onmessage({ data: published[0] });
    assert.deepEqual(f.resets, []);
    console.log("CONTROL: the MessagePort branch tags old output and the dispatcher rejects it after generation advances.");
}

