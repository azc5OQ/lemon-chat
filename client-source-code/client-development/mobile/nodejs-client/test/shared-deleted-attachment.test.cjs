"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const app = path.resolve(__dirname, "../../../browser/main/scripts/app");

function noop()
{
}

function fixture()
{
    const classes = new Set();
    const picture =
    {
        src: "original-picture", alt: "Original image", title: "Original image",
        removeEventListener: noop,
        classList:
        {
            contains(value)
            {
                return classes.has(value);
            },
            add(...values)
            {
                for (const value of values)
                {
                    classes.add(value);
                }
            },
            remove(...values)
            {
                for (const value of values)
                {
                    classes.delete(value);
                }
            }
        }
    };
    let completed = 0;
    const page = vm.createContext(
    {
        console, g_chat_message_author_public_keys: {14: "author"},
        g_chat_file_transfers: {}, g_chat_files_by_message_id: {},
        UI: {single_chat_message_onrightclick: noop}, chat_reactions__forget: noop,
        chat__scroll_chat_to_end: noop,
        document:
        {
            getElementById(id)
            {
                return id === "chat-picture-img-14" ? picture : null;
            },
            querySelector(selector)
            {
                return selector.startsWith(".chat-picture-img[") ? picture : null;
            }
        }
    });
    for (const file of ["chat/chat-files.js", "msg/messages.js"])
    {
        vm.runInContext(fs.readFileSync(path.join(app, file), "utf8"), page);
    }
    page.chat_files__finish_chat_picture_progress = function()
    {
        completed++;
    };
    page.server_msg.process_chat_message_delete_from_server({message: {chat_message_id: 14, requester_public_key: "author"}});
    return {page, picture, classes, completed: function()
    {
        return completed;
    }};
}

test("late direct and channel picture bodies settle without restoring a deleted image", function()
{
    for (const handler of ["process_direct_chat_picture_from_server", "process_channel_chat_picture_from_server"])
    {
        const f = fixture();
        const deleted_source = f.picture.src;
        assert.equal(f.classes.has("chat-picture-deleted"), true);
        assert.equal(f.page.server_msg[handler]({message: {picture_id: 14,
            decrypted_base64_picture: "data:image/png;base64,AA=="}}), true);
        assert.equal(f.picture.src, deleted_source);
        assert.equal(f.picture.alt, "Image deleted");
        assert.equal(f.classes.has("chat-picture-img-default"), true);
        assert.equal(f.completed(), 1);
    }
});

test("late attachment failures preserve deleted labels and settle receive state", function()
{
    const f = fixture();
    f.page.received_files = [{file_id: 14}];
    f.page.g_chat_file_transfers[14] = {};
    assert.equal(f.page.server_msg.process_chat_picture_decrypt_failed_from_server({message: {picture_id: 14}}), true);
    assert.equal(f.picture.alt, "Image deleted");
    assert.equal(f.page.received_files.length, 0);
    assert.equal(f.page.g_chat_file_transfers[14], undefined);
    assert.equal(f.completed(), 1);

    const status = {textContent: "deleted"};
    const card =
    {
        classList:
        {
            contains(value)
            {
                return value === "chat-file-deleted";
            },
            add: noop, remove: noop
        },
        querySelector()
        {
            return status;
        }
    };
    f.page.chat_files__get_chat_file_card_by_key = function()
    {
        return card;
    };
    f.page.received_files = [{file_id: 15}];
    f.page.g_chat_file_transfers[15] = {};
    assert.equal(f.page.server_msg.process_chat_file_decrypt_failed_from_server({message: {file_id: 15}}), true);
    assert.equal(status.textContent, "deleted");
    assert.equal(f.page.received_files.length, 0);
    assert.equal(f.page.g_chat_file_transfers[15], undefined);
});
