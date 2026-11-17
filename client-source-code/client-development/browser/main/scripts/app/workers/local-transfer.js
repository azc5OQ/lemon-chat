// reassembles bounded local chunks into the original JSON bytes. encryption stays with the sender
/**
 * @brief reassembles local transfers within the current page session
 *
 * @param object options -> scope reader and acknowledgement, frame and failure callbacks
 *
 * @return object receiver with receive, cancelAll and readState methods
 */
function local_transfer__create_receiver(options)
{
    const chunk_bytes = 48 * 1024;
    const maximum_bytes = 48 * 1024 * 1024 + 64 * 1024;
    const aggregate_bytes = 64 * 1024 * 1024 + 8 * 64 * 1024;
    const transfers = new Map();
    let reserved_bytes = 0;

    /**
     * @brief checks a local transfer identifier against its length and character limits
     *
     * @param any value -> identifier to validate
     *
     * @return boolean -> true for a supported identifier
     */
    function valid_id(value)
    {
        return typeof value === "string" && /^[A-Za-z0-9._:-]{1,128}$/.test(value);
    }

    /**
     * @brief checks that a transfer still belongs to the current page session
     *
     * @param object entry -> transfer entry with session and generation
     *
     * @return boolean -> true when both scope values match
     */
    function current(entry)
    {
        const scope = options.getScope();
        return scope.session_id === entry.session_id && scope.generation === entry.generation;
    }

    /**
     * @brief sends a transfer acknowledgement without propagating callback errors
     *
     * @param object message -> transfer identifiers
     * @param number index -> chunk index, or minus one for admission
     * @param boolean accepted -> whether the step was accepted
     * @param string|undefined reason -> failure reason when rejected
     *
     * @return boolean -> true when the acknowledgement callback completes
     */
    function reply(message, index, accepted, reason)
    {
        try
        {
            options.onAck(
            {
                type: "local_transfer_ack",
                transfer_id: message.transfer_id,
                session_id: message.session_id,
                index: index,
                accepted: accepted,
                reason: reason || ""
            });
            return true;
        }
        catch (error)
        {
            return false;
        }
    }

    /**
     * @brief releases a registered transfer's buffer, timer and capacity
     *
     * @param object entry -> transfer entry to release
     *
     * @return void
     */
    function release(entry)
    {
        if (transfers.get(entry.transfer_id) !== entry)
        {
            return;
        }
        transfers.delete(entry.transfer_id);
        reserved_bytes -= entry.total_bytes;
        options.clearTimer(entry.timer);
        entry.buffer = null;
    }

    /**
     * @brief reports a transfer failure only while its session remains current
     *
     * @param object entry -> failed transfer entry
     * @param string reason -> failure reason
     *
     * @return void
     */
    function notify_failure(entry, reason)
    {
        if (current(entry))
        {
            try
            {
                options.onFailure(entry.metadata, reason);
            }
            catch (error)
            {
                // cleanup and the negative acknowledgement must not depend on presentation code
            }
        }
    }

    /**
     * @brief releases a failed transfer before notifying its current owner
     *
     * @param object entry -> registered transfer entry
     * @param number index -> failed chunk index
     * @param string reason -> failure reason
     *
     * @return void
     */
    function fail(entry, index, reason)
    {
        if (transfers.get(entry.transfer_id) !== entry)
        {
            return;
        }
        release(entry);
        if (current(entry))
        {
            reply(entry, index, false, reason);
            notify_failure(entry, reason);
        }
    }

    /**
     * @brief reads the attachment identifier needed for a terminal transfer result
     *
     * @param object message -> transfer admission message
     *
     * @return object|null -> attachment metadata, or null for other content types
     */
    function terminal_metadata(message)
    {
        if (message.content_type !== "direct_chat_file_decrypted" && message.content_type !== "direct_chat_picture_decrypted" && message.content_type !== "channel_chat_file_decrypted" && message.content_type !== "channel_chat_picture_decrypted")
        {
            return null;
        }
        const key = message.content_type.endsWith("_file_decrypted") ? "file_id" : "picture_id";
        const id = message[key];
        if (!((typeof id === "string" && id.length > 0 && id.length <= 128) || (Number.isSafeInteger(id) && id >= 0)))
        {
            throw new Error("invalid terminal identifier");
        }
        return {content_type: message.content_type, key: key, id: id};
    }

    /**
     * @brief validates and reserves bounded storage for an incoming local transfer
     *
     * @param object message -> transfer admission message
     *
     * @return void
     */
    function begin(message)
    {
        const scope = options.getScope();
        if (message.session_id !== scope.session_id || !scope.session_id)
        {
            reply(message, -1, false, "stale_session");
            return;
        }
        let metadata;
        try
        {
            metadata = terminal_metadata(message);
        }
        catch (error)
        {
            reply(message, -1, false, "invalid_metadata");
            return;
        }
        let existing = transfers.get(message.transfer_id);
        if (existing != null && !current(existing))
        {
            release(existing);
            existing = null;
        }
        if (existing != null)
        {
            if (current(existing) && existing.total_bytes === message.total_bytes && existing.total_chunks === message.total_chunks && JSON.stringify(existing.metadata) === JSON.stringify(metadata))
            {
                reply(existing, -1, true);
            }
            else
            {
                fail(existing, -1, "conflicting_begin");
            }
            return;
        }
        if (!Number.isSafeInteger(message.total_bytes) || message.total_bytes < 1 || message.total_bytes > maximum_bytes || !Number.isSafeInteger(message.total_chunks) || message.total_chunks !== Math.ceil(message.total_bytes / chunk_bytes))
        {
            reply(message, -1, false, "invalid_size");
            return;
        }
        if (transfers.size >= 8 || reserved_bytes + message.total_bytes > aggregate_bytes)
        {
            reply(message, -1, false, "receiver_busy");
            return;
        }
        const entry =
        {
            transfer_id: message.transfer_id,
            session_id: message.session_id,
            generation: scope.generation,
            total_bytes: message.total_bytes,
            total_chunks: message.total_chunks,
            metadata: metadata,
            next_index: 0,
            buffer: null,
            timer: null
        };
        try
        {
            entry.buffer = new Uint8Array(entry.total_bytes);
        }
        catch (error)
        {
            reply(message, -1, false, "allocation_failed");
            return;
        }
        transfers.set(entry.transfer_id, entry);
        reserved_bytes += entry.total_bytes;
        try
        {
            entry.timer = options.setTimer(function()
            {
                fail(entry, entry.next_index, "expired");
            }, 120000);
        }
        catch (error)
        {
            fail(entry, -1, "timer_failed");
            return;
        }
        if (!reply(entry, -1, true))
        {
            release(entry);
        }
    }

    /**
     * @brief validates an ordered chunk and dispatches the complete JSON frame
     *
     * @param object message -> encoded chunk and transfer identifiers
     *
     * @return void
     */
    function chunk(message)
    {
        const entry = transfers.get(message.transfer_id);
        if (entry == null)
        {
            reply(message, Number.isSafeInteger(message.index) ? message.index : -1, false, "unknown_transfer");
            return;
        }
        if (message.session_id !== entry.session_id)
        {
            reply(message, Number.isSafeInteger(message.index) ? message.index : -1, false, "stale_session");
            return;
        }
        if (!current(entry))
        {
            fail(entry, message.index, "stale_session");
            return;
        }
        if (!Number.isSafeInteger(message.index) || message.index < 0 || message.index >= entry.total_chunks || message.index > entry.next_index)
        {
            fail(entry, -1, "invalid_sequence");
            return;
        }
        const offset = message.index * chunk_bytes;
        const expected = Math.min(chunk_bytes, entry.total_bytes - offset);
        let binary;
        try
        {
            if (typeof message.data !== "string" || message.data.length !== 4 * Math.ceil(expected / 3) || !/^[A-Za-z0-9+/]*={0,2}$/.test(message.data))
            {
                throw new Error("invalid base64 chunk");
            }
            binary = atob(message.data);
            if (binary.length !== expected || btoa(binary) !== message.data)
            {
                throw new Error("invalid base64 length");
            }
        }
        catch (error)
        {
            fail(entry, message.index, "invalid_chunk");
            return;
        }
        if (message.index < entry.next_index)
        {
            for (let index = 0; index < binary.length; index++)
            {
                if (entry.buffer[offset + index] !== binary.charCodeAt(index))
                {
                    fail(entry, message.index, "conflicting_chunk");
                    return;
                }
            }
            reply(entry, message.index, true);
            return;
        }
        for (let index = 0; index < binary.length; index++)
        {
            entry.buffer[offset + index] = binary.charCodeAt(index);
        }
        entry.next_index++;
        if (entry.next_index < entry.total_chunks)
        {
            if (!reply(entry, message.index, true))
            {
                release(entry);
            }
            return;
        }
        let text;
        let frame;
        try
        {
            text = new TextDecoder("utf-8", {fatal: true}).decode(entry.buffer);
            frame = JSON.parse(text);
            if (frame == null || typeof frame !== "object" || Array.isArray(frame) || frame.message == null || typeof frame.message !== "object" || Array.isArray(frame.message) || typeof frame.message.type !== "string" || frame.message.type.startsWith("local_transfer_") || (frame.local_delivery != null && frame.local_delivery.session_id !== entry.session_id) || (entry.metadata != null && (frame.message.type !== entry.metadata.content_type || frame.message[entry.metadata.key] !== entry.metadata.id)))
            {
                throw new Error("invalid reconstructed frame");
            }
        }
        catch (error)
        {
            fail(entry, message.index, "invalid_frame");
            return;
        }
        release(entry);
        // acknowledge the bytes before dispatch can acknowledge the processed content
        if (!reply(entry, message.index, true))
        {
            notify_failure(entry, "acknowledgement_failed");
            return;
        }
        try
        {
            options.onFrame(text, frame);
        }
        catch (error)
        {
            notify_failure(entry, "processing_failed");
            return;
        }
    }

    return {
        /**
         * @brief routes a valid local transfer message to its receiver action
         *
         * @param object message -> begin, chunk or cancel message
         *
         * @return void
         */
        receive: function(message)
        {
            if (message == null || !valid_id(message.transfer_id) || !valid_id(message.session_id))
            {
                return;
            }
            if (message.type === "local_transfer_begin")
            {
                begin(message);
            }
            else if (message.type === "local_transfer_chunk")
            {
                chunk(message);
            }
            else if (message.type === "local_transfer_cancel")
            {
                const entry = transfers.get(message.transfer_id);
                if (entry != null && entry.session_id === message.session_id)
                {
                    fail(entry, entry.next_index, "canceled");
                }
            }
        },
        /**
         * @brief fails all registered transfers and releases their buffers
         *
         * @param string|undefined reason -> failure reason, defaulting to canceled
         *
         * @return void
         */
        cancelAll: function(reason)
        {
            for (const entry of Array.from(transfers.values()))
            {
                fail(entry, entry.next_index, reason || "canceled");
            }
        },
        /**
         * @brief reports current receiver capacity usage
         *
         * @return object -> active transfer count and reserved bytes
         */
        readState: function()
        {
            return {active_count: transfers.size, reserved_bytes: reserved_bytes};
        }
    };
}
