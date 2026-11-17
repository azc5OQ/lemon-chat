const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const { spawnSync } = require("node:child_process");
const loopback = require("../loopback-ui-server.js");

const directory = fs.mkdtempSync(path.join(os.tmpdir(), "lemon-bundle-events-"));
const output = path.join(directory, "bundle.js");
const built = spawnSync("python", ["-c", [
    "import importlib.util,sys",
    "spec=importlib.util.spec_from_file_location('builder',sys.argv[1])",
    "builder=importlib.util.module_from_spec(spec)",
    "spec.loader.exec_module(builder)",
    "builder.OUT=sys.argv[2]",
    "sys.exit(builder.main())"
].join("\n"), path.resolve(__dirname, "../build-node.py"), output], { encoding: "utf8" });
assert.equal(built.status, 0, built.stderr || built.stdout);
let source = fs.readFileSync(output, "utf8");
fs.unlinkSync(output);
fs.rmdirSync(directory);
const anchor = "node_forward_upload_request: function(request)";
assert.ok(source.includes(anchor));
source = source.replace(anchor, `event_test_setup: function(clients)
{
    g_is_authenticated = true;
    g_is_client_list_retrieved = true;
    g_connection_status = {state: "connected", reason: ""};
    g_local_client_id = 0;
    g_local_username = "Me";
    g_current_channel_id = 1;
    g_current_chat_context_id = "chat-context-channel-1";
    g_client_list = clients;
    g_map_client_id_to_array_index = new Map(clients.map(function(client, index)
    {
        return [client.client_id, index];
    }));
    g_channel_list = [{channel_id: 1, parent_channel_id: -1, is_root_channel: true, unread_count: 0}];
    g_chat_context_array = [{type: "channel", chat_context_id: "chat-context-channel-1", last_known_message_sender_username: ""}];
    g_rsa_public_key_string = "public-me";
    android_host__begin_session();
    return {
        peer_state: function()
        {
            return Array.from(chat_peer_local_state.entries());
        },
        runtime_kind: function()
        {
            return {dom: G_HAS_DOM, headless: document.lemonchat_headless === true};
        },
        prepare_channel_move: function()
        {
            g_channel_list.push({channel_id: 2, parent_channel_id: 1, unread_count: 3});
            g_current_channel_keys = [{key_bytes: [1], iv_bytes: [2]}];
            g_channel_unread_counts[2] = 3;
            g_client_list[1].audio_state = G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE;
        },
        channel_move_state: function()
        {
            return {channel: g_current_channel_id, context: g_current_chat_context_id, receiver: g_chat_message_receiver_type,
                has_keys: g_current_channel_keys != null, contexts: g_chat_context_array.map(item => item.chat_context_id),
                unread: g_channel_unread_counts[2] || 0, audio: g_client_list[1].audio_state,
                quiet_audio: G_AUDIO_STATE.PUSH_TO_TALK_ENABLED};
        },
        forbid_rendering: function()
        {
            function fail()
            {
                throw new Error("headless channel update tried to render");
            }
            document.querySelector = fail;
            document.querySelectorAll = fail;
            document.getElementById = fail;
            document.getElementsByClassName = fail;
            channel_tree__generate_html_for_single_client = fail;
        },
        key_work_state: function()
        {
            return {pending: channel_key_creation_pending, generation: channel_key_creation_generation,
                timer: channel_key_creation_timer, waiting: maintainer_keys_wait_timer};
        },
        reconnect_target: function(host, port)
        {
            g_are_server_details_predefined = true;
            g_autoconnect_details = {host: host, port: port, keys: []};
            connection__show_predefined_details_in_connect_window();
            connection__disconnect_by_user();
            connection__request_connect("button");
            return target_slot.value;
        },
        permission_state: function()
        {
            return {groups: g_groups, roles: channel_groups.catalog, access: groups_window.access};
        },
        register_attachment: function(element)
        {
            chat_reactions__register_attachment(element);
        },
        recipient_key: function(context_id, client_id)
        {
            return chat__private_recipient_key(context_id, client_id);
        },
        idle_state: function()
        {
            return {idle: g_is_deep_idle, forced: node_ui_forced_idle, pending: pending_deep_idle_forced};
        },
        login_after_hidden: function()
        {
            g_is_authenticated = true;
            android_host__node_apply_idle_for_ui_state();
        },
        set_authenticated: function(value)
        {
            g_is_authenticated = value;
        },
        capture_presence_requests: function()
        {
            const channels = [];
            client_msg.send_come_from_idle_mode_request = function(channel_id)
            {
                channels.push(channel_id);
            };
            return channels;
        },
        capture_seen_receipts: function()
        {
            const requests = [];
            const previous = g_data_processing_worker.postMessage;
            g_data_processing_worker.postMessage = function(message)
            {
                if (message.type === "mainthread__create_seen_receipt_message")
                {
                    requests.push(message);
                    return;
                }
                previous(message);
            };
            return requests;
        },
        dispatch_incoming: function(type, frame)
        {
            g_node_connection_wanted = true;
            android_host__dispatch_safely(dispatch__mainthread_onmessage,
                {type: "data_processing_worker__" + type, value: frame}, "mainthread");
            return g_node_connection_wanted;
        },
        reset: function()
        {
            connection__reset_chat_app_keep_identity();
        },
        fail_reserved_handler: function()
        {
            g_node_connection_wanted = true;
            let reservation;
            android_host__dispatch_safely(function()
            {
                reservation = g_node_frame_listener(JSON.stringify({message:
                {
                    type: "offline_chat_message", value: "must not be published"
                }}), {defer_delivery: true});
                throw new Error("private content must not be logged");
            }, {type: "injected_owner_failure"}, "test");
            g_node_frame_listener(null, {release_delivery: reservation});
            return g_node_connection_wanted;
        },
        raw_membership: function(replay_clients)
        {
            g_loopback_port = 0;
            g_client_list = [];
            g_map_client_id_to_array_index.clear();
            chat_peer_local_state.clear();
            g_is_client_list_retrieved = false;
            server_msg.process_client_list_from_server({message: {clients: replay_clients, local_username: "Me"}});
        },
        page_replay: function(replay_clients, peer_states)
        {
            g_loopback_port = 1234;
            node_session_id = "";
            g_is_authenticated = false;
            g_client_list = [];
            g_map_client_id_to_array_index.clear();
            chat_peer_local_state.clear();
            g_is_client_list_retrieved = false;
            dispatch__mainthread_onmessage({data:
            {
                type: "data_processing_worker__local_session_state",
                value:
                {
                    session_id: "page-session",
                    public_key_string: "public-me",
                    identity_revision: 1,
                    peer_states: peer_states || replay_clients.map(function(client)
                    {
                        return {
                            key: chat__peer_state_key(client),
                            unread_count: client.unread_count || 0,
                            is_ignored_by_local_client: client.is_ignored_by_local_client === true,
                            is_muted_by_local_client: client.is_muted_by_local_client === true
                        };
                    })
                }
            }});
            server_msg.process_client_list_from_server({message: {clients: replay_clients, local_username: "Me"}});
        }
    };
},
` + anchor);

function fixture()
{
    const errors = [];
    const timers = [];
    const context = {
        process: {}, crypto: crypto.webcrypto, Buffer, TextDecoder, TextEncoder, performance, atob, btoa,
        console: {
            log: function()
            {
            },
            warn: function()
            {
            },
            error: function(value)
            {
                errors.push(String(value));
            }
        },
        setTimeout: function(callback, delay)
        {
            timers.push({ callback, delay });
            return timers.length;
        },
        clearTimeout: function()
        {
        },
        setInterval: function()
        {
        },
        clearInterval: function()
        {
        }
    };
    context.global = context;
    vm.createContext(context);
    vm.runInContext(source, context, { timeout: 10000 });
    const bundle = context.what;
    bundle.init_node_runtime();
    const clients = [
        { client_id: 0, username: "Me", public_key: "public-me", identity_hash: "bWU=", channel_id: 1, group_ids: [], unread_count: 0 },
        { client_id: 7, username: "Maya", public_key: "public-maya", identity_hash: "bWF5YQ==", channel_id: 1, group_ids: [], unread_count: 0 }
    ];
    const setup = bundle.event_test_setup(clients);
    const notifications = [];
    const counts = [];
    bundle.set_notification_listener(function(event)
    {
        notifications.push(event);
    });
    bundle.set_unread_listener(function(count)
    {
        counts.push(count);
    });
    return { context, bundle, setup, clients, notifications, counts, errors };
}

function direct_message(id)
{
    return { message: { type: "direct_chat_message", sender_id: 7, sender_username: "Maya", server_chat_message_id: id,
        some_json: { type: "direct_chat_message", value: JSON.stringify({ value: "hello", font_size: 14 }) } } };
}

test("repeated messages notify their actual sender and ignored messages do not add unread", function()
{
    const f = fixture();
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(1));
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(2));
    assert.equal(f.notifications.length, 2);
    assert.deepEqual(f.notifications.map(function(event)
    {
        return [event.username, event.sender_id, event.event_id];
    }), [["Maya", 7, 1], ["Maya", 7, 2]]);
    assert.equal(f.clients[1].unread_count, 2);
    assert.equal(f.bundle.node_set_local_ignore({ public_key: "public-maya", identity_hash: "bWF5YQ==", ignored: true }), true);
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(3));
    assert.equal(f.notifications.length, 2);
    assert.equal(f.clients[1].unread_count, 2);
    assert.deepEqual(f.errors, []);
});

test("absent offline sender has identity unread state and receives a native activity event", function()
{
    const f = fixture();
    const frame = { message: { type: "offline_chat_message", sender_identity_hash: "c2Ft", sender_name: "Sam",
        some_json: { value: JSON.stringify({ value: "queued hello" }) } } };
    f.bundle.server_msg.process_offline_chat_message_from_server(frame);
    assert.equal(f.notifications.length, 1);
    assert.equal(f.notifications[0].username, "Sam");
    assert.equal(f.notifications[0].sender_id, null);
    assert.equal(f.setup.peer_state().find(function(entry)
    {
        return entry[0] === "identity:c2Ft";
    })[1].unread_count, 1);
    assert.equal(f.bundle.node_delivery_context(frame), "identity:c2Ft");
    assert.equal(f.counts.at(-1), 1);
    assert.deepEqual(f.errors, []);
});

test("current page reads emit Android seen receipts once and respect the page preference", function()
{
    const f = fixture();
    const receipts = f.setup.capture_seen_receipts();
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(31));
    f.bundle.node_set_ui_attached(true, true);
    assert.equal(receipts.length, 0, "Attaching the page is not reading its conversation");
    const read = {context_id: "identity:bWF5YQ==", identity_hash: "bWF5YQ==", public_key: "public-maya", send_seen_receipts: true};
    assert.equal(f.bundle.node_read_conversation(read), true);
    assert.equal(receipts.length, 1);
    assert.equal(receipts[0].public_key, "public-maya");
    assert.equal(receipts[0].server_chat_message_id, 31);
    f.bundle.node_read_conversation(read);
    assert.equal(receipts.length, 1);
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(32));
    f.bundle.node_read_conversation({...read, send_seen_receipts: false});
    f.bundle.node_read_conversation(read);
    assert.equal(receipts.length, 1, "Reading with receipts disabled must not send one later");
});

test("pending seen receipts follow the original identity across slot reuse and stop at session reset", function()
{
    const f = fixture();
    const receipts = f.setup.capture_seen_receipts();
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(41));
    f.bundle.server_msg.process_client_disconnect_from_server({message: {client_id: 7, reason: "disconnect"}});
    f.bundle.server_msg.process_client_connect_from_server({message:
    {
        client_id: 7, username: "Sam", identity_hash: "c2Ft", public_key: "public-sam",
        channel_id: 1, audio_state: false, group_ids: [], is_music_bot: false
    }});
    f.bundle.node_set_ui_attached(true, true);
    f.bundle.node_read_conversation({context_id: "identity:c2Ft", identity_hash: "c2Ft", public_key: "public-sam", send_seen_receipts: true});
    assert.equal(receipts.length, 0, "Sam must not receive Maya's pending read receipt");
    f.bundle.server_msg.process_client_connect_from_server({message:
    {
        client_id: 8, username: "Maya", identity_hash: "bWF5YQ==", public_key: "public-maya",
        channel_id: 1, audio_state: false, group_ids: [], is_music_bot: false
    }});
    const read = {context_id: "identity:bWF5YQ==", identity_hash: "bWF5YQ==", public_key: "public-maya", send_seen_receipts: true};
    f.bundle.node_read_conversation(read);
    assert.equal(receipts.length, 1);
    assert.equal(receipts[0].receiver_id, 8);
    assert.equal(receipts[0].server_chat_message_id, 41);
    f.bundle.server_msg.process_direct_chat_message_from_server({message:
    {
        ...direct_message(42).message, sender_id: 8
    }});
    f.setup.reset();
    const replacement = f.bundle.event_test_setup(JSON.parse(JSON.stringify(f.clients)));
    const replacement_receipts = replacement.capture_seen_receipts();
    f.bundle.node_set_ui_attached(true, true);
    f.bundle.node_read_conversation(read);
    assert.equal(replacement_receipts.length, 0, "A full session reset must clear the old pending receipts");
});

test("invalid encrypted chat payloads are discarded without stopping the owning connection", async function(t)
{
    for (const payload of ["{", "null", "[]", "true", "123", '"text"', '{}'])
    {
        for (const kind of ["direct", "channel", "offline"])
        {
            const f = fixture();
            let host;
            await new Promise(function(resolve)
            {
                host = loopback.start(f.bundle, resolve);
            });
            t.after(function()
            {
                host.close();
            });
            const frame = kind === "channel" ? {message:
            {
                type: "channel_chat_message", sender_id: 7, channel_id: 1,
                server_chat_message_id: 51, decrypted_value: payload
            }} : kind === "direct" ? direct_message(51) : {message:
            {
                type: "offline_chat_message", sender_identity_hash: "bWF5YQ==", sender_name: "Maya",
                some_json: {type: "direct_chat_message", value: payload}
            }};
            if (kind === "direct")
            {
                frame.message.some_json.value = payload;
            }
            assert.equal(f.setup.dispatch_incoming(kind + "_chat_message", frame), true, kind + ": " + payload);
            assert.equal(f.notifications.length, 0);
            assert.equal(f.bundle.get_ui_session_snapshot().authenticated, true);
            assert.deepEqual(f.errors, []);
            f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(52));
            assert.equal(f.notifications.length, 1, "A later valid message is still processed");
        }
    }
});

test("headless poke notification preserves sender and ignored sender produces no notification", function()
{
    const f = fixture();
    f.bundle.server_msg.process_poke_from_server({ message: { client_id: 7, poke_message: "Are you there?" } });
    assert.equal(f.notifications.length, 1);
    assert.equal(f.notifications[0].type, "poke");
    assert.equal(f.notifications[0].sender_id, 7);
    assert.equal(f.notifications[0].username, "Maya");
    assert.equal(f.notifications[0].message, "Are you there?");
    f.bundle.node_set_local_ignore({ public_key: "public-maya", identity_hash: "bWF5YQ==", ignored: true });
    f.bundle.server_msg.process_poke_from_server({ message: { client_id: 7, poke_message: "Ignored" } });
    assert.equal(f.notifications.length, 1);
    assert.deepEqual(f.errors, []);
});

test("incremental departure and return preserve a peer's unread and local flags by identity", function()
{
    const f = fixture();
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(1));
    f.bundle.node_set_local_ignore({public_key: "public-maya", identity_hash: "bWF5YQ==", ignored: true});
    f.clients[1].is_muted_by_local_client = true;
    f.bundle.server_msg.process_client_disconnect_from_server({message: {client_id: 7, reason: "disconnect"}});
    f.bundle.server_msg.process_client_connect_from_server({message:
    {
        client_id: 8, username: "Maya", identity_hash: "bWF5YQ==", public_key: "public-maya",
        channel_id: 1, audio_state: false, group_ids: [], is_music_bot: false
    }});
    const returned = f.bundle.read_state().g_client_list.find(function(client)
    {
        return client.client_id === 8;
    });
    assert.equal(returned.is_ignored_by_local_client, true);
    assert.equal(returned.is_muted_by_local_client, true);
    assert.equal(returned.unread_count, 1);
    assert.equal(f.bundle.get_ui_session_snapshot().peer_states.find(function(peer)
    {
        return peer.key === "identity:bWF5YQ==";
    }).unread_count, 1);
});

test("incremental departure preserves newly received unread even without a prior peer snapshot", function()
{
    const f = fixture();
    f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(1));
    f.bundle.server_msg.process_client_disconnect_from_server({message: {client_id: 7, reason: "disconnect"}});
    const state = f.bundle.get_ui_session_snapshot().peer_states.find(function(peer)
    {
        return peer.key === "identity:bWF5YQ==";
    });
    assert.ok(state);
    assert.equal(state.unread_count, 1);
});

test("promoting an offline conversation binds the live recipient key for replies", function()
{
    const f = fixture();
    f.bundle.server_msg.process_offline_chat_message_from_server({message:
    {
        type: "offline_chat_message", sender_identity_hash: "c2Ft", sender_name: "Sam",
        some_json: {value: JSON.stringify({value: "queued hello"})}
    }});
    f.bundle.server_msg.process_client_connect_from_server({message:
    {
        client_id: 8, username: "Sam", identity_hash: "c2Ft", public_key: "public-sam",
        channel_id: 1, audio_state: false, group_ids: [], is_music_bot: false
    }});
    assert.equal(f.setup.recipient_key("chat-context-pm-8", 8), "public-sam");
});

test("task removal forces Node idle after gentle backgrounding and survives hidden replay", function()
{
    const f = fixture();
    f.bundle.node_set_ui_attached(true, true);
    f.bundle.node_set_ui_attached(false, true);
    assert.equal(f.setup.idle_state().idle, false, "Home preserves an active non-root channel");
    f.bundle.node_set_ui_attached(false, true, false, true);
    assert.equal(f.setup.idle_state().idle, true);
    assert.equal(f.setup.idle_state().forced, true);
    f.bundle.node_set_ui_attached(false, true);
    assert.equal(f.setup.idle_state().forced, true);
    f.bundle.node_set_ui_attached(true, true);
    assert.equal(f.setup.idle_state().forced, false);
    assert.equal(f.setup.idle_state().idle, false);
});

test("task removal before authentication keeps its force when the login completes", function()
{
    const f = fixture();
    f.setup.set_authenticated(false);
    f.bundle.node_set_ui_attached(false, true, false, true);
    assert.equal(f.setup.idle_state().pending, true);
    f.setup.login_after_hidden();
    assert.equal(f.setup.idle_state().idle, true);
});

test("accepting a current call clears the retained task-removal force", function()
{
    const f = fixture();
    f.bundle.node_set_ui_attached(false, true, false, true);
    f.bundle.set_incoming_call_listener(function()
    {
    });
    f.bundle.server_msg.process_call_from_server({message: {caller_client_id: 7}});
    const call = f.bundle.get_ui_session_snapshot().current_call;
    assert.equal(f.bundle.node_handle_call_action("accept_call", call), true);
    assert.equal(f.setup.idle_state().forced, false);
    assert.equal(f.setup.idle_state().idle, false);
});

test("call result retries are idempotent and recover a replayed forced-idle snapshot", function()
{
    const f = fixture();
    const requests = f.setup.capture_presence_requests();
    f.bundle.set_incoming_call_listener(function()
    {
    });
    f.bundle.server_msg.process_call_from_server({message: {caller_client_id: 7}});
    const call = f.bundle.get_ui_session_snapshot().current_call;
    assert.equal(f.bundle.node_handle_call_action("accept_call", call), true);
    assert.equal(f.bundle.node_handle_call_action("accept_call", call), true);
    assert.equal(requests.length, 1, "An ordinary result retry cannot repeat the join");
    f.bundle.node_set_ui_attached(false, true, false, true);
    assert.equal(f.setup.idle_state().idle, true);
    assert.equal(f.bundle.node_handle_call_action("accept_call", call), true);
    assert.equal(f.setup.idle_state().forced, false);
    assert.equal(f.setup.idle_state().idle, false);
    assert.equal(requests.length, 2, "The replayed go-idle needs one compensating presence request");
    assert.equal(requests[1], call.channel_id);
    assert.equal(f.bundle.node_handle_call_action("accept_call", {...call, session_id: "obsolete"}), false);
    f.bundle.server_msg.process_call_from_server({message: {caller_client_id: 7}});
    const replacement = f.bundle.get_ui_session_snapshot().current_call;
    f.bundle.node_handle_call_action("accept_call", call);
    assert.equal(f.bundle.get_ui_session_snapshot().current_call.call_id, replacement.call_id);
    assert.equal(requests.length, 2);
});

test("fresh page membership replay preserves the Node-owned ignored and muted flags", function()
{
    const f = fixture();
    const clients = JSON.parse(JSON.stringify(f.clients));
    clients[1].is_ignored_by_local_client = true;
    clients[1].is_muted_by_local_client = true;
    f.setup.page_replay(clients);
    const client = f.bundle.read_state().g_client_list.find(function(candidate)
    {
        return candidate.client_id === 7;
    });
    assert.equal(client.is_ignored_by_local_client, true);
    assert.equal(client.is_muted_by_local_client, true);
});

test("untrusted membership flags cannot change local ignore state", function()
{
    const f = fixture();
    const clients = JSON.parse(JSON.stringify(f.clients));
    clients[1].is_ignored_by_local_client = true;
    clients[1].is_muted_by_local_client = true;
    clients[1].unread_count = 200;
    f.setup.raw_membership(clients);
    const client = f.bundle.read_state().g_client_list.find(function(candidate)
    {
        return candidate.client_id === 7;
    });
    assert.equal(client.is_ignored_by_local_client, false);
    assert.equal(client.is_muted_by_local_client, false);
    assert.equal(client.unread_count, 0);
});

test("pending content replay does not double the authoritative unread snapshot", function()
{
    const f = fixture();
    const clients = JSON.parse(JSON.stringify(f.clients));
    clients[1].unread_count = 3;
    f.setup.page_replay(clients);
    delete f.context.process;
    for (let id = 1; id <= 3; id++)
    {
        f.bundle.server_msg.process_direct_chat_message_from_server(direct_message(id));
    }
    const client = f.bundle.read_state().g_client_list.find(function(candidate)
    {
        return candidate.client_id === 7;
    });
    assert.equal(client.unread_count, 3);
});

test("trusted absent-peer state suppresses replay from an ignored offline sender", function()
{
    const f = fixture();
    f.setup.page_replay(JSON.parse(JSON.stringify(f.clients)), [{ key: "identity:c2Ft", unread_count: 3,
        is_ignored_by_local_client: true, is_muted_by_local_client: false }]);
    delete f.context.process;
    f.bundle.server_msg.process_offline_chat_message_from_server({ message: { type: "offline_chat_message",
        sender_identity_hash: "c2Ft", sender_name: "Sam", some_json: { value: JSON.stringify({ value: "ignored" }) } } });
    const state = f.setup.peer_state().find(function(entry)
    {
        return entry[0] === "identity:c2Ft";
    })[1];
    assert.equal(state.is_ignored_by_local_client, true);
    assert.equal(state.unread_count, 3);
    assert.equal(f.bundle.read_state().g_chat_context_array.some(function(context)
    {
        return context.chat_context_id === "chat-context-offline-c2Ft";
    }), false);
});

test("reading legacy absent-peer history matches its public key before clearing unread", function()
{
    const f = fixture();
    f.setup.page_replay(JSON.parse(JSON.stringify(f.clients)),
        [{key: "key:legacy-public", unread_count: 2, is_ignored_by_local_client: false, is_muted_by_local_client: false}]);
    f.bundle.node_set_ui_attached(true, true);
    assert.equal(f.bundle.node_read_conversation({context_id: "key:legacy-public", public_key: "someone-else", identity_hash: ""}), false);
    assert.equal(f.setup.peer_state().find(function(entry)
    {
        return entry[0] === "key:legacy-public";
    })[1].unread_count, 2);
    assert.equal(f.bundle.node_read_conversation({context_id: "key:legacy-public", public_key: "legacy-public", identity_hash: ""}), true);
    assert.equal(f.setup.peer_state().find(function(entry)
    {
        return entry[0] === "key:legacy-public";
    })[1].unread_count, 0);
});

test("full headless reset clears current call ownership and completes without a DOM exception", function()
{
    const f = fixture();
    const calls = [];
    f.bundle.set_incoming_call_listener(function(caller, channel_id, record)
    {
        calls.push(record);
    });
    f.bundle.server_msg.process_call_from_server({ message: { caller_client_id: 7 } });
    assert.equal(calls.length, 1);
    assert.ok(f.bundle.get_ui_session_snapshot().current_call);
    assert.doesNotThrow(function()
    {
        f.setup.reset();
    });
    assert.equal(f.bundle.get_ui_session_snapshot().current_call, null);
    assert.equal(f.bundle.get_ui_session_snapshot().authenticated, false);
});

test("a throwing full-bundle owner handler stops intake with an explicit error", async function(t)
{
    const f = fixture();
    let host;
    await new Promise(function(resolve)
    {
        host = loopback.start(f.bundle, resolve);
    });
    t.after(function()
    {
        host.close();
    });
    assert.equal(f.setup.fail_reserved_handler(), false);
    assert.equal(f.bundle.get_ui_session_snapshot().authenticated, false);
    assert.equal(f.bundle.get_connection_status().state, "error");
    assert.equal(f.bundle.get_connection_status().reason, "Could not process an incoming message. Reconnect to continue.");
    assert.equal(f.errors.some(function(value)
    {
        return value.includes("private content");
    }), false);
});


test("production headless marker excludes browser rendering while group state remains available", function()
{
    const f = fixture();
    assert.equal(f.setup.runtime_kind().headless, true);
    assert.equal(f.setup.runtime_kind().dom, false);
    const groups = [{group_id: 2, group_name: "Helpers", group_type: "normal", permissions: {send_pokes: "grant"}}];
    const roles = [{group_id: 1, name: "Channel Admin", icon_id: -2, permissions: {talk_power: 1000, flags: {talk_power: {skip: false, negate: false}}}}];
    const access = {view_group_permissions: true, powers: {permission_edit_power: 40}, editable_groups: [2]};
    assert.equal(f.setup.dispatch_incoming("group_list_from_server", {message: {type: "group_list", groups, channel_groups: roles, access}}), true);
    assert.equal(f.setup.permission_state().groups[0].group_name, "Helpers");
    assert.equal(f.setup.permission_state().roles[0].name, "Channel Admin");
    assert.equal(f.setup.permission_state().access.powers.permission_edit_power, 40);
    const exported = f.bundle.read_state();
    assert.equal(exported.channel_groups[0].permissions.talk_power, 1000);
    assert.equal(exported.group_access.powers.permission_edit_power, 40);
    assert.equal(exported.group_access.editable_groups[0], 2);
    exported.channel_groups[0].permissions.talk_power = 0;
    exported.channel_groups[0].permissions.flags.talk_power.skip = true;
    exported.group_access.powers.permission_edit_power = 0;
    exported.group_access.editable_groups.push(9);
    exported.g_groups[0].permissions.send_pokes = "deny";
    const next = f.bundle.read_state();
    assert.equal(next.channel_groups[0].permissions.talk_power, 1000);
    assert.equal(next.channel_groups[0].permissions.flags.talk_power.skip, false);
    assert.equal(next.group_access.powers.permission_edit_power, 40);
    assert.equal(next.group_access.editable_groups.length, 1);
    assert.equal(next.g_groups[0].permissions.send_pokes, "grant");
    assert.equal(f.bundle.get_ui_session_snapshot().authenticated, true);
    assert.deepEqual(f.errors, []);
});

test("headless emoji dispatch and reaction-bearing messages preserve the owning connection", function()
{
    const f = fixture();
    const emoji = {type: "server_emoji_list", emojis: [{id: "a".repeat(32), name: "hello", unicode: "x"}]};
    assert.equal(f.setup.dispatch_incoming("server_emoji", emoji), true);
    const direct = direct_message(73);
    direct.message.some_json.value = JSON.stringify({value: "actual message", reaction_id: "b".repeat(32), font_size: 14});
    assert.equal(f.setup.dispatch_incoming("direct_chat_message", direct), true);
    assert.equal(f.notifications.length, 1);
    assert.equal(f.notifications[0].sender_id, 7);
    assert.equal(f.clients[1].unread_count, 1);
    assert.equal(f.bundle.get_ui_session_snapshot().authenticated, true);
    assert.deepEqual(f.errors, []);
});

test("headless attachment registration never touches a rendering element", function()
{
    const f = fixture();
    const element = new Proxy({},
    {
        get()
        {
            throw new Error("rendering was attempted in the service");
        }
    });
    assert.doesNotThrow(function()
    {
        f.setup.register_attachment(element);
    });
});


test("the headless shim preserves an existing browser document and DOM classification", function()
{
    const document = {};
    const context = vm.createContext({window: {}, document});
    vm.runInContext(fs.readFileSync(path.resolve(__dirname, "../dom-shim.js"), "utf8"), context);
    const globals = fs.readFileSync(path.resolve(__dirname, "../../../browser/main/scripts/app/globals.js"), "utf8");
    const declaration = globals.match(/^var G_HAS_DOM = .*;$/m);
    assert.ok(declaration);
    vm.runInContext(declaration[0], context);
    assert.equal(context.G_HAS_DOM, true);
    assert.equal(context.document, document);
    assert.equal(document.lemonchat_headless, undefined);
    const worker = vm.createContext({});
    vm.runInContext(declaration[0], worker);
    assert.equal(worker.G_HAS_DOM, false);
});


test("headless manual reconnect keeps the saved server after the connect dialog reset", function()
{
    const f = fixture();
    const target = f.setup.reconnect_target("saved.example", 1111);
    assert.equal(target.kind, "server");
    assert.equal(target.host, "saved.example");
    assert.equal(target.port, 1111);
    assert.deepEqual(f.errors, []);
});


test("headless local channel joins update keys and chat state without browser playback", function()
{
    const f = fixture();
    f.setup.prepare_channel_move();
    f.setup.forbid_rendering();
    for (const channel of [2, 1, 2])
    {
        assert.equal(f.setup.dispatch_incoming("channel_join_from_server", {message:
        {
            type: "channel_join", client_id: 0, channel_id: channel, channel_group_id: 3
        }}), true);
        const state = f.setup.channel_move_state();
        assert.equal(state.channel, channel);
        assert.equal(state.context, "chat-context-channel-" + channel);
        assert.equal(state.receiver, "channel");
        assert.equal(state.has_keys, false);
        assert.equal(state.unread, 0);
        assert.equal(state.audio, state.quiet_audio);
        assert.equal(state.contexts.filter(id => id === "chat-context-channel-2").length, 1);
        assert.equal(f.clients[0].channel_group_id, 3);
        assert.deepEqual(f.errors, []);
    }
});

test("headless peer moves keep local context and channel visibility current", function()
{
    const f = fixture();
    f.setup.prepare_channel_move();
    f.setup.forbid_rendering();
    for (const channel of [-1, 2, 1])
    {
        f.setup.dispatch_incoming("channel_join_from_server", {message:
        {
            type: "channel_join", client_id: 7, channel_id: channel, channel_group_id: 2
        }});
        assert.equal(f.clients[1].channel_id, channel);
        assert.equal(f.clients[1].is_clients_channel_hidden, channel === -1);
        assert.equal(f.clients[1].channel_group_id, 2);
        assert.equal(f.setup.channel_move_state().channel, 1);
        assert.deepEqual(f.errors, []);
    }
});

test("headless idle and return retain channel membership and receiving keys", function()
{
    const f = fixture();
    f.setup.prepare_channel_move();
    f.setup.forbid_rendering();
    for (const clientId of [7, 0])
    {
        assert.equal(f.setup.dispatch_incoming("client_going_to_idle_mode", {message: {client_id: clientId}}), true);
        const client = f.clients.find(item => item.client_id === clientId);
        assert.equal(client.is_idle, true);
        assert.equal(client.channel_id, -2);
        assert.equal(client.channel_group_id, 0);
        assert.equal(f.setup.dispatch_incoming("client_coming_back_from_idle_mode", {message: {client_id: clientId, channel_id: 2, channel_group_id: 3}}), true);
        assert.equal(client.is_idle, false);
        assert.equal(client.channel_id, 2);
        assert.equal(client.channel_group_id, 3);
        assert.equal(client.is_clients_channel_hidden, false);
        assert.deepEqual(f.errors, []);
    }
    assert.equal(f.setup.channel_move_state().channel, 2);
    assert.equal(f.setup.channel_move_state().has_keys, false);
});

test("headless channel catalogue mutations preserve complete channel state", function()
{
    const f = fixture();
    f.setup.forbid_rendering();
    const channel = {channel_id: 2, parent_channel_id: 1, name: "Second", description: "Before",
        is_audio_enabled: true, is_video_stream_enabled: true, is_temp_channel: false, is_client_limit_active: false,
        max_client_count: 20, is_using_password: false, has_maintainer: false, maintainer_id: -1};
    assert.equal(f.setup.dispatch_incoming("channel_create_from_server", {message: channel}), true);
    assert.equal(f.bundle.read_state().g_channel_list.find(item => item.channel_id === 2).parent_channel_id, 1);
    assert.equal(f.setup.dispatch_incoming("channel_edit_from_server", {message: {...channel,
        channel_name: "Renamed", channel_description: "After", is_description_hidden: true,
        is_temp_channel: true, is_using_password: true, is_audio_enabled: false, is_video_stream_enabled: false,
        required_talk_power: 12, required_message_power: 13, required_picture_power: 14, required_stream_power: 15,
        required_file_power: 16, required_view_power: 17, required_create_power: 18}}), true);
    const edited = f.bundle.read_state().g_channel_list.find(item => item.channel_id === 2);
    assert.equal(edited.name, "Renamed");
    assert.equal(edited.description, "After");
    assert.equal(edited.is_temp_channel, true);
    assert.equal(edited.is_audio_enabled, false);
    assert.equal(edited.is_video_stream_enabled, false);
    assert.equal(edited.required_create_power, 18);
    assert.equal(f.setup.dispatch_incoming("channel_delete_from_server", {message: {channel_id: 2}}), true);
    assert.equal(f.bundle.read_state().g_channel_list.some(item => item.channel_id === 2), false);
    assert.deepEqual(f.errors, []);
});

test("headless maintainer changes retain key ownership across local moves", function()
{
    const f = fixture();
    f.setup.prepare_channel_move();
    f.setup.forbid_rendering();
    const announce = (channel, client, generation) => f.setup.dispatch_incoming("channel_maintainer_id", {message:
    {
        channel_id: channel, maintainer_id: client, maintainer_generation: generation, has_maintainer: true
    }});
    assert.equal(announce(1, 0, 1), true);
    assert.equal(f.setup.key_work_state().pending, true);
    f.setup.dispatch_incoming("channel_join_from_server", {message: {client_id: 0, channel_id: 2}});
    assert.equal(f.setup.key_work_state().pending, false);
    assert.equal(f.setup.key_work_state().timer, null);
    const generation = f.setup.key_work_state().generation;
    assert.equal(announce(1, 0, 2), true);
    assert.equal(f.setup.key_work_state().generation, generation);
    assert.equal(announce(2, 7, 3), true);
    assert.equal(f.bundle.read_state().g_channel_list.find(item => item.channel_id === 2).maintainer_id, undefined);
    f.setup.dispatch_incoming("channel_join_from_server", {message: {client_id: 7, channel_id: 2}});
    assert.equal(announce(2, 7, 4), true);
    const channel = f.bundle.read_state().g_channel_list.find(item => item.channel_id === 2);
    assert.equal(channel.maintainer_id, 7);
    assert.equal(channel.maintainer_generation, 4);
    assert.equal(f.setup.key_work_state().pending, false);
    assert.deepEqual(f.errors, []);
});
