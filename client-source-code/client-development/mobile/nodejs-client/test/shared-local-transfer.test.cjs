"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const test = require("node:test");
const app = path.resolve(__dirname, "../../../browser/main/scripts/app");
const chunkBytes = 48 * 1024;
const maximumBytes = 48 * 1024 * 1024 + 64 * 1024;
const aggregateBytes = 64 * 1024 * 1024 + 8 * 64 * 1024;
function read(name)
{
    return fs.readFileSync(path.join(app, name), "utf8");
}
function noop()
{
}
function fixture(overrides = {})
{
    const acknowledgements = [];
    const failures = [];
    const frames = [];
    const timers = [];
    const scope = {session_id: "session-one", generation: 4};
    const context = vm.createContext({TextDecoder, atob, btoa, Uint8Array});
    vm.runInContext(read("workers/local-transfer.js"), context);
    const receiver = context.local_transfer__create_receiver(
    {
        getScope()
        {
            return scope;
        },
        setTimer(callback, milliseconds)
        {
            const timer = {callback, milliseconds, cleared: false};
            timers.push(timer);
            return timer;
        },
        clearTimer(timer)
        {
            timer.cleared = true;
        },
        onAck(value)
        {
            acknowledgements.push(value);
        },
        onFailure(metadata, reason)
        {
            failures.push({metadata, reason});
        },
        onFrame(text, value)
        {
            frames.push({text, value});
        },
        ...overrides
    });
    return {receiver, acknowledgements, failures, frames, timers, scope};
}
function begin(target, id, bytes, metadata = {})
{
    const message =
    {
        type: "local_transfer_begin", transfer_id: id, session_id: target.scope.session_id,
        total_bytes: bytes.length ?? bytes, total_chunks: Math.ceil((bytes.length ?? bytes) / chunkBytes), ...metadata
    };
    target.receiver.receive(message);
    return message;
}
function chunk(target, id, bytes, index)
{
    const message =
    {
        type: "local_transfer_chunk", transfer_id: id, session_id: target.scope.session_id, index,
        data: bytes.subarray(index * chunkBytes, (index + 1) * chunkBytes).toString("base64")
    };
    target.receiver.receive(message);
    return message;
}
function payload(extra = {})
{
    return Buffer.from(JSON.stringify(
    {
        message: {type: "direct_chat_file_decrypted", file_id: 14, file: {data: "a".repeat(chunkBytes * 2)}, ...extra},
        local_delivery: {id: "delivery-one", session_id: "session-one"}
    }));
}

test("reassembles exact file JSON, including a Unicode sequence split between chunks", function()
{
    const f = fixture();
    const prefix = '{"message":{"type":"direct_chat_file_decrypted","file_id":14,"file":{"name":"';
    const text = prefix + "a".repeat(chunkBytes - Buffer.byteLength(prefix) - 1)
        + '🙂ž","data":"AAECA/7/"}},"local_delivery":{"id":"delivery-one","session_id":"session-one"}}';
    const bytes = Buffer.from(text);
    begin(f, "unicode", bytes, {content_type: "direct_chat_file_decrypted", file_id: 14});
    assert.equal(f.acknowledgements[0].index, -1);
    assert.equal(f.acknowledgements[0].accepted, true);
    const first = chunk(f, "unicode", bytes, 0);
    f.receiver.receive(first);
    assert.equal(f.frames.length, 0);
    chunk(f, "unicode", bytes, 1);
    assert.equal(f.frames.length, 1);
    assert.equal(f.frames[0].text, text);
    assert.equal(f.frames[0].value.message.file.data, "AAECA/7/");
    assert.equal(f.frames[0].value.local_delivery.id, "delivery-one");
    assert.equal(f.receiver.readState().reserved_bytes, 0);
    assert.ok(f.timers.every(function(timer)
    {
        return timer.cleared;
    }));
    chunk(f, "unicode", bytes, 1);
    assert.equal(f.frames.length, 1, "A final duplicate never applies content twice");
});

test("checks per-transfer, aggregate, and count limits before allocating", function()
{
    const f = fixture();
    for (const size of [0, -1, 0.5, Number.MAX_SAFE_INTEGER, maximumBytes + 1])
    {
        begin(f, "invalid", size);
        assert.equal(f.receiver.readState().reserved_bytes, 0);
        assert.equal(f.acknowledgements.at(-1).accepted, false);
    }
    begin(f, "largest", maximumBytes);
    begin(f, "remaining", aggregateBytes - maximumBytes);
    assert.equal(f.receiver.readState().reserved_bytes, aggregateBytes);
    begin(f, "overflow", 1);
    assert.equal(f.acknowledgements.at(-1).reason, "receiver_busy");
    f.receiver.cancelAll("reset");
    for (let index = 0; index < 8; index++)
    {
        begin(f, "small-" + index, 1);
    }
    begin(f, "ninth", 1);
    assert.equal(f.receiver.readState().active_count, 8);
    assert.equal(f.acknowledgements.at(-1).reason, "receiver_busy");
    f.receiver.cancelAll("reset");
    assert.equal(f.receiver.readState().reserved_bytes, 0);
});

test("reordered, conflicting, and malformed chunks release their buffers", function()
{
    const bytes = payload();
    const f = fixture();
    begin(f, "order", bytes);
    chunk(f, "order", bytes, 1);
    assert.equal(f.acknowledgements.at(-1).reason, "invalid_sequence");
    begin(f, "conflict", bytes);
    const first = chunk(f, "conflict", bytes, 0);
    f.receiver.receive({...first, data: Buffer.alloc(chunkBytes, 0).toString("base64")});
    assert.equal(f.acknowledgements.at(-1).reason, "conflicting_chunk");
    for (const data of ["AA==", "!".repeat(65536), "A".repeat(65535) + "="])
    {
        begin(f, "malformed", bytes);
        f.receiver.receive({...first, transfer_id: "malformed", data});
        assert.equal(f.acknowledgements.at(-1).reason, "invalid_chunk");
    }
    assert.equal(f.receiver.readState().reserved_bytes, 0);
    assert.equal(f.frames.length, 0);
});

test("strict UTF-8, JSON, nested transfer, delivery session, and metadata validation", function()
{
    const f = fixture();
    const inputs =
    [
        Buffer.from([0xc0, 0x80]),
        Buffer.from("{"),
        Buffer.from('{"message":{"type":"local_transfer_begin"}}'),
        Buffer.from('{"message":{"type":"direct_chat_file_decrypted","file_id":15}}'),
        Buffer.from('{"message":{"type":"direct_chat_file_decrypted","file_id":14},"local_delivery":{"session_id":"old"}}')
    ];
    for (const bytes of inputs)
    {
        begin(f, "invalid-frame", bytes, {content_type: "direct_chat_file_decrypted", file_id: 14});
        chunk(f, "invalid-frame", bytes, 0);
        assert.equal(f.acknowledgements.at(-1).reason, "invalid_frame");
        assert.equal(f.receiver.readState().reserved_bytes, 0);
    }
    assert.equal(f.failures.length, inputs.length);
    assert.equal(f.frames.length, 0);
});

test("expiry is absolute; old sessions, generations, and timers cannot damage replacements", function()
{
    const f = fixture();
    const bytes = payload();
    const metadata = {content_type: "direct_chat_file_decrypted", file_id: 14};
    const initial = begin(f, "same-id", bytes, metadata);
    f.receiver.receive(initial);
    chunk(f, "same-id", bytes, 0);
    assert.equal(f.timers.length, 1);
    assert.equal(f.timers[0].milliseconds, 120000);
    f.scope.session_id = "replacement";
    f.scope.generation++;
    begin(f, "same-id", bytes, metadata);
    f.timers[0].callback();
    assert.equal(f.receiver.readState().active_count, 1);
    f.receiver.receive({type: "local_transfer_cancel", transfer_id: "same-id", session_id: "session-one"});
    f.receiver.receive({type: "local_transfer_chunk", transfer_id: "same-id", session_id: "session-one", index: 0, data: ""});
    assert.equal(f.receiver.readState().active_count, 1);
    f.timers.at(-1).callback();
    assert.equal(f.receiver.readState().reserved_bytes, 0);
    assert.equal(f.failures.at(-1).reason, "expired");
    begin(f, "generation", bytes, metadata);
    f.scope.generation++;
    chunk(f, "generation", bytes, 0);
    assert.equal(f.receiver.readState().reserved_bytes, 0);
    assert.equal(f.failures.length, 1, "Stale generation failures never alter the replacement page");
});

test("callback failures cannot retain buffers and byte acknowledgement remains separate from model acceptance", function()
{
    const bytes = Buffer.from('{"message":{"type":"direct_chat_file_decrypted","file_id":14}}');
    const f = fixture(
    {
        onFrame()
        {
            throw new Error("model failure");
        }
    });
    begin(f, "callback", bytes, {content_type: "direct_chat_file_decrypted", file_id: 14});
    chunk(f, "callback", bytes, 0);
    assert.equal(f.receiver.readState().reserved_bytes, 0);
    assert.equal(f.acknowledgements.at(-1).accepted, true);
    assert.equal(f.failures.at(-1).reason, "processing_failed");
    const g = fixture(
    {
        onAck()
        {
            throw new Error("port closed");
        }
    });
    begin(g, "closed-port", bytes);
    assert.equal(g.receiver.readState().reserved_bytes, 0);
});

function workerFixture()
{
    const outputs = [];
    const timers = [];
    const worker = vm.createContext(
    {
        console, performance, TextDecoder, atob, btoa, Uint8Array,
        setTimeout(callback)
        {
            timers.push(callback);
            return timers.length;
        },
        clearTimeout: noop, clearInterval: noop,
        g_loopback_port: 1234, g_is_authenticated: false, g_identity_revision: 1,
        g_rsa_public_key_string: "public", DBG_WORKER_BOOT_LOG: false,
        keys__clear_channel_keys: noop, utils__custom_log: noop,
        keys__decrypt_message_metadata(value)
        {
            return value;
        },
        global:
        {
            postMessage(value)
            {
                outputs.push(value);
            }
        }
    });
    vm.runInContext(read("workers/local-transfer.js") + read("workers/workers.js"), worker);
    worker.workers__apply_keepalive_policy = noop;
    worker.workers__apply_channel_key_policy = noop;
    function frame(message, delivery, local = true)
    {
        worker.workers__mainthread__process_received_websocket_message_continue(
        {
            data: {is_plaintext: local, value: JSON.stringify({message, local_delivery: delivery})}
        });
    }
    frame({type: "local_session_state", value: {session_id: "session-one", public_key_string: "public"}});
    outputs.length = 0;
    return {worker, frame, outputs, timers};
}

test("worker accepts same-session prepared content during recovery and separately acknowledges model delivery", function()
{
    const f = workerFixture();
    const bytes = payload();
    f.frame({type: "local_transfer_begin", transfer_id: "file", session_id: "session-one", total_bytes: bytes.length,
        total_chunks: Math.ceil(bytes.length / chunkBytes), content_type: "direct_chat_file_decrypted", file_id: 14});
    for (let index = 0; index < Math.ceil(bytes.length / chunkBytes); index++)
    {
        f.frame({type: "local_transfer_chunk", transfer_id: "file", session_id: "session-one", index,
            data: bytes.subarray(index * chunkBytes, (index + 1) * chunkBytes).toString("base64")});
    }
    const content = f.outputs.filter(function(event)
    {
        return event.type === "data_processing_worker__direct_chat_file";
    });
    assert.equal(content.length, 1);
    assert.equal(content[0].value.message.file.data, "a".repeat(chunkBytes * 2));
    assert.equal(content[0].local_delivery.id, "delivery-one");
    const marker = f.outputs.at(-1);
    assert.equal(marker.type, "data_processing_worker__local_delivery_accepted");
    assert.equal(marker.value.id, "delivery-one");
    assert.equal(f.worker.g_is_authenticated, false);
    assert.ok(f.outputs.filter(function(event)
    {
        return event.type === "data_processing_worker__local_transfer_ack";
    }).every(function(event)
    {
        return event.local_delivery == null;
    }));
    const before = f.outputs.length;
    f.frame({type: "direct_chat_file_decrypted", file_id: 14, file: {}}, {id: "old", session_id: "old"});
    assert.equal(f.outputs.length, before);
});

test("prepared channel file and picture types pass the bounded local transfer receiver", function()
{
    for (const kind of ["file", "picture"])
    {
        const f = workerFixture();
        const content = kind === "file" ? {file: {base64: "a".repeat(chunkBytes * 2)}}
            : {decrypted_base64_picture: "data:image/png;base64," + "a".repeat(chunkBytes * 2)};
        const bytes = Buffer.from(JSON.stringify(
        {
            message: {type: "channel_chat_" + kind + "_decrypted", [kind + "_id"]: 14, ...content},
            local_delivery: {id: "channel-" + kind, session_id: "session-one"}
        }));
        f.frame({type: "local_transfer_begin", transfer_id: kind, session_id: "session-one", total_bytes: bytes.length,
            total_chunks: Math.ceil(bytes.length / chunkBytes), content_type: "channel_chat_" + kind + "_decrypted", [kind + "_id"]: 14});
        for (let index = 0; index < Math.ceil(bytes.length / chunkBytes); index++)
        {
            f.frame({type: "local_transfer_chunk", transfer_id: kind, session_id: "session-one", index,
                data: bytes.subarray(index * chunkBytes, (index + 1) * chunkBytes).toString("base64")});
        }
        assert.ok(f.outputs.some(function(event)
        {
            return event.type === "data_processing_worker__channel_chat_" + kind
                && event.local_delivery.id === "channel-" + kind;
        }));
        assert.equal(f.outputs.at(-1).type, "data_processing_worker__local_delivery_accepted");
    }
});

test("worker rejects remote forged transfers and terminal events before forwarding; reset releases all receives", function()
{
    const f = workerFixture();
    f.worker.dpw_forward_frames = true;
    for (const type of ["local_transfer_begin", "chat_file_decrypt_failed", "chat_picture_decrypt_failed"])
    {
        f.frame({type, transfer_id: "remote", session_id: "session-one", total_bytes: 2, total_chunks: 1}, undefined, false);
    }
    assert.equal(f.outputs.length, 0);
    assert.equal(f.worker.dpw_local_transfer_receiver, null);
    for (const kind of ["file", "picture"])
    {
        f.frame({type: "local_transfer_begin", transfer_id: kind, session_id: "session-one", total_bytes: 2, total_chunks: 1,
            content_type: "direct_chat_" + kind + "_decrypted", [kind + "_id"]: 14});
    }
    f.worker.workers__data_processing_worker_onmessage({data: {type: "mainthread_reset_data"}});
    assert.equal(f.worker.dpw_local_transfer_receiver.readState().reserved_bytes, 0);
    for (const kind of ["file", "picture"])
    {
        assert.ok(f.outputs.some(function(event)
        {
            return event.type === "data_processing_worker__chat_" + kind + "_decrypt_failed"
                && event.value.message[kind + "_id"] === 14;
        }));
    }
});

test("channel attachment completion waits for its decoded result before model acknowledgement", function()
{
    for (const kind of ["file", "picture"])
    {
        const f = workerFixture();
        f.worker.g_is_authenticated = true;
        const delivery = {id: "channel-" + kind, session_id: "session-one"};
        f.frame({type: "file_receive_completed", receive_type: "channel_chat_" + kind,
            server_chat_message_id: 14, sender_id: 3}, delivery);
        const completion = f.outputs.find(function(event)
        {
            return event.type === "data_processing_worker__file_receive_completed_from_server";
        });
        assert.equal(completion.local_delivery.id, delivery.id);
        assert.equal(f.outputs.some(function(event)
        {
            return event.type === "data_processing_worker__local_delivery_accepted";
        }), false, "Enqueuing a decryption request is not acceptance of its content");
        f.outputs.length = 0;
        f.worker.chat_files__decrypt_channel_chat_file_body = function()
        {
            return {name: "download.txt", base64: "aGVsbG8="};
        };
        f.worker.keys__decrypt_channel_contents = function()
        {
            return "data:image/png;base64,AA==";
        };
        f.worker.chat__sanitize_image_data_url = function(value)
        {
            return value;
        };
        f.worker.workers__data_processing_worker_onmessage({data:
        {
            type: "mainthread__process_encrypted_channel_chat_" + kind + "_data",
            [kind + "_id"]: 14, sender_id: 3, message_raw: "ciphertext", local_delivery: delivery
        }});
        assert.equal(f.outputs.length, 2);
        assert.equal(f.outputs[0].type, "data_processing_worker__channel_chat_" + kind);
        assert.equal(f.outputs[0].local_delivery.id, delivery.id);
        assert.equal(f.outputs[1].type, "data_processing_worker__local_delivery_accepted");
        assert.equal(f.outputs[1].value.id, delivery.id);
    }
});

test("stale deferred attachment results cannot decrypt or acknowledge a replacement session", function()
{
    const f = workerFixture();
    f.worker.chat_files__decrypt_channel_chat_file_body = function()
    {
        assert.fail("Stale session work must be rejected before decryption");
    };
    f.worker.workers__data_processing_worker_onmessage({data:
    {
        type: "mainthread__process_encrypted_channel_chat_file_data", file_id: 14,
        local_delivery: {id: "old-file", session_id: "old-session"}
    }});
    assert.equal(f.outputs.length, 0);
});

test("failed picture decryption emits a terminal result and reaches a detached Android page", function()
{
    for (const kind of ["direct", "channel"])
    {
        for (const failure of ["malformed", "unavailable", "throws"])
        {
            const f = workerFixture();
            f.worker.g_loopback_port = 0;
            f.worker.dpw_forward_frames = true;
            f.worker.g_my_rsa_key_object = {};
            f.worker.keys__open_direct_content = function()
            {
                if (failure === "throws")
                {
                    throw new Error("synthetic decryption failure");
                }
                return null;
            };
            f.worker.keys__decrypt_channel_contents = function()
            {
                if (failure === "throws")
                {
                    throw new Error("synthetic decryption failure");
                }
                return null;
            };
            f.worker.chat__sanitize_image_data_url = function(value)
            {
                return value == null ? "" : value;
            };
            assert.doesNotThrow(function()
            {
                f.worker.workers__data_processing_worker_onmessage({data:
                {
                    type: "mainthread__process_encrypted_" + kind + "_chat_picture_data",
                    picture_id: 14, sender_id: 3, message_raw: failure === "malformed" ? "{" : "{}"
                }});
            });
            const forwarded = f.outputs.find(function(event)
            {
                return event.type === "decrypted_frame";
            });
            assert.equal(JSON.parse(forwarded.value).message.type, "chat_picture_decrypt_failed");
            assert.equal(f.outputs.at(-1).type, "data_processing_worker__chat_picture_decrypt_failed");
            assert.equal(f.outputs.at(-1).value.message.picture_id, 14);
        }
    }
    const f = workerFixture();
    f.worker.keys__decrypt_channel_contents = function()
    {
        return null;
    };
    f.worker.chat__sanitize_image_data_url = function()
    {
        return "";
    };
    f.worker.workers__data_processing_worker_onmessage({data:
    {
        type: "mainthread__process_encrypted_channel_chat_picture_data", picture_id: 14,
        local_delivery: {id: "failed-picture", session_id: "session-one"}
    }});
    assert.equal(f.outputs[0].type, "data_processing_worker__chat_picture_decrypt_failed");
    assert.equal(f.outputs[0].local_delivery.id, "failed-picture");
    assert.equal(f.outputs[1].type, "data_processing_worker__local_delivery_accepted");
});

test("main dispatcher forwards transfer acknowledgements and routes file/picture terminal cleanup", function()
{
    const sent = [];
    const failed = [];
    const page = vm.createContext(
    {
        DBG_WORKER_BOOT_LOG: false,
        connection__send_message_object(value)
        {
            sent.push(value);
        },
        server_msg:
        {
            process_chat_file_decrypt_failed_from_server(value)
            {
                failed.push(value.message.file_id);
            },
            process_chat_picture_decrypt_failed_from_server(value)
            {
                failed.push(value.message.picture_id);
            }
        }
    });
    vm.runInContext(read("msg/dispatch.js"), page);
    const ack = {message: {type: "local_transfer_ack", transfer_id: "one", session_id: "session-one", index: 0, accepted: true}};
    page.dispatch__mainthread_onmessage({data: {type: "data_processing_worker__local_transfer_ack", value: ack}});
    assert.equal(sent[0], ack);
    page.dispatch__mainthread_onmessage({data: {type: "data_processing_worker__chat_file_decrypt_failed", value: {message: {file_id: 11}}}});
    page.dispatch__mainthread_onmessage({data: {type: "data_processing_worker__chat_picture_decrypt_failed", value: {message: {picture_id: 12}}}});
    assert.deepEqual(failed, [11, 12]);
});

test("terminal file and picture failures clear receiving memory and show a persistent failure state", function()
{
    const fileStatus = {};
    const pictureLabel = {};
    const picture = {classList:
    {
        contains()
        {
            return false;
        }
    }};
    const classes = new Set(["chat-file-receiving", "imgnotsentyet"]);
    let ringRemoved = false;
    const card =
    {
        classList:
        {
            contains(value)
            {
                return classes.has(value);
            },
            remove(value)
            {
                classes.delete(value);
            },
            add(value)
            {
                classes.add(value);
            }
        },
        querySelector()
        {
            return fileStatus;
        }
    };
    const progress =
    {
        querySelector(selector)
        {
            if (selector === ".chat-picture-progress-ring")
            {
                return {
                    remove()
                    {
                        ringRemoved = true;
                    }
                };
            }
            return pictureLabel;
        },
        setAttribute: noop
    };
    const page = vm.createContext(
    {
        g_chat_file_transfers: {11: {}, 12: {}, 13: {}},
        document:
        {
            querySelector()
            {
                return progress;
            },
            getElementById()
            {
                return picture;
            }
        }
    });
    vm.runInContext(read("chat/chat-files.js") + read("msg/messages.js"), page);
    page.received_files = [{file_id: 11}, {file_id: 12}, {file_id: 13}];
    page.chat_files__get_chat_file_card_by_key = function()
    {
        return card;
    };
    for (let duplicate = 0; duplicate < 2; duplicate++)
    {
        page.server_msg.process_chat_file_decrypt_failed_from_server({message: {file_id: 11, reason: "could not receive this file"}});
        page.server_msg.process_chat_picture_decrypt_failed_from_server({message: {picture_id: 12}});
    }
    assert.equal(page.received_files.length, 1);
    assert.equal(page.received_files[0].file_id, 13);
    assert.deepEqual(Object.keys(page.g_chat_file_transfers), ["13"]);
    assert.equal(fileStatus.textContent, "could not receive this file");
    assert.equal(classes.has("chat-file-receiving"), false);
    assert.equal(classes.has("chat-file-failed"), true);
    assert.equal(ringRemoved, true);
    assert.equal(pictureLabel.textContent, "could not receive this image");
    assert.equal(picture.alt, "Image could not be received");
});
