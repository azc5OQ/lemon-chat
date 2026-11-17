"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const {once} = require("node:events");
const {createRequire} = require("node:module");
const adapter = require("../websocket-adapter.js");
const loopback = require("../loopback-ui-server.js");

function wait_for(predicate)
{
    return new Promise(function(resolve, reject)
    {
        const deadline = Date.now() + 3000;
        const timer = setInterval(function()
        {
            if (predicate())
            {
                clearInterval(timer);
                resolve();
            }
            else if (Date.now() > deadline)
            {
                clearInterval(timer);
                reject(new Error("Timed out waiting for local transfer"));
            }
        }, 5);
    });
}

async function fixture(t, runtime = loopback)
{
    const listeners = {};
    const state = {g_is_client_list_retrieved: true, g_client_list: [], g_channel_list: [],
        g_groups: [], g_icons: [], g_local_username: "Me"};
    const session = {authenticated: true, session_id: "session-1", public_key_string: "public-1",
        identity_revision: 1, local_client_id: 1, current_channel_id: 1};
    const bundle =
    {
        read_state: function()
        {
            return state;
        },
        get_ui_session_snapshot: function()
        {
            return session;
        },
        get_auth_frame: function()
        {
            return JSON.stringify({message: {type: "authentication_status", status: true}});
        },
        get_connection_status: function()
        {
            return {state: "connected"};
        },
        set_frame_listener: function(callback)
        {
            listeners.frame = callback;
        },
        set_on_message_processed: function(callback)
        {
            listeners.processed = callback;
        },
        set_connection_status_listener: function()
        {
        },
        set_unread_listener: function()
        {
        },
        node_fail_delivery: function(reason)
        {
            throw new Error(reason);
        }
    };
    let host;
    const endpoint = await new Promise(function(resolve)
    {
        host = runtime.start(bundle, function(port, token)
        {
            resolve({port, token});
        });
    });
    t.after(function()
    {
        host.close();
    });
    const page = await attach_page(t, endpoint, session);
    return Object.assign(page, {listeners, state, session, endpoint,
        attach: function(options)
        {
            return attach_page(t, endpoint, session, options);
        }
    });
}

async function attach_page(t, endpoint, session, options = {})
{
    const socket = new adapter.WebSocketClient("ws://127.0.0.1:" + endpoint.port);
    const delivered = [];
    const received = [];
    const source = fs.readFileSync(path.resolve(__dirname, "../../../browser/main/scripts/app/workers/local-transfer.js"), "utf8");
    const receiver_factory = vm.runInNewContext(source + "\nlocal_transfer__create_receiver;",
        {TextDecoder, Uint8Array, atob, btoa});
    function accept(frame)
    {
        delivered.push(frame.message);
        received.push(frame);
        if (frame.local_delivery && (!options.accept || options.accept(frame)))
        {
            socket.send(JSON.stringify({message: Object.assign({type: "local_delivery_ack"}, frame.local_delivery)}));
        }
    }
    const receiver = receiver_factory(
    {
        getScope: function()
        {
            return {session_id: session.session_id, generation: 1};
        },
        setTimer: setTimeout,
        clearTimer: clearTimeout,
        onAck: function(message)
        {
            socket.send(JSON.stringify({message}));
        },
        onFailure: function(metadata, reason)
        {
            assert.fail(reason);
        },
        onFrame: function(text, frame)
        {
            accept(JSON.parse(text));
        }
    });
    t.after(function()
    {
        receiver.cancelAll();
    });
    socket.onmessage = function(event)
    {
        const frame = JSON.parse(event.data);
        if (frame.message.type.startsWith("local_transfer_"))
        {
            receiver.receive(frame.message);
        }
        else
        {
            accept(frame);
        }
    };
    await once(socket, "open");
    socket.send(endpoint.token);
    await wait_for(function()
    {
        return delivered.some(function(message)
        {
            return message.type === "icon_list";
        });
    });
    delivered.length = 0;
    return {delivered, received, socket};
}

test("a later edit cannot overtake a large message on the local chunk transport", async function(t)
{
    const f = await fixture(t);
    const original = {message: {type: "channel_chat_message", channel_id: 1, client_id: 3,
        server_chat_message_id: 40, value: "A".repeat(200000)}};
    const edit = {message: {type: "chat_message_edit", channel_id: 1, server_chat_message_id: 40, value: "edited"}};
    f.listeners.frame(JSON.stringify(original));
    f.listeners.frame(JSON.stringify(edit));
    await wait_for(function()
    {
        return f.delivered.length === 2;
    });
    assert.deepEqual(f.delivered.map(function(message)
    {
        return message.type;
    }), [original.message.type, edit.message.type]);
    assert.equal(f.delivered[0].value, original.message.value);
    assert.equal(f.delivered[1].value, edit.message.value);
});

test("failed originals keep dependent encrypted edits and deletions without blocking unrelated messages", async function(t)
{
    const f = await fixture(t);
    f.state.g_client_list = [{client_id: 3, public_key: "author-key", identity_hash: "YQ==", username: "Author"}];
    let failed_page;
    failed_page = await f.attach({accept: function(frame)
    {
        if (frame.message.server_chat_message_id === 40)
        {
            failed_page.socket.send(JSON.stringify({message: Object.assign({type: "local_delivery_ack", accepted: false}, frame.local_delivery)}));
            return false;
        }
        return true;
    }});
    const reaction_id = "a".repeat(32);
    const original = {type: "channel_chat_message", channel_id: 1, sender_id: 3,
        server_chat_message_id: 40, decrypted_value: JSON.stringify({value: "Original", reaction_id})};
    const edit = {type: "channel_chat_message", channel_id: 1, sender_id: 3,
        server_chat_message_id: 41, decrypted_value: JSON.stringify({type: "chat_message_edit",
            message_id: reaction_id, author_public_key: "author-key", new_message_value: "Edited"})};
    const deletion = {type: "chat_message_delete", chat_message_id: 40, requester_public_key: "author-key"};
    const unrelated = {type: "channel_chat_message", channel_id: 2, sender_id: 3,
        server_chat_message_id: 42, decrypted_value: JSON.stringify({value: "Unrelated", reaction_id: "b".repeat(32)})};
    for (const message of [original, edit, deletion, unrelated])
    {
        f.listeners.frame(JSON.stringify({message}));
    }
    await wait_for(function()
    {
        return failed_page.delivered.some(function(message)
        {
            return message.server_chat_message_id === 42;
        });
    });
    assert.deepEqual(failed_page.delivered.map(function(message)
    {
        return message.server_chat_message_id;
    }), [40, 42]);
    const replacement = await f.attach();
    await wait_for(function()
    {
        return replacement.received.some(function(frame)
        {
            return frame.message.type === "chat_message_delete";
        });
    });
    assert.deepEqual(replacement.received.filter(function(frame)
    {
        return frame.local_delivery;
    }).map(function(frame)
    {
        return frame.message;
    }), [original, edit, deletion]);
});

test("membership deltas cannot overtake a large message or an earlier unpublished reservation", async function(t)
{
    const f = await fixture(t);
    const original = {message: {type: "channel_chat_message", channel_id: 1, client_id: 3, value: "B".repeat(200000)}};
    const left = {message: {type: "client_disconnect", client_id: 3}};
    const first = f.listeners.frame(JSON.stringify(original), {defer_delivery: true});
    const second = f.listeners.frame(JSON.stringify(left), {defer_delivery: true});
    f.listeners.frame(null, {release_delivery: second});
    assert.equal(f.delivered.length, 0);
    f.listeners.frame(null, {release_delivery: first});
    await wait_for(function()
    {
        return f.delivered.length === 2;
    });
    assert.deepEqual(f.delivered.map(function(message)
    {
        return message.type;
    }), ["channel_chat_message", "client_disconnect"]);
});

async function acknowledgement_fence(page)
{
    const request_id = "fence-" + page.received.length;
    page.socket.send(JSON.stringify({message: {type: "local_command", action: "unknown", request_id}}));
    await wait_for(function()
    {
        return page.received.some(function(frame)
        {
            return frame.message.request_id === request_id;
        });
    });
}

test("accepted private-file metadata remains available until its body is accepted by a page", async function(t)
{
    const f = await fixture(t);
    const first = await f.attach(
    {
        accept: function(frame)
        {
            return frame.message.type !== "direct_chat_file_decrypted";
        }
    });
    const metadata = {message: {type: "direct_chat_file_metadata", file_id: 7, sender_id: 3,
        encrypted_size: 32, file_header_decrypted: {name: "example.txt", size: 10}}};
    f.listeners.frame(JSON.stringify(metadata));
    f.listeners.frame(JSON.stringify({message: {type: "file_receive_chunk", server_chat_message_id: 7, value: "x".repeat(32)}}));
    f.listeners.frame(JSON.stringify({message: {type: "file_receive_completed", server_chat_message_id: 7, receive_type: "direct_chat_file"}}));
    await wait_for(function()
    {
        return first.delivered.some(function(message)
        {
            return message.type === "file_receive_completed";
        });
    });
    await acknowledgement_fence(first);
    const body = {message: {type: "direct_chat_file_decrypted", file_id: 7, client_id: 3, file: {base64: "A".repeat(200000)}}};
    f.listeners.frame(JSON.stringify(body));
    await wait_for(function()
    {
        return first.delivered.some(function(message)
        {
            return message.type === "direct_chat_file_decrypted";
        });
    });
    const rejected = first.received.find(function(frame)
    {
        return frame.message.type === "direct_chat_file_decrypted";
    }).local_delivery;
    first.socket.send(JSON.stringify({message: Object.assign({type: "local_delivery_ack", accepted: false}, rejected)}));
    await acknowledgement_fence(first);
    const replacement = await f.attach();
    await wait_for(function()
    {
        return replacement.delivered.some(function(message)
        {
            return message.type === "direct_chat_file_decrypted";
        });
    });
    const content = replacement.received.map(function(frame)
    {
        return frame.message;
    }).filter(function(message)
    {
        return message.type === "direct_chat_file_metadata" || message.type === "direct_chat_file_decrypted"
            || message.type === "file_receive_chunk" || message.type === "file_receive_completed";
    });
    assert.deepEqual(content.map(function(message)
    {
        return message.type;
    }), ["direct_chat_file_metadata", "direct_chat_file_decrypted"]);
    assert.equal(content[0].file_header_decrypted.name, "example.txt");
    assert.equal(content[1].file.base64.length, 200000);
    await acknowledgement_fence(replacement);
    const final_page = await f.attach();
    assert.equal(final_page.received.some(function(frame)
    {
        return frame.local_delivery;
    }), false);
});

test("explicit attachment discard drops its later body without retaining an orphan", async function(t)
{
    const f = await fixture(t);
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_file_metadata", file_id: 12, sender_id: 3, encrypted_size: 32}}));
    await wait_for(function()
    {
        return f.received.some(function(frame)
        {
            return frame.message.type === "direct_chat_file_metadata";
        });
    });
    const metadata = f.received.find(function(frame)
    {
        return frame.message.type === "direct_chat_file_metadata";
    }).local_delivery;
    f.socket.send(JSON.stringify({message: Object.assign({type: "local_delivery_ack", discard_attachment: true}, metadata)}));
    await acknowledgement_fence(f);
    f.listeners.frame(JSON.stringify({message: {type: "file_receive_chunk", server_chat_message_id: 12, value: "X".repeat(32)}}));
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_file_decrypted", file_id: 12, file: {base64: "Y".repeat(80000)}}}));
    const replacement = await f.attach();
    assert.equal(replacement.received.some(function(frame)
    {
        return frame.local_delivery || frame.message.type === "local_transfer_begin";
    }), false);
});

test("prepared channel attachments replay with metadata after their encrypted prefix is retired", async function(t)
{
    const f = await fixture(t);
    const first = await f.attach({accept: function(frame)
    {
        return !frame.message.type.endsWith("_decrypted");
    }});
    const expected = [];
    for (const [kind, id] of [["file", 15], ["picture", 16]])
    {
        const metadata = {type: "channel_chat_" + kind + "_metadata", channel_id: 1,
            sender_id: 3, encrypted_size: 32, [kind + "_id"]: id};
        const prepared = {type: "channel_chat_" + kind + "_decrypted", client_id: 3, [kind + "_id"]: id};
        if (kind === "file")
        {
            metadata.file_header_decrypted = {name: "before-key-rotation.txt", size: 60000};
            prepared.file = {name: metadata.file_header_decrypted.name, size: 60000, base64: "A".repeat(80000)};
        }
        else
        {
            prepared.decrypted_base64_picture = "data:image/png;base64," + "B".repeat(80000);
        }
        for (const message of [metadata, {type: "file_receive_chunk", server_chat_message_id: id, value: "X".repeat(32)}, prepared])
        {
            f.listeners.frame(JSON.stringify({message}));
        }
        expected.push(metadata, prepared);
    }
    await wait_for(function()
    {
        return first.received.filter(function(frame)
        {
            return frame.message.type.endsWith("_decrypted");
        }).length === 2;
    });
    const replacement = await f.attach();
    await wait_for(function()
    {
        return replacement.received.filter(function(frame)
        {
            return frame.message.type.endsWith("_decrypted");
        }).length === 2;
    });
    assert.deepEqual(replacement.received.filter(function(frame)
    {
        return frame.local_delivery;
    }).map(function(frame)
    {
        return frame.message;
    }), expected);
    await acknowledgement_fence(replacement);
    const final_page = await f.attach();
    assert.equal(final_page.received.some(function(frame)
    {
        return frame.local_delivery;
    }), false);
});

test("an accepted attachment deletion remains replayable while its original body is pending", async function(t)
{
    const f = await fixture(t);
    const metadata = {type: "direct_chat_picture_metadata", picture_id: 17, sender_id: 3, encrypted_size: 32};
    const deletion = {type: "chat_message_delete", chat_message_id: 17, requester_public_key: "author"};
    f.listeners.frame(JSON.stringify({message: metadata}));
    f.listeners.frame(JSON.stringify({message: deletion}));
    await wait_for(function()
    {
        return f.received.some(function(frame)
        {
            return frame.message.type === "chat_message_delete";
        });
    });
    await acknowledgement_fence(f);
    const replacement = await f.attach();
    await wait_for(function()
    {
        return replacement.received.some(function(frame)
        {
            return frame.message.type === "chat_message_delete";
        });
    });
    assert.deepEqual(replacement.received.filter(function(frame)
    {
        return frame.local_delivery;
    }).map(function(frame)
    {
        return frame.message;
    }), [metadata, deletion]);
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_picture_decrypted", picture_id: 17,
        decrypted_base64_picture: "data:image/png;base64,AA=="}}));
    await wait_for(function()
    {
        return replacement.received.some(function(frame)
        {
            return frame.message.type === "direct_chat_picture_decrypted";
        });
    });
    await acknowledgement_fence(replacement);
    const final_page = await f.attach();
    assert.equal(final_page.received.some(function(frame)
    {
        return frame.local_delivery;
    }), false);
});

test("retained sender projection belongs to the original public identity after slot reuse", async function(t)
{
    const f = await fixture(t);
    f.state.g_client_list.push({client_id: 3, username: "Maya", public_key: "maya-public", identity_hash: "bWF5YQ=="});
    const first = await f.attach(
    {
        accept: function(frame)
        {
            return frame.message.type !== "direct_chat_file_decrypted";
        }
    });
    const token = f.listeners.frame(JSON.stringify({message: {type: "direct_chat_message", client_id: 3, value: "hello"},
        local_sender: {public_key: "forged", username: "forged"}}), {defer_delivery: true});
    f.state.g_client_list[0] = {client_id: 3, username: "Sam", public_key: "sam-public", identity_hash: "c2Ft"};
    f.listeners.frame(null, {release_delivery: token});
    await wait_for(function()
    {
        return first.received.some(function(frame)
        {
            return frame.message.type === "direct_chat_message";
        });
    });
    const sender = first.received.find(function(frame)
    {
        return frame.message.type === "direct_chat_message";
    }).local_sender;
    assert.deepEqual(sender, {client_id: 3, username: "Maya", public_key: "maya-public", identity_hash: "bWF5YQ=="});
    f.state.g_client_list[0] = sender;
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_file_metadata", file_id: 13, sender_id: 3, encrypted_size: 32}}));
    await wait_for(function()
    {
        return first.received.some(function(frame)
        {
            return frame.message.type === "direct_chat_file_metadata";
        });
    });
    await acknowledgement_fence(first);
    f.state.g_client_list[0] = {client_id: 3, username: "Sam", public_key: "sam-public", identity_hash: "c2Ft"};
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_file_decrypted", file_id: 13, client_id: 3,
        file: {base64: "Z".repeat(80000)}}}));
    await wait_for(function()
    {
        return first.received.some(function(frame)
        {
            return frame.message.type === "direct_chat_file_decrypted";
        });
    });
    const replacement = await f.attach();
    await wait_for(function()
    {
        return replacement.received.some(function(frame)
        {
            return frame.message.type === "direct_chat_file_decrypted";
        });
    });
    for (const frame of replacement.received.filter(function(frame)
    {
        return frame.message.type === "direct_chat_file_metadata" || frame.message.type === "direct_chat_file_decrypted";
    }))
    {
        assert.deepEqual(frame.local_sender, sender);
    }
});

test("incomplete attachment prefixes expire but completed unaccepted bodies keep their replay context", async function(t)
{
    let now = Date.now();
    let watchdog;
    const module_path = path.resolve(__dirname, "../loopback-ui-server.js");
    const output = {exports: {}};
    vm.runInNewContext(fs.readFileSync(module_path, "utf8"),
    {
        module: output,
        require: createRequire(module_path),
        Buffer, console, setImmediate,
        Date:
        {
            now: function()
            {
                return now;
            }
        },
        setInterval: function(callback)
        {
            watchdog = callback;
            return {
                unref: function()
                {
                }
            };
        },
        clearInterval: function()
        {
        }
    });
    const f = await fixture(t, output.exports);
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_file_metadata", file_id: 14, sender_id: 3, encrypted_size: 32}}));
    const first = await f.attach(
    {
        accept: function(frame)
        {
            return frame.message.type !== "direct_chat_file_decrypted";
        }
    });
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_file_metadata", file_id: 15, sender_id: 3, encrypted_size: 32}}));
    f.listeners.frame(JSON.stringify({message: {type: "direct_chat_file_decrypted", file_id: 15, file: {base64: "W".repeat(80000)}}}));
    await wait_for(function()
    {
        return first.received.some(function(frame)
        {
            return frame.message.type === "direct_chat_file_decrypted";
        });
    });
    now += 300001;
    watchdog();
    await wait_for(function()
    {
        return first.received.some(function(frame)
        {
            return frame.message.type === "chat_file_decrypt_failed" && frame.message.file_id === 14;
        });
    });
    const replacement = await f.attach();
    assert.equal(replacement.received.some(function(frame)
    {
        return frame.message.type === "direct_chat_file_metadata" && frame.message.file_id === 14;
    }), false);
    assert.equal(replacement.received.some(function(frame)
    {
        return frame.message.type === "direct_chat_file_metadata" && frame.message.file_id === 15;
    }), true);
});

test("an acknowledged channel attachment prefix is replayed before completion and released with terminal acceptance", async function(t)
{
    const f = await fixture(t);
    f.listeners.frame(JSON.stringify({message: {type: "channel_chat_file_metadata", file_id: 8, sender_id: 3, encrypted_size: 32}}));
    f.listeners.frame(JSON.stringify({message: {type: "file_receive_chunk", server_chat_message_id: 8, value: "x".repeat(32)}}));
    await wait_for(function()
    {
        return f.delivered.length === 2;
    });
    await acknowledgement_fence(f);
    const replacement = await f.attach();
    await wait_for(function()
    {
        return replacement.received.filter(function(frame)
        {
            return frame.local_delivery;
        }).length === 2;
    });
    assert.deepEqual(replacement.received.filter(function(frame)
    {
        return frame.local_delivery;
    }).map(function(frame)
    {
        return frame.message.type;
    }), ["channel_chat_file_metadata", "file_receive_chunk"]);
    f.listeners.frame(JSON.stringify({message: {type: "file_receive_completed", server_chat_message_id: 8, receive_type: "channel_chat_file"}}));
    await wait_for(function()
    {
        return replacement.delivered.some(function(message)
        {
            return message.type === "file_receive_completed";
        });
    });
    await acknowledgement_fence(replacement);
    const final_page = await f.attach();
    assert.equal(final_page.received.some(function(frame)
    {
        return frame.local_delivery;
    }), false);
});
