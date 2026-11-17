const test = require("node:test");
const assert = require("node:assert/strict");
const transport = require("../loopback-transfer.js");

function fixture()
{
    const sent = [];
    const completed = [];
    const failed = [];
    const timers = new Map();
    let next_timer = 0;
    const sender = transport.create({
        session_id: "session-1",
        send: function(message)
        {
            sent.push(message);
            return true;
        },
        on_complete: function(metadata)
        {
            completed.push(metadata);
        },
        on_failure: function(metadata, reason)
        {
            failed.push({ metadata, reason });
        },
        set_timeout: function(callback, delay)
        {
            assert.equal(delay, 120000);
            const id = ++next_timer;
            timers.set(id, callback);
            return id;
        },
        clear_timeout: function(id)
        {
            timers.delete(id);
        }
    });
    return { sender, sent, completed, failed, timers };
}

function ack(sender, begin, index, extra)
{
    return sender.acknowledge(Object.assign({ transfer_id: begin.transfer_id, session_id: begin.session_id, index, accepted: true }, extra));
}

test("four-chunk window preserves exact UTF-8 bytes and distinguishes chunk completion from delivery", function()
{
    const f = fixture();
    const text = JSON.stringify({ message: { type: "direct_chat_file_decrypted", file_id: "file-1",
        file: { base64: ("x".repeat(16383) + "🍋ž日本語").repeat(30) } }, local_delivery: { id: "delivery-1", session_id: "session-1" } });
    const metadata = { delivery_id: "delivery-1", content_type: "direct_chat_file_decrypted", file_id: "file-1" };
    assert.equal(f.sender.send_frame(text, metadata), "started");
    const begin = f.sent[0];
    assert.equal(begin.type, "local_transfer_begin");
    assert.equal(begin.total_bytes, Buffer.byteLength(text));
    assert.equal(begin.file_id, "file-1");
    assert.equal(f.sent.length, 1, "no chunks before admission acknowledgement");
    assert.equal(ack(f.sender, begin, -1, { session_id: "wrong" }), false);
    assert.equal(f.sent.length, 1);
    assert.equal(ack(f.sender, begin, -1), true);
    assert.equal(f.sent.length, 5, "at most four chunks are initially in flight");
    const chunks = [];
    for (let index = 0; index < begin.total_chunks; index++)
    {
        const frame = f.sent.find(function(message)
        {
            return message.type === "local_transfer_chunk" && message.index === index;
        });
        assert.ok(frame);
        assert.ok(frame.data.length <= 64 * 1024);
        chunks.push(Buffer.from(frame.data, "base64"));
        assert.equal(ack(f.sender, begin, index), true);
        assert.equal(ack(f.sender, begin, index), index + 1 !== begin.total_chunks, "duplicates never complete twice");
        const highest = Math.max(...f.sent.filter(function(message)
        {
            return message.type === "local_transfer_chunk";
        }).map(function(message)
        {
            return message.index;
        }));
        assert.ok(highest - index <= 4);
    }
    assert.equal(Buffer.concat(chunks).toString("utf8"), text);
    assert.deepEqual(f.completed, [metadata]);
    assert.deepEqual(f.failed, []);
    assert.equal(f.timers.size, 0);
    assert.equal(f.sent.some(function(message)
    {
        return message.type === "local_delivery_ack";
    }), false, "transport completion never invents the page's content acknowledgement");
});

test("out-of-order and unsent acknowledgements cancel only their transfer", function()
{
    const f = fixture();
    f.sender.send_frame("x".repeat(10 * transport.RAW_CHUNK_BYTES), { delivery_id: "first" });
    const first = f.sent[0];
    f.sender.send_frame("y", { delivery_id: "second" });
    const second = f.sent[1];
    ack(f.sender, first, -1);
    assert.equal(ack(f.sender, first, 2), false);
    assert.equal(f.failed.length, 1);
    assert.equal(f.failed[0].reason, "invalid_acknowledgement");
    assert.equal(f.failed[0].metadata.delivery_id, "first");
    ack(f.sender, second, -1);
    ack(f.sender, second, 0);
    assert.equal(f.completed.length, 1);
    assert.equal(f.completed[0].delivery_id, "second");
});

test("admission count, aggregate bytes, maximum frame and expiry are bounded", function()
{
    const count = fixture();
    for (let index = 0; index < 8; index++)
    {
        assert.equal(count.sender.send_frame("x", { delivery_id: String(index) }), "started");
    }
    assert.equal(count.sender.send_frame("x", { delivery_id: "ninth" }), "busy");
    const expire = count.timers.values().next().value;
    expire();
    expire();
    assert.equal(count.failed.length, 1);
    assert.equal(count.failed[0].reason, "expired");
    assert.equal(count.sender.send_frame("x", { delivery_id: "replacement" }), "started");
    count.sender.close();
    assert.equal(count.timers.size, 0);
    const aggregate = fixture();
    const large = "x".repeat(40 * 1024 * 1024);
    assert.equal(aggregate.sender.send_frame(large, { delivery_id: "large-one" }), "started");
    assert.equal(aggregate.sender.send_frame(large, { delivery_id: "large-two" }), "busy");
    assert.equal(aggregate.sender.send_frame("x".repeat(transport.MAX_FRAME_BYTES + 1), { delivery_id: "oversize" }), "rejected");
    assert.equal(aggregate.failed[0].reason, "frame_too_large");
    aggregate.sender.close();
    assert.equal(aggregate.timers.size, 0);
});

test("page refusal cancels admission and permits another transfer", function()
{
    const f = fixture();
    f.sender.send_frame("original", { delivery_id: "delivery-1" });
    const begin = f.sent[0];
    assert.equal(ack(f.sender, begin, -1, { accepted: false }), true);
    assert.equal(f.sent[1].type, "local_transfer_cancel");
    assert.equal(f.failed[0].reason, "page_rejected");
    assert.equal(f.timers.size, 0);
    assert.deepEqual(f.completed, []);
});
