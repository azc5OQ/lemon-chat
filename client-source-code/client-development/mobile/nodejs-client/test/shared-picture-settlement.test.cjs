"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.resolve(__dirname, "../../../browser/main/scripts/app/chat/chat.js"), "utf8");
const nodes = new Map();
const notices = [];
function noop()
{
}
function node(id)
{
    if (!nodes.has(id))
    {
        nodes.set(id, {style: {}, value: "", classList: {add: noop}});
    }
    return nodes.get(id);
}
const context =
{
    picture_preparation: null,
    upload_generation: 0,
    upload_ack_timer: null,
    g_is_file_being_uploaded: true,
    g_base64_picture_string_to_send: "",
    g_pending_chat_file: null,
    g_current_chat_context_id: "chat-context-pm-7",
    clearTimeout: noop,
    connection__cancel_upload_send: noop,
    document: {getElementById: node, querySelector: node},
    UI: {},
    utils__custom_alert(message)
    {
        notices.push(message);
    }
};
vm.createContext(context);
for (const name of ["chat__release_file_upload_lock", "chat__picture_preparation_failed", "chat__picture_preparation_succeeded"])
{
    const found = source.match(new RegExp("^function " + name + "\\([^\\n]*\\)\\r?\\n\\{[\\s\\S]*?^\\}", "m"));
    assert.ok(found, name);
    vm.runInContext(found[0], context);
}
function operation(id)
{
    return {operation_id: id, local_message_id: id, context_id: "chat-context-pm-7", picture: "data:image/png;base64,AA==", text: "draft"};
}

context.picture_preparation = operation(2);
context.chat__picture_preparation_failed({operation_id: 1});
assert.equal(context.g_is_file_being_uploaded, true);
assert.equal(context.picture_preparation.operation_id, 2);
assert.equal(notices.length, 0);
assert.equal(context.chat__picture_preparation_succeeded({operation_id: 1}), false);

context.chat__picture_preparation_failed({operation_id: 2});
assert.equal(context.g_is_file_being_uploaded, false);
assert.equal(context.picture_preparation, null);
assert.equal(context.g_base64_picture_string_to_send, "data:image/png;base64,AA==");
assert.equal(node("chat-input-container-text-input").value, "draft");
assert.equal(node("upload-progress-overlay").style.display, "none");
assert.equal(notices.length, 1);
context.chat__picture_preparation_failed({operation_id: 2});
assert.equal(notices.length, 1);

context.picture_preparation = operation(3);
context.g_current_chat_context_id = "chat-context-pm-8";
context.g_base64_picture_string_to_send = "new-picture";
node("chat-input-container-text-input").value = "new draft";
context.chat__picture_preparation_failed({operation_id: 3});
assert.equal(context.g_base64_picture_string_to_send, "new-picture");
assert.equal(node("chat-input-container-text-input").value, "new draft");
context.picture_preparation = operation(4);
assert.equal(context.chat__picture_preparation_succeeded({operation_id: 4}), true);
assert.equal(context.picture_preparation, null);
console.log("PASS: picture failures settle once, preserve retry data and cannot overwrite a newer operation or conversation.");
