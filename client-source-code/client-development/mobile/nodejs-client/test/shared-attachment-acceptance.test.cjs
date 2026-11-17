"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const test = require("node:test");
const app = path.resolve(__dirname, "../../../browser/main/scripts/app");

function noop()
{
}

function fixture()
{
    const sent = [];
    const requests = [];
    const cards = new Map();
    const page = vm.createContext({});
    vm.runInContext(fs.readFileSync(path.join(app, "globals.js"), "utf8"), page);
    Object.assign(page,
    {
        console: {log: noop}, clearInterval: noop,
        DBG_WORKER_BOOT_LOG: false,
        g_worker_connection_generation: 4, g_worker_connection_ended: false,
        node_session_id: "session-one",
        g_chat_file_transfers: {}, g_chat_files_by_message_id: {},
        chat_last_delivery_by_context: new Map(),
        android_host__is_ui_only_runtime()
        {
            return true;
        },
        chat__delivery_context()
        {
            return "chat-context-channel-1";
        },
        chat__is_the_user_actually_looking()
        {
            return false;
        },
        document:
        {
            getElementById()
            {
                return null;
            },
            querySelector()
            {
                return null;
            }
        },
        connection__send_message_object(frame)
        {
            sent.push(frame.message);
        },
        g_data_processing_worker:
        {
            postMessage(value)
            {
                requests.push(value);
            }
        }
    });
    for (const file of ["chat/chat-files.js", "msg/messages.js", "msg/dispatch.js"])
    {
        vm.runInContext(fs.readFileSync(path.join(app, file), "utf8"), page);
    }
    page.chat_files__get_chat_file_card_by_key = function(key)
    {
        return cards.get(key) || null;
    };
    page.chat_files__wire_chat_file_download_button = function(card, key)
    {
        card.downloadKey = key;
    };
    function emit(type, message, id = "delivery-one", session = "session-one")
    {
        const delivery = {id, session_id: session};
        page.dispatch__mainthread_onmessage({data:
        {
            type: "data_processing_worker__" + type,
            connection_generation: 4,
            local_delivery: delivery,
            value: {message, local_delivery: delivery}
        }});
    }
    function complete(id = "delivery-one")
    {
        page.dispatch__mainthread_onmessage({data:
        {
            type: "data_processing_worker__local_delivery_accepted", connection_generation: 4,
            value: {id, session_id: "session-one"}
        }});
    }
    function card(key)
    {
        const classes = new Set(["chat-file-receiving"]);
        const name = {};
        const size = {};
        const value =
        {
            isConnected: true, name, size, classes,
            classList:
            {
                contains(item)
                {
                    return classes.has(item);
                },
                remove(item)
                {
                    classes.delete(item);
                },
                add(item)
                {
                    classes.add(item);
                }
            },
            querySelector(selector)
            {
                return selector === ".chat-file-name" ? name : size;
            }
        };
        cards.set(key, value);
        return value;
    }
    return {page, sent, requests, emit, complete, card};
}

test("channel completion is acknowledged only after the decoded file reaches its card", function()
{
    const f = fixture();
    const card = f.card(14);
    const now = Date.now();
    f.page.received_files = [{file_id: 14, file_content_base64: "ciphertext", expected_size: 10,
        timestamp_started: now, timestamp_last_received: now}];
    f.emit("file_receive_completed_from_server", {type: "file_receive_completed", server_chat_message_id: 14,
        receive_type: "channel_chat_file", sender_id: 7});
    assert.equal(f.sent.length, 0);
    assert.equal(f.requests.length, 1);
    assert.equal(f.requests[0].local_delivery.id, "delivery-one");
    assert.equal(f.requests[0].connection_generation, 4);
    f.complete();
    assert.equal(f.sent.length, 0, "A premature marker cannot accept pending decryption");
    f.emit("channel_chat_file", {file_id: 14, file: {name: "hello.txt", size: 5, base64: "aGVsbG8="}});
    assert.equal(card.name.textContent, "hello.txt");
    assert.equal(card.downloadKey, 14);
    assert.equal(card.classes.has("chat-file-receiving"), false);
    f.complete();
    assert.equal(f.sent.length, 1);
    assert.equal(f.sent[0].type, "local_delivery_ack");
    assert.notEqual(f.sent[0].accepted, false);
});

test("missing buffers, file cards and picture placeholders cannot accept retained content", function()
{
    for (const [type, message] of
    [
        ["file_receive_completed_from_server", {server_chat_message_id: 14, receive_type: "channel_chat_file"}],
        ["direct_chat_file", {file_id: 14, file: {name: "hello.txt", size: 5, base64: "aGVsbG8="}}],
        ["channel_chat_file", {file_id: 14, file: {name: "hello.txt", size: 5, base64: "aGVsbG8="}}],
        ["direct_chat_picture", {picture_id: 14, decrypted_base64_picture: "data:image/png;base64,AA=="}],
        ["channel_chat_picture", {picture_id: 14, decrypted_base64_picture: "data:image/png;base64,AA=="}],
        ["chat_file_decrypt_failed", {file_id: 14}],
        ["chat_picture_decrypt_failed", {picture_id: 14}]
    ])
    {
        const f = fixture();
        f.emit(type, message);
        f.complete();
        assert.equal(f.sent.length, 1, type);
        assert.equal(f.sent[0].accepted, false, type);
    }
});

test("intentional discard is explicit and stale-session output cannot populate current cards", function()
{
    const f = fixture();
    const card = f.card(14);
    card.classes.add("chat-file-deleted");
    f.emit("direct_chat_file", {file_id: 14, file: {name: "deleted.txt", base64: "AA=="}});
    f.complete();
    assert.notEqual(f.sent[0].accepted, false);
    assert.equal(card.downloadKey, undefined);
    f.sent.length = 0;
    f.emit("direct_chat_file", {file_id: 14, file: {name: "stale.txt", base64: "AA=="}}, "stale", "old-session");
    assert.equal(f.sent.length, 0);
    assert.equal(card.name.textContent, undefined);
    f.page.server_msg.process_direct_chat_file_metadata_from_server = function()
    {
        return "discarded";
    };
    f.emit("direct_chat_file_metadata", {server_chat_message_id: 15}, "ignored");
    f.complete("ignored");
    assert.equal(f.sent[0].discard_attachment, true);
});

test("discarded text uses a normal consumption acknowledgement instead of an attachment cancellation", function()
{
    for (const kind of ["direct", "channel", "offline"])
    {
        const f = fixture();
        f.page.server_msg["process_" + kind + "_chat_message_from_server"] = function()
        {
            return "discarded";
        };
        f.emit(kind + "_chat_message", {server_chat_message_id: 15});
        f.complete();
        assert.equal(f.sent.length, 1);
        assert.equal(f.sent[0].discard_attachment, false, "Text must not use the host's attachment-only cancellation");
        assert.notEqual(f.sent[0].accepted, false);
    }
});
