// this file sends large content, such as files and pictures, from Node.js to the page in the app window.
// the content is sent in parts of 48 KB. the page confirms each part, accepts the whole transfer or cancels it.
// at most 4 parts are sent before the page confirms them, at most 8 transfers run at the same time, and a transfer
// that gets no answer for 2 minutes is stopped
const crypto = require("crypto");
const RAW_CHUNK_BYTES = 48 * 1024;
const MAX_FRAME_BYTES = 48 * 1024 * 1024 + 64 * 1024;
const MAX_AGGREGATE_BYTES = 64 * 1024 * 1024 + 8 * 64 * 1024;
const MAX_TRANSFERS = 8;
const WINDOW = 4;
const TIMEOUT_MS = 120000;

/**
 * @brief encodes the next UTF8 transfer chunk without splitting surrogate pairs
 *
 * @param transfer current encoding state
 *
 * @return Base64 encoded chunk
 */
function next_chunk(transfer)
{
    const size = Math.min(RAW_CHUNK_BYTES, transfer.total_bytes - transfer.encoded_bytes);
    const output = Buffer.allocUnsafe(size);
    let written = 0;
    while (written < size)
    {
        if (transfer.batch_offset === transfer.batch.length)
        {
            let end = Math.min(transfer.text.length, transfer.text_offset + 16 * 1024);
            const last = transfer.text.charCodeAt(end - 1);
            const next = transfer.text.charCodeAt(end);
            if (last >= 0xd800 && last <= 0xdbff && next >= 0xdc00 && next <= 0xdfff)
            {
                end--;
            }
            transfer.batch = Buffer.from(transfer.text.slice(transfer.text_offset, end), "utf8");
            transfer.text_offset = end;
            transfer.batch_offset = 0;
        }
        const copied = Math.min(size - written, transfer.batch.length - transfer.batch_offset);
        if (copied === 0)
        {
            throw new Error("Local transfer encoding did not advance");
        }
        transfer.batch.copy(output, written, transfer.batch_offset, transfer.batch_offset + copied);
        transfer.batch_offset += copied;
        written += copied;
    }
    transfer.encoded_bytes += written;
    return output.toString("base64");
}

/**
 * @brief creates the local content transfer manager
 *
 * @param options session ID, transport callbacks and optional timer adapters
 *
 * @return transfer manager with send_frame, acknowledge, cancel_delivery and close methods
 */
function create(options)
{
    const transfers = new Map();
    const schedule = options.set_timeout || setTimeout;
    const unschedule = options.clear_timeout || clearTimeout;
    let total_bytes = 0;
    let closed = false;

    /**
     * @brief sends a transfer message through the transport callback
     *
     * @param object message -> local transfer message
     *
     * @return boolean -> true only when the callback accepts the message
     */
    function send(message)
    {
        try
        {
            return options.send(message) === true;
        }
        catch (error)
        {
            return false;
        }
    }

    /**
     * @brief releases a registered outgoing transfer and its timer
     *
     * @param object transfer -> transfer entry
     *
     * @return boolean -> true when the entry was still registered
     */
    function remove(transfer)
    {
        if (!transfers.delete(transfer.id))
        {
            return false;
        }
        total_bytes -= transfer.total_bytes;
        unschedule(transfer.timer);
        transfer.text = "";
        transfer.batch = Buffer.alloc(0);
        return true;
    }

    /**
     * @brief cancels an outgoing transfer and reports its failure once
     *
     * @param object transfer -> transfer entry
     * @param string reason -> failure reason
     *
     * @return void
     */
    function fail(transfer, reason)
    {
        if (!remove(transfer))
        {
            return;
        }
        send({ type: "local_transfer_cancel", transfer_id: transfer.id, session_id: options.session_id });
        options.on_failure(transfer.metadata, reason);
    }

    /**
     * @brief sends more chunks while the acknowledgement window has capacity
     *
     * @param object transfer -> active outgoing transfer
     *
     * @return void
     */
    function pump(transfer)
    {
        while (!closed && transfers.has(transfer.id) && transfer.started && transfer.next_index < transfer.total_chunks)
        {
            const outstanding_chunks = transfer.next_index - transfer.last_acknowledged - 1;
            if (outstanding_chunks >= WINDOW)
            {
                return;
            }
            const index = transfer.next_index++;
            let data;
            try
            {
                data = next_chunk(transfer);
            }
            catch (error)
            {
                fail(transfer, "encoding_failed");
                return;
            }
            const message = {
                type: "local_transfer_chunk",
                transfer_id: transfer.id,
                session_id: options.session_id,
                index,
                data
            };
            if (!send(message))
            {
                fail(transfer, "transport_closed");
                return;
            }
        }
    }

    return {
        /**
         * @brief reserves outgoing capacity and announces a local transfer
         *
         * @param string text -> complete JSON frame
         * @param object metadata -> delivery and attachment identifiers
         *
         * @return string -> started, busy or rejected
         */
        send_frame: function(text, metadata)
        {
            const bytes = Buffer.byteLength(text, "utf8");
            if (closed || bytes === 0 || bytes > MAX_FRAME_BYTES)
            {
                options.on_failure(metadata, closed ? "transport_closed" : "frame_too_large");
                return "rejected";
            }
            if (transfers.size >= MAX_TRANSFERS || total_bytes + bytes > MAX_AGGREGATE_BYTES)
            {
                return "busy";
            }
            const transfer = {
                id: crypto.randomBytes(16).toString("hex"),
                text,
                metadata,
                total_bytes: bytes,
                total_chunks: Math.ceil(bytes / RAW_CHUNK_BYTES),
                started: false,
                next_index: 0,
                last_acknowledged: -1,
                text_offset: 0,
                encoded_bytes: 0,
                batch: Buffer.alloc(0),
                batch_offset: 0,
                timer: null
            };
            transfers.set(transfer.id, transfer);
            total_bytes += bytes;
            transfer.timer = schedule(function()
            {
                fail(transfer, "expired");
            }, TIMEOUT_MS);
            const begin = {
                type: "local_transfer_begin",
                transfer_id: transfer.id,
                session_id: options.session_id,
                total_bytes: bytes,
                total_chunks: transfer.total_chunks
            };
            if (metadata.content_type === "direct_chat_file_decrypted" || metadata.content_type === "direct_chat_picture_decrypted")
            {
                begin.content_type = metadata.content_type;
                if (metadata.file_id !== undefined)
                {
                    begin.file_id = metadata.file_id;
                }
                if (metadata.picture_id !== undefined)
                {
                    begin.picture_id = metadata.picture_id;
                }
            }
            if (!send(begin))
            {
                fail(transfer, "transport_closed");
                return "rejected";
            }
            return "started";
        },
        /**
         * @brief validates a byte acknowledgement and advances its transfer
         *
         * @param object message -> page acknowledgement with transfer, session and index
         *
         * @return boolean -> true when the acknowledgement was handled
         */
        acknowledge: function(message)
        {
            if (closed || message.session_id !== options.session_id || typeof message.transfer_id !== "string")
            {
                return false;
            }
            const transfer = transfers.get(message.transfer_id);
            if (!transfer)
            {
                return false;
            }
            if (!Number.isInteger(message.index) || message.index < -1 || message.index >= transfer.total_chunks || typeof message.accepted !== "boolean")
            {
                fail(transfer, "invalid_acknowledgement");
                return false;
            }
            if (!message.accepted)
            {
                fail(transfer, "page_rejected");
                return true;
            }
            if (!transfer.started)
            {
                if (message.index !== -1)
                {
                    fail(transfer, "invalid_acknowledgement");
                    return false;
                }
                transfer.started = true;
                pump(transfer);
                return true;
            }
            if (message.index <= transfer.last_acknowledged)
            {
                return true;
            }
            if (message.index !== transfer.last_acknowledged + 1 || message.index >= transfer.next_index)
            {
                fail(transfer, "invalid_acknowledgement");
                return false;
            }
            transfer.last_acknowledged = message.index;
            if (message.index + 1 === transfer.total_chunks)
            {
                remove(transfer);
                options.on_complete(transfer.metadata);
                return true;
            }
            pump(transfer);
            return true;
        },
        /**
         * @brief fails the active transfer associated with a discarded delivery
         *
         * @param string delivery_id -> delivery identifier
         *
         * @return boolean -> true when a matching transfer was canceled
         */
        cancel_delivery: function(delivery_id)
        {
            for (const transfer of transfers.values())
            {
                if (transfer.metadata.delivery_id === delivery_id)
                {
                    fail(transfer, "page_discarded");
                    return true;
                }
            }
            return false;
        },
        /**
         * @brief stops the sender and releases all active transfers
         *
         * @return void
         */
        close: function()
        {
            closed = true;
            for (const transfer of transfers.values())
            {
                remove(transfer);
            }
        }
    };
}

module.exports = { create, RAW_CHUNK_BYTES, MAX_FRAME_BYTES, MAX_AGGREGATE_BYTES, MAX_TRANSFERS, WINDOW, TIMEOUT_MS };
