"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "../../../browser/main/scripts/app");

function noop()
{
}

function extract(file, name)
{
    const source = fs.readFileSync(path.join(root, file), "utf8");
    const match = source.match(new RegExp("^function " + name + "\\([^\\n]*\\)\\r?\\n\\{[\\s\\S]*?^\\}", "m"));
    assert.ok(match, name);
    return match[0];
}

function method(file, name)
{
    const source = fs.readFileSync(path.join(root, file), "utf8");
    const start = source.indexOf("    " + name + ": function(");
    const end = source.indexOf("\n    /**", start);
    assert.ok(start >= 0 && end > start, name);
    return source.slice(start, end);
}

function fixture(live)
{
    const nodes = new Map();
    const renders = [];
    const receipts = [];
    const badges = [];
    const reads = [];
    const keys = [];
    const closed = [];
    const transferred = [];
    const maya = {client_id: 7, public_key: "maya-key", identity_hash: "MAYA", username: "Maya"};
    function makeNode(id)
    {
        const result =
        {
            id: id,
            style: {},
            innerHTML: "",
            value: "",
            addEventListener: noop,
            remove()
            {
                nodes.delete(id);
            },
            getElementsByClassName()
            {
                return [];
            },
            querySelector()
            {
                return {};
            },
            insertAdjacentHTML(position, html)
            {
                this.innerHTML += html;
                for (const match of html.matchAll(/\bid="([^"]+)"/g))
                {
                    if (!nodes.has(match[1]))
                    {
                        makeNode(match[1]);
                    }
                }
            }
        };
        nodes.set(id, result);
        return result;
    }
    for (const id of ["chat-context-container", "chat-context-selectors-container", "chat-context-channel-0", "chat-context-channel-2", "chat-input-container-text-input"])
    {
        makeNode(id);
    }
    const context =
    {
        console: {log: noop, warn: noop},
        g_client_list: live || [],
        g_offline_client_list: [],
        g_chat_context_array: [{type: "channel", chat_context_id: "chat-context-channel-0"}, {type: "channel", chat_context_id: "chat-context-channel-2"}],
        g_current_chat_context_id: "chat-context-channel-0",
        g_current_channel_id: 0,
        g_chat_message_author_public_keys: {},
        g_rsa_public_key_string: "alice-key",
        g_are_sound_effects_enabled: false,
        g_send_seen_receipts: true,
        g_local_client_id: 1,
        g_local_username: "Alice",
        g_worker_connection_generation: 1,
        G_HAS_DOM: true,
        node_session_id: "current",
        chat_peer_local_state: new Map(),
        chat_last_delivery_by_context: new Map(),
        sounds__play: noop,
        android_host__is_ui_only_runtime()
        {
            return true;
        },
        android_host__generate_message_sender_html(id, name)
        {
            renders.push({id, name});
            return name;
        },
        android_host__notify: noop,
        channel_tree__get_client_by_client_id(id)
        {
            return context.g_client_list.find(function(client)
            {
                return client.client_id === Number(id);
            }) || null;
        },
        chat__clear_typing_from_client: noop,
        chat__scroll_chat_to_end: noop,
        chat__sanitize_font()
        {
            return "default";
        },
        chat__sanitize_color()
        {
            return "#000000";
        },
        chat__increment_channel_unread_count: noop,
        chat__increment_unread_count(id)
        {
            badges.push(id);
        },
        chat__render_unread_badge: noop,
        chat__is_the_user_actually_looking()
        {
            return true;
        },
        chat__remember_message_awaiting_receipt(id)
        {
            receipts.push(id);
        },
        chat__send_seen_receipt_for_message(id)
        {
            receipts.push(id);
        },
        chat__receive_message_edit()
        {
            return false;
        },
        chat_reactions__receive()
        {
            return false;
        },
        chat_reactions__register(element, reaction, key)
        {
            keys.push(key);
        },
        chat_files__begin_chat_file_transfer(id)
        {
            transferred.push(id);
            return true;
        },
        chat_files__generate_chat_picture_progress_html()
        {
            return "<span>receiving</span>";
        },
        chat_files__generate_chat_file_card_html(file)
        {
            return '<div id="chat-file-card-' + file.key + '">' + file.name + "</div>";
        },
        chat_files__get_chat_file_card_by_key(id)
        {
            return nodes.get("chat-file-card-" + id);
        },
        chat_files__chat_file_card_onrightclick: noop,
        chat_files__cancel_pending_read: noop,
        chat_replies__refresh: noop,
        groups__refresh_channel_controls: noop,
        chat__clear_channel_unread_count: noop,
        chat__send_direct_chat_closed_notice(id)
        {
            closed.push(id);
        },
        connection__send_local_command(type, args)
        {
            reads.push({type, args});
        },
        document:
        {
            getElementById(id)
            {
                return nodes.get(id) || null;
            },
            getElementsByClassName()
            {
                return [];
            },
            querySelector()
            {
                return {style: {}, remove: noop};
            }
        },
        UI:
        {
            enabled: null,
            schedule_member_list_sync: noop,
            enable_inputs()
            {
                this.enabled = true;
            },
            disable_inputs()
            {
                this.enabled = false;
            },
            chat_context_remove(node)
            {
                node.remove();
            }
        }
    };
    vm.createContext(context);
    for (const name of ["chat__get_chat_context_index_by_chat_context_id", "chat__sanitize_string", "chat__peer_state_key", "chat__read_message_payload", "chat__resolve_direct_sender", "chat__ensure_direct_context", "chat__record_direct_activity", "chat__delivery_context", "chat__send_read_command"])
    {
        vm.runInContext(extract("chat/chat.js", name), context);
    }
    const names = ["process_direct_chat_message_from_server", "process_direct_chat_file_metadata_from_server", "process_direct_chat_picture_metadata_from_server", "process_channel_chat_message_from_server", "process_channel_chat_file_metadata_from_server", "process_channel_chat_picture_metadata_from_server"];
    vm.runInContext("var server_msg = {\n" + names.map(function(name)
    {
        return method("msg/messages.js", name);
    }).join("\n") + "\n};", context);
    vm.runInContext("Object.assign(UI, {\n" + method("ui/ui.js", "chat_context_selector_onclick") + "\n" + method("ui/ui.js", "chat_context_remove_onclick") + "\n});", context);
    function frame(type, id = 42)
    {
        return {
            local_delivery: {id: String(id), session_id: "current"},
            local_sender: Object.assign({}, maya),
            message:
            {
                type,
                sender_id: 7,
                sender_username: "replacement name must not win",
                channel_id: 2,
                server_chat_message_id: id,
                file_id: id,
                picture_id: id,
                size: 1024,
                encrypted_size: 1024,
                file_header_decrypted: {name: "notes.txt", size: 5},
                some_json: {type: "direct_chat_message", value: JSON.stringify({value: "hello", font_size: 12})},
                decrypted_value: JSON.stringify({value: "hello channel", font_size: 12})
            }
        };
    }
    return {context, nodes, renders, receipts, badges, reads, keys, closed, transferred, frame};
}

for (const absent of [true, false])
{
    const sam = {client_id: 7, public_key: "sam-key", identity_hash: "SAM", username: "Sam", is_ignored_by_local_client: true};
    const f = fixture(absent ? [] : [sam]);
    f.context.server_msg.process_direct_chat_message_from_server(f.frame("direct_chat_message"));
    assert.ok(f.nodes.has("chat-context-offline-MAYA"));
    assert.equal(f.context.g_chat_context_array.at(-1).peer_public_key, "maya-key");
    assert.deepEqual(f.keys, ["maya-key"]);
    assert.deepEqual(f.renders, [{id: null, name: "Maya"}]);
    assert.deepEqual(f.badges, []);
    assert.deepEqual(f.receipts, []);
    assert.equal(f.context.g_client_list.length, absent ? 0 : 1);
    assert.equal(f.context.chat__delivery_context(f.frame("direct_chat_message")), "identity:MAYA");
    for (const kind of ["file", "picture"])
    {
        const result = f.context.server_msg["process_direct_chat_" + kind + "_metadata_from_server"](f.frame("direct_chat_" + kind + "_metadata", kind === "file" ? 43 : 44));
        assert.equal(result, true);
        assert.equal(f.context.g_chat_message_author_public_keys[kind === "file" ? 43 : 44], "maya-key");
    }
    console.log("PASS: retained direct text/file/picture keeps Maya's author and private history " + (absent ? "after she leaves." : "after Sam reuses her slot."));
}

{
    const f = fixture([{client_id: 7, public_key: "sam-key", username: "Sam"}, {client_id: 9, public_key: "maya-key", identity_hash: "MAYA", username: "Maya"}]);
    f.context.server_msg.process_direct_chat_message_from_server(f.frame("direct_chat_message"));
    assert.ok(f.nodes.has("chat-context-pm-9"));
    assert.deepEqual(f.badges, [9]);
    assert.deepEqual(f.receipts, [9]);
    assert.equal(f.renders[0].id, 9);
    console.log("PASS: retained direct content follows the original identity to its new live slot.");
}

{
    const f = fixture([{client_id: 7, public_key: "sam-key", username: "Sam", is_ignored_by_local_client: true}]);
    for (const kind of ["message", "file_metadata", "picture_metadata"])
    {
        f.context.server_msg["process_channel_chat_" + kind + "_from_server"](f.frame("channel_chat_" + kind));
    }
    assert.equal(f.renders.length, 3);
    assert.ok(f.renders.every(function(sender)
    {
        return sender.id === null && sender.name === "Maya";
    }));
    assert.equal(f.context.g_chat_context_array[0].last_known_message_sender_username, undefined);
    assert.equal(f.context.g_chat_context_array[1].last_known_message_sender_username, "Maya");
    f.context.chat_peer_local_state.set("identity:MAYA", {is_ignored_by_local_client: true});
    for (const kind of ["direct", "channel"])
    {
        assert.equal(f.context.server_msg["process_" + kind + "_chat_file_metadata_from_server"](f.frame(kind + "_chat_file_metadata")), "discarded");
    }
    console.log("PASS: retained channel content uses its original author and destination, with identity-bound ignore rules.");
}

{
    const f = fixture();
    const packet = f.frame("direct_chat_message");
    packet.local_delivery.session_id = "obsolete";
    assert.equal(f.context.server_msg.process_direct_chat_message_from_server(packet), false);
    packet.local_delivery.session_id = "current";
    packet.local_sender.identity_hash = "";
    f.context.server_msg.process_direct_chat_message_from_server(packet);
    const chat = f.context.g_chat_context_array.at(-1);
    const selector = {style: {}, getAttribute(name)
    {
        return name.endsWith("-type") ? "user" : chat.chat_context_id.slice("chat-context-".length);
    }};
    f.context.chat_last_delivery_by_context.set("key:maya-key", "42");
    f.context.UI.chat_context_selector_onclick.call(selector);
    assert.equal(f.context.g_current_chat_context_id, chat.chat_context_id);
    assert.equal(f.context.g_chat_message_receiver_id, -1);
    assert.equal(f.context.g_offline_chat_recipient_identity_hash, "");
    assert.equal(f.context.UI.enabled, false);
    assert.equal(f.reads.at(-1).args.public_key, "maya-key");
    f.context.UI.chat_context_remove_onclick.call(selector, {stopPropagation: noop, srcElement: selector});
    assert.equal(f.nodes.has(chat.chat_context_id), false);
    assert.deepEqual(f.closed, []);
    console.log("PASS: stale retained senders are rejected; legacy key-only history opens read-only and closes without addressing a replacement.");
}

{
    const f = fixture();
    f.context.server_msg.process_direct_chat_message_from_server(f.frame("direct_chat_message"));
    f.context.g_offline_client_list = [{identity_hash: "MAYA", public_key: "maya-key", username: "Maya"}];
    const selector = {style: {}, getAttribute(name)
    {
        return name.endsWith("-type") ? "user" : "offline-MAYA";
    }};
    f.context.UI.chat_context_selector_onclick.call(selector);
    assert.equal(f.context.g_current_chat_context_id, "chat-context-offline-MAYA");
    assert.equal(f.context.g_offline_chat_recipient_identity_hash, "MAYA");
    assert.equal(f.context.UI.enabled, true);
    console.log("PASS: the real offline selector opens a saved identity's conversation with its pinned recipient.");
}

{
    const sam = {client_id: 9, public_key: "sam-key", identity_hash: "SAM", username: "Sam"};
    const f = fixture([sam]);
    f.context.server_msg.process_direct_chat_message_from_server(f.frame("direct_chat_message"));
    f.context.g_offline_client_list = [{identity_hash: "MAYA", public_key: "maya-key", username: "Maya"}];
    let selected = "offline-MAYA";
    const selector = {style: {}, getAttribute(name)
    {
        return name.endsWith("-type") ? "user" : selected;
    }};
    f.context.UI.chat_context_selector_onclick.call(selector);
    assert.equal(f.context.g_offline_chat_recipient_identity_hash, "MAYA");
    const packet = f.frame("direct_chat_message", 45);
    packet.local_sender = sam;
    packet.message.sender_id = 9;
    f.context.server_msg.process_direct_chat_message_from_server(packet);
    f.context.chat__clear_unread_count = noop;
    selected = "user-9";
    f.context.UI.chat_context_selector_onclick.call(selector);
    const sent = [];
    Object.assign(f.context,
    {
        g_is_authenticated: true,
        g_server_policy: {is_typing_indicator_allowed: true},
        g_typing: {last_sent_at: 0},
        TYPING_SEND_INTERVAL_MS: 3000,
        connection__send_message_object(message)
        {
            sent.push(message);
        }
    });
    vm.runInContext(extract("chat/chat.js", "chat__send_typing_indicator"), f.context);
    f.context.chat__send_typing_indicator();
    assert.equal(sent.length, 1);
    assert.equal(sent[0].message.receiver_id, 9);
    assert.equal(f.context.g_offline_chat_recipient_identity_hash, "");
    console.log("PASS: switching from a saved offline person to a live conversation clears offline mode and permits typing notices.");
}

{
    const f = fixture([{client_id: 7, public_key: "maya-key", identity_hash: "MAYA", username: "Maya"}]);
    f.context.android_host__is_ui_only_runtime = function()
    {
        return false;
    };
    const frame = f.frame("direct_chat_message");
    delete frame.local_sender;
    delete frame.local_delivery;
    f.context.server_msg.process_direct_chat_message_from_server(frame);
    assert.ok(f.nodes.has("chat-context-pm-7"));
    assert.deepEqual(f.renders, [{id: 7, name: "Maya"}]);
    assert.deepEqual(f.receipts, [7]);
    console.log("PASS: normal browser direct messages still render and receipt their live sender without a local snapshot.");
}

{
    const f = fixture([{client_id: 7, public_key: "sam-key", username: "Sam"}]);
    const frame = f.frame("direct_chat_message");
    f.context.server_msg.process_direct_chat_message_from_server(frame);
    const sender = f.context.chat__resolve_direct_sender(frame);
    const element = {classList: {add: noop}};
    const state = {author: "maya-key", peer_key: "maya-key", generation: 1};
    const actors = [];
    f.context.chat_replies__find_original = function(context)
    {
        assert.equal(context.id, "chat-context-offline-MAYA");
        return element;
    };
    f.context.chat_reactions = {entries: new Map([[element, state]])};
    f.context.chat_reactions__apply = function(entry, payload, actor)
    {
        actors.push(actor);
    };
    vm.runInContext(extract("chat/chat.js", "chat__receive_message_edit"), f.context);
    vm.runInContext(extract("chat/chat-reactions.js", "chat_reactions__receive"), f.context);
    const payload = {type: "chat_message_edit", message_id: "a".repeat(32), author_public_key: "maya-key", new_message_value: "edited by Maya"};
    assert.equal(f.context.chat__receive_message_edit(payload, frame.message, true, sender), true);
    assert.equal(element.textContent, "edited by Maya");
    payload.type = "chat_reaction";
    payload.emoji = "server:" + "b".repeat(32);
    payload.emoji_name = "wave";
    payload.active = true;
    assert.equal(f.context.chat_reactions__receive(payload, frame.message, true, sender), true);
    assert.deepEqual(actors, ["maya-key"]);
    console.log("PASS: retained encrypted edits and reactions act as the original identity in its own history after slot reuse.");
}
