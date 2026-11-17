"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
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

function fixture(newIdentity)
{
    const oldClient =
    {
        client_id: 7,
        username: "Maya",
        public_key: "synthetic-maya-key",
        identity_hash: "MAYA",
        channel_id: 0,
        unread_count: 3,
        is_ignored_by_local_client: true,
        is_muted_by_local_client: true
    };
    const freshClient =
    {
        client_id: 7,
        username: newIdentity ? "Sam" : "Maya",
        public_key: newIdentity ? "synthetic-sam-key" : "synthetic-maya-key",
        identity_hash: newIdentity ? "SAM" : "MAYA",
        channel_id: 0,
        is_idle: false,
        group_ids: []
    };
    const nodes = new Map();
    function node(id)
    {
        if (!nodes.has(id))
        {
            nodes.set(id,
            {
                style: {},
                value: "",
                innerHTML: id === "chat-context-pm-7" ? "now talking to user: Maya; his public key: synthetic-maya-key; old Maya history" : "",
                removed: false,
                addEventListener: noop,
                insertAdjacentHTML(position, text)
                {
                    this.innerHTML += text;
                },
                querySelector()
                {
                    return {};
                },
                remove()
                {
                    this.removed = true;
                }
            });
        }
        return nodes.get(id);
    }
    const workerMessages = [];
    const context =
    {
        console: { log: noop, warn: noop },
        chat_peer_local_state: new Map(),
        chat__update_app_unread_badge: noop,
        utils__custom_alert: noop,
        G_HAS_DOM: true,
        g_is_authenticated: true,
        g_server_policy: { is_permission_system_enabled: false },
        g_fast_reconnect:
        {
            resumed: true,
            pending_lists:
            {
                channel: { message: { channels: [{ channel_id: 0 }] } },
                client: { message: { clients: [freshClient], local_username: "Alice" } }
            },
            deadline_timer: 1
        },
        g_client_list: [oldClient],
        g_map_client_id_to_array_index: new Map([[7, 0]]),
        g_channel_list: [{ channel_id: 0 }],
        g_groups: [],
        g_icons: [],
        g_icon_upload_queue: [],
        g_icon_upload_in_flight_base64: null,
        g_is_client_list_retrieved: true,
        g_is_channel_list_retrieved: true,
        g_chat_context_array: [{ type: "user", chat_context_id: "chat-context-pm-7", last_known_message_sender_username: "Maya" }],
        g_is_running_in_android_webview: false,
        g_current_channel_id: 0,
        g_current_chat_context_id: "chat-context-pm-7",
        g_chat_message_receiver_id: 7,
        g_chat_message_receiver_type: "user",
        g_pending_chat_file: null,
        g_base64_picture_string_to_send: "",
        g_local_client_id: 1,
        g_local_username: "Alice",
        g_local_message_id: 5,
        g_rsa_public_key_string: "synthetic-alice-key",
        g_selected_font: "default",
        g_selected_font_size: 12,
        g_selected_font_color: "#ffffff",
        clearTimeout: noop,
        utils__custom_log: noop,
        utils__hide_custom_alert: noop,
        groups__schedule_refresh: noop,
        chat__sanitize_string(value)
        {
            return value;
        },
        channel_tree__generate_html_for_single_client()
        {
            return "";
        },
        channel_tree__enqueue_all_avatars_for_loading: noop,
        channel_tree__start_avatar_prefetch: noop,
        video__apply_client_list: noop,
        chat__clear_seen_indicator_for_client: noop,
        chat__normalize_composed_text(value)
        {
            return value;
        },
        android_host__generate_message_sender_html()
        {
            return "Alice";
        },
        chat_reactions__new_id()
        {
            return "synthetic-message-id";
        },
        chat_reactions__register: noop,
        chat_replies__take()
        {
            return null;
        },
        chat__append_local_chat_message_block: noop,
        chat__scroll_chat_to_end: noop,
        UI:
        {
            connected_local_user_input_on_focusout: noop,
            refresh_all_channel_fullness: noop,
            refresh_member_list_state: noop
        },
        g_data_processing_worker:
        {
            postMessage(message)
            {
                workerMessages.push(message);
            }
        },
        document:
        {
            getElementById: node,
            getElementsByClassName()
            {
                return [];
            },
            querySelectorAll()
            {
                return [];
            },
            querySelector: node
        }
    };
    const messages = fs.readFileSync(path.join(root, "msg/messages.js"), "utf8");
    const start = messages.indexOf("    process_client_list_from_server: function(msg)");
    const end = messages.indexOf("\n    /**", start);
    assert.ok(start >= 0 && end > start);
    vm.createContext(context);
    vm.runInContext("var server_msg = {\n" + messages.slice(start, end) + "\n};", context);
    context.server_msg.process_channel_list_from_server = function(message)
    {
        context.g_channel_list.push(...message.message.channels);
        context.g_is_channel_list_retrieved = true;
    };
    for (const entry of [
        ["msg/messages.js", "messages__create_client_record"],
        ["connection/connection.js", "connection__fast_reconnect_apply_lists"],
        ["chat/chat.js", "chat__peer_state_key"],
        ["chat/chat.js", "chat__remember_peer_state"],
        ["chat/chat.js", "chat__restore_peer_state"],
        ["chat/chat.js", "chat__bind_private_contexts"],
        ["chat/chat.js", "chat__private_recipient_key"],
        ["chat/chat.js", "chat__reconcile_private_contexts"],
        ["chat/chat.js", "chat__send_composed_chat_message"],
        ["chat/chat.js", "chat__send_chat_message"],
        ["chat/chat.js", "chat__send_direct_chat_message"],
        ["chat/chat.js", "chat__get_chat_context_index_by_chat_context_id"],
        ["channel/channel-tree.js", "channel_tree__get_client_by_client_id"],
        ["channel/channel-tree.js", "channel_tree__get_public_key_by_client_id"]
    ])
    {
        vm.runInContext(extract(entry[0], entry[1]), context);
    }
    node("chat-context-pm-7");
    return { context, node, workerMessages, oldClient, freshClient };
}

{
    const f = fixture(false);
    assert.equal(f.context.g_client_list[0].unread_count, 3);
    f.context.connection__fast_reconnect_apply_lists();
    assert.equal(f.context.g_client_list[0].identity_hash, "MAYA");
    assert.equal(f.context.g_client_list[0].unread_count, 3);
    assert.equal(f.context.g_client_list[0].is_ignored_by_local_client, true);
    assert.equal(f.context.g_client_list[0].is_muted_by_local_client, true);
    assert.equal(f.node("chat-context-pm-7").removed, false);
    console.log("PASS: same-identity fast refresh retains unread/ignore/mute fields.");
    f.node("chat-input-container-text-input").value = "message still for Maya";
    f.context.chat__send_chat_message();
    assert.equal(f.workerMessages[0].receiver_public_key, "synthetic-maya-key");
    console.log("CONTROL: unchanged identity retains the context and prepares its send for Maya's key.");
}

{
    const f = fixture(true);
    f.context.connection__fast_reconnect_apply_lists();
    assert.equal(f.context.g_client_list[0].identity_hash, "SAM");
    assert.equal(f.node("chat-context-pm-7").removed, false);
    assert.ok(f.node("chat-context-pm-7").innerHTML.includes("synthetic-maya-key"));
    f.node("chat-input-container-text-input").value = "intended for the existing Maya conversation";
    f.context.chat__send_composed_chat_message("user");
    assert.equal(f.workerMessages.length, 0);
    assert.equal(f.context.g_chat_context_array[0].peer_public_key, "synthetic-maya-key");
    assert.equal(f.context.g_current_chat_context_id, "chat-context-offline-MAYA");
    assert.equal(f.context.g_client_list[0].unread_count, 0);
    assert.equal(f.context.g_client_list[0].is_ignored_by_local_client, false);
    console.log("PASS: reused slot cannot send Maya's conversation to Sam or transfer local flags.");
}

{
    const f = fixture(false);
    f.context.g_fast_reconnect.pending_lists.client.message.clients[0].client_id = 8;
    f.context.connection__fast_reconnect_apply_lists();
    assert.equal(f.context.g_current_chat_context_id, "chat-context-pm-8");
    assert.equal(f.context.g_chat_message_receiver_id, 8);
    assert.equal(f.context.g_client_list[0].unread_count, 3);
    f.node("chat-input-container-text-input").value = "same Maya at new slot";
    f.context.chat__send_composed_chat_message("user");
    assert.equal(f.workerMessages[0].receiver_public_key, "synthetic-maya-key");
    assert.equal(f.workerMessages[0].chat_message_receiver_id, 8);
    console.log("PASS: same identity at a new slot retains history and uses its verified key.");
}

{
    const f = fixture(true);
    f.oldClient.identity_hash = "";
    const selectors = new Map();
    f.context.document.querySelectorAll = function(query)
    {
        const attributes = new Map();
        selectors.set(query, attributes);
        return [{setAttribute(name, value)
        {
            attributes.set(name, value);
        }}];
    };
    f.context.connection__fast_reconnect_apply_lists();
    const context = f.context.g_chat_context_array[0];
    const expected = context.chat_context_id.slice("chat-context-".length);
    assert.equal(selectors.get('[data-chat-context-selector-id="user-7"]').get("data-chat-context-selector-id"), expected);
    assert.equal(selectors.get('[data-chat-context-remove-selector-id="user-7"]').get("data-chat-context-remove-selector-id"), expected);
    assert.equal(context.peer_public_key, "synthetic-maya-key");
    assert.equal(f.context.g_chat_message_receiver_id, null);
    f.context.g_chat_context_array.push(
    {
        type: "user",
        chat_context_id: "chat-context-pm-7",
        peer_public_key: "synthetic-second-key",
        peer_identity_hash: ""
    });
    f.context.chat__reconcile_private_contexts();
    assert.notEqual(f.context.g_chat_context_array[1].chat_context_id, context.chat_context_id);
    console.log("PASS: a detached legacy key-only conversation keeps matching open and close selectors.");
}

{
    const f = fixture(false);
    const context = f.context;
    const events = [];
    const clients =
    [
        {client_id: "11", username: "Hidden", channel_id: -1, is_idle: true, is_clients_channel_hidden: true},
        {client_id: "12", username: "A&B", channel_id: -2, is_idle: true, is_clients_channel_hidden: true,
            is_music_bot: true, music_bot_encryption: 2, music_bot_keys: {key: "synthetic-bot-key"}},
        {client_id: "13", username: "Alice", channel_id: 0, is_idle: false, is_clients_channel_hidden: true}
    ];
    context.g_client_list = [];
    context.g_map_client_id_to_array_index.clear();
    context.g_chat_context_array = [];
    context.g_is_client_list_retrieved = false;
    vm.runInContext(extract("chat/chat.js", "chat__sanitize_string"), context);
    context.channel_tree__generate_html_for_single_client = function(client, isLocal)
    {
        assert.equal(client, clients.find(function(record)
        {
            return record.client_id === client.client_id;
        }));
        events.push(["render", client.username, isLocal,
            context.g_map_client_id_to_array_index.has(Number(client.client_id)), context.g_local_client_id]);
        return client.username;
    };
    context.document.querySelector = function(selector)
    {
        return {
            insertAdjacentHTML(position, html)
            {
                events.push(["insert", selector, position, html, context.g_client_list.length]);
            }
        };
    };
    context.server_msg.process_client_list_from_server({message: {clients, local_username: "Alice"}});
    assert.deepEqual(events,
    [
        ["render", "A&B", false, true, 1],
        ["insert", '[data-channel-id="idle"]', "afterend", "A&B", 2],
        ["render", "Alice", true, false, "13"],
        ["insert", '[data-channel-id="0"]', "afterend", "Alice", 2]
    ]);
    assert.deepEqual(Array.from(context.g_map_client_id_to_array_index), [[11, 0], [12, 1], [13, 2]]);
    assert.equal(context.g_client_list[0].is_clients_channel_hidden, true);
    assert.equal(context.g_client_list[1].is_clients_channel_hidden, true);
    assert.equal(context.g_client_list[2].is_clients_channel_hidden, false);
    const bot = context.g_client_list[1];
    assert.equal(bot.username, "A&amp;B");
    assert.equal(bot.is_music_bot, true);
    assert.equal(bot.music_bot_encryption, 2);
    assert.equal(bot.music_bot_keys, clients[1].music_bot_keys);
    console.log("PASS: hidden, idle and visible clients retain their rendering order, raw names and music bot state.");
}
