// retains session content until the authenticated page accepts it
// attaching a page does not connect to the server or mark conversations read
const crypto = require("crypto");
const adapter = require("./websocket-adapter.js");
const local_transfer = require("./loopback-transfer.js");
const limits = adapter.limits;
const retained_types = new Set([ "direct_chat_message", "offline_chat_message", "channel_chat_message", "direct_chat_picture_metadata", "channel_chat_picture_metadata", "direct_chat_picture_decrypted", "channel_chat_picture_decrypted", "direct_chat_file_metadata", "channel_chat_file_metadata", "direct_chat_file_decrypted", "channel_chat_file_decrypted", "file_receive_chunk", "file_receive_completed", "chat_file_decrypt_failed", "chat_picture_decrypt_failed", "chat_message_edit", "chat_message_delete", "file_send_error", "file_send_success", "image_sent_status", "server_chat_message_id_for_local_message_id", "local_transfer_result" ]);

/**
 * @brief starts the authenticated page listener and retains session delivery state
 *
 * @param bundle shared client exports
 * @param on_ready callback receiving the local port and token
 *
 * @return server handle with a close method
 */
function start(bundle, on_ready)
{
    const token = crypto.randomBytes(16).toString("hex");
    const delivery_prefix = crypto.randomBytes(16).toString("hex");
    const server = new adapter.WebSocketServer({ host: "127.0.0.1", port: 0 });
    let current = null;
    let page_connection_id = 0;
    let delivery_id = 0;
    let transport_epoch = 0;
    let cached_auth = null;
    let cached_media_keys = null;
    let cached_identity = null;
    const cached_channel_keys = new Map();
    let cached_channel_key_bytes = 0;
    const pending = new Map();
    const discarded_attachments = new Map();
    const latest_delivery_by_context = new Map();
    let pending_bytes = 0;

    /**
     * @brief identifies the attachment and delivery role of a content message
     *
     * @param object message -> message fields
     *
     * @return object|null -> attachment ID, role and kind, or null for unrelated content
     */
    function attachment_details(message)
    {
        let id;
        let role;
        let kind;
        if (message.type === "direct_chat_file_metadata" || message.type === "channel_chat_file_metadata")
        {
            id = message.file_id;
            role = "metadata";
            kind = "file";
        }
        else if (message.type === "direct_chat_picture_metadata" || message.type === "channel_chat_picture_metadata")
        {
            id = message.picture_id;
            role = "metadata";
            kind = "picture";
        }
        else if (message.type === "file_receive_chunk" || message.type === "file_receive_completed")
        {
            id = message.server_chat_message_id;
            role = message.type === "file_receive_chunk" ? "chunk" : message.receive_type === "direct_chat_file" || message.receive_type === "direct_chat_picture" ? "direct_complete" : "terminal";
        }
        else if (message.type === "direct_chat_file_decrypted" || message.type === "channel_chat_file_decrypted" || message.type === "chat_file_decrypt_failed")
        {
            id = message.file_id;
            role = "terminal";
            kind = "file";
        }
        else if (message.type === "direct_chat_picture_decrypted" || message.type === "channel_chat_picture_decrypted" || message.type === "chat_picture_decrypt_failed")
        {
            id = message.picture_id;
            role = "terminal";
            kind = "picture";
        }
        if (!role || !((Number.isSafeInteger(id) && id >= 0) || (typeof id === "string" && id.length > 0 && id.length <= 128)))
        {
            return null;
        }
        return {id: String(id), role, kind, prepared: message.type.endsWith("_decrypted")};
    }

    /**
     * @brief checks whether two deliveries belong to the same attachment scope
     *
     * @param object left -> first delivery
     * @param object right -> second delivery
     *
     * @return boolean|null|undefined -> truthy only for the same attachment, session and transport epoch
     */
    function same_attachment(left, right)
    {
        return left.attachment && right.attachment && left.attachment.id === right.attachment.id && left.session_id === right.session_id && left.epoch === right.epoch;
    }

    /**
     * @brief builds the scoped key used to track a discarded attachment
     *
     * @param object entry -> delivery with session, transport epoch and attachment
     *
     * @return string -> serialized attachment scope
     */
    function attachment_key(entry)
    {
        return JSON.stringify([entry.session_id, entry.epoch, entry.attachment.id]);
    }

    /**
     * @brief removes retained attachment parts up to its accepted terminal result
     *
     * @param object entry -> accepted terminal delivery
     *
     * @return void
     */
    function finish_attachment(entry)
    {
        for (const candidate of pending.values())
        {
            if (same_attachment(entry, candidate) && candidate.sequence <= entry.sequence)
            {
                remove_pending(candidate);
            }
        }
    }

    /**
     * @brief removes all attachment parts before canceling their active transfers
     *
     * @param object entry -> discarded delivery
     * @param object owner -> page connection that discarded it
     *
     * @return void
     */
    function discard_attachment(entry, owner)
    {
        discarded_attachments.set(attachment_key(entry), entry.attachment_deadline);
        const canceled = [];
        for (const candidate of pending.values())
        {
            if (same_attachment(entry, candidate))
            {
                canceled.push(candidate.id);
                remove_pending(candidate);
            }
        }
        if (owner.transfers)
        {
            for (const id of canceled)
            {
                owner.transfers.cancel_delivery(id);
            }
        }
    }

    /**
     * @brief captures sender identity and preserves it across attachment parts
     *
     * @param object frame -> content frame
     * @param object state -> shared client snapshot
     * @param object|null attachment -> attachment details when present
     * @param string|null session_id -> delivery session
     *
     * @return object|null -> retained sender details, or null when unavailable
     */
    function sender_projection(frame, state, attachment, session_id)
    {
        if (attachment)
        {
            for (const candidate of pending.values())
            {
                if (candidate.attachment && candidate.attachment.id === attachment.id && candidate.session_id === session_id && candidate.epoch === transport_epoch && candidate.sender)
                {
                    // keep the sender bound to the original attachment metadata

                    return Object.assign({}, candidate.sender);
                }
            }
        }
        const id = Number.isSafeInteger(frame.message.sender_id) ? frame.message.sender_id : frame.message.client_id;
        const client = Number.isSafeInteger(id) && id >= 0
            ? state.g_client_list.find(function(candidate)
            {
                return candidate.client_id === id;
            }) : null;
        if (client && typeof client.public_key === "string" && client.public_key.length > 0 && client.public_key.length <= 16384 && typeof client.username === "string" && client.username.length <= 512)
        {
            return {client_id: id, public_key: client.public_key,
                identity_hash: typeof client.identity_hash === "string" && client.identity_hash.length <= 128 && /^[A-Za-z0-9+/=]+$/.test(client.identity_hash) ? client.identity_hash : "",
                username: client.username};
        }
        return null;
    }

    /**
     * @brief finds the message identity or target needed to order dependent content
     *
     * @param object message -> message fields
     * @param object|null sender -> retained sender identity
     * @param object|null attachment -> attachment details
     *
     * @return object -> message or target identifiers when available
     */
    function delivery_subject(message, sender, attachment)
    {
        const subject = {};
        if (attachment && attachment.role === "metadata")
        {
            subject.server_id = attachment.id;
        }
        else if (message.type === "chat_message_delete" && Number.isSafeInteger(message.chat_message_id) && message.chat_message_id >= 0)
        {
            subject.target_server_id = String(message.chat_message_id);
        }
        else if (message.type === "channel_chat_message" || message.type === "direct_chat_message")
        {
            const channel = message.type === "channel_chat_message";
            let payload;
            try
            {
                payload = JSON.parse(channel ? message.decrypted_value : message.some_json && message.some_json.type === "direct_chat_message" ? message.some_json.value : "null");
            }
            catch (error)
            {
                payload = null;
            }
            if (payload && sender)
            {
                const context = channel ? "channel:" + message.channel_id : "private:" + sender.public_key;
                if ((payload.type === "chat_message_edit" || payload.type === "chat_reaction") && typeof payload.message_id === "string" && /^[a-f0-9]{32}$/.test(payload.message_id) && typeof payload.author_public_key === "string" && payload.author_public_key.length <= 16384)
                {
                    subject.target_content_key = JSON.stringify([context, payload.author_public_key, payload.message_id]);
                }
                else if (typeof payload.reaction_id === "string" && /^[a-f0-9]{32}$/.test(payload.reaction_id))
                {
                    subject.content_key = JSON.stringify([context, sender.public_key, payload.reaction_id]);
                }
            }
            if (!subject.target_content_key && Number.isSafeInteger(message.server_chat_message_id) && message.server_chat_message_id >= 0)
            {
                subject.server_id = String(message.server_chat_message_id);
            }
        }
        return subject;
    }

    /**
     * @brief finds the original retained message required by an edit or deletion
     *
     * @param object subject -> delivery subject identifiers
     * @param string|null session_id -> delivery session
     *
     * @return string|null -> prerequisite delivery ID, or null when none is retained
     */
    function dependency_for(subject, session_id)
    {
        if (!subject.target_server_id && !subject.target_content_key)
        {
            return null;
        }
        for (const candidate of pending.values())
        {
            if (candidate.session_id === session_id && candidate.epoch === transport_epoch && candidate.subject && ((subject.target_server_id && subject.target_server_id === candidate.subject.server_id) || (subject.target_content_key && subject.target_content_key === candidate.subject.content_key)))
            {
                return candidate.id;
            }
        }
        return null;
    }

    /**
     * @brief removes expired partial attachments and clears their visible receiving state
     *
     * @return void
     */
    function expire_incomplete_attachments()
    {
        const terminal_groups = new Set();
        const expired = new Map();
        const now = Date.now();
        for (const [key, deadline] of discarded_attachments)
        {
            if (deadline <= now)
            {
                discarded_attachments.delete(key);
            }
        }
        for (const entry of pending.values())
        {
            if (entry.attachment && entry.attachment.role === "terminal")
            {
                terminal_groups.add(attachment_key(entry));
            }
        }
        for (const entry of pending.values())
        {
            if (entry.attachment && entry.attachment.role !== "terminal" && entry.attachment_deadline <= now && !terminal_groups.has(attachment_key(entry)))
            {
                const group = attachment_key(entry);
                if (!expired.has(group) || entry.attachment.kind)
                {
                    expired.set(group, entry);
                }
                remove_pending(entry);
            }
        }
        for (const entry of expired.values())
        {
            if (current !== null && current.session_id === entry.session_id && entry.attachment.kind)
            {
                const picture = entry.attachment.kind === "picture";
                const message = {type: picture ? "chat_picture_decrypt_failed" : "chat_file_decrypt_failed",
                    reason: picture ? "could not receive this image" : "could not receive this file"};
                const numeric = Number(entry.attachment.id);
                message[picture ? "picture_id" : "file_id"] = Number.isSafeInteger(numeric) ? numeric : entry.attachment.id;
                send_message(current, message);
            }
        }
    }

    /**
     * @brief records the latest conversation delivery used to validate read requests
     *
     * @param object entry -> delivery entry to associate with a conversation
     * @param object frame -> content frame
     *
     * @return boolean -> false when conversation tracking exceeds capacity
     */
    function track_delivery(entry, frame)
    {
        const context_id = typeof bundle.node_delivery_context === "function" ? bundle.node_delivery_context(frame) : "";
        if (typeof context_id !== "string" || context_id.length === 0)
        {
            return true;
        }
        if (!latest_delivery_by_context.has(context_id) && latest_delivery_by_context.size >= limits.replayQueueCount)
        {
            bundle.node_fail_delivery("Conversation read state exceeds this device's capacity.");
            return false;
        }
        entry.context_id = context_id;
        const latest = latest_delivery_by_context.get(context_id);
        if (!latest || entry.sequence >= latest.sequence)
        {
            latest_delivery_by_context.set(context_id, { id: entry.id, sequence: entry.sequence,
                session_id: entry.session_id, acknowledged: false });
        }
        return true;
    }

    /**
     * @brief combines shared client and local session state
     *
     * @return object -> current state snapshot
     */
    function snapshot()
    {
        return Object.assign({}, bundle.read_state(), bundle.get_ui_session_snapshot());
    }

    /**
     * @brief projects unread, ignore and mute state for the local page
     *
     * @param object state -> current state snapshot
     *
     * @return object -> session, peer state and channel counts
     */
    function peer_state(state)
    {
        return {
            session_id: state.session_id,
            peer_states: state.peer_states || state.g_client_list.map(function(client)
            {
                return {
                    key: client.identity_hash ? "identity:" + client.identity_hash : "key:" + client.public_key,
                    unread_count: client.unread_count || 0,
                    is_ignored_by_local_client: client.is_ignored_by_local_client === true,
                    is_muted_by_local_client: client.is_muted_by_local_client === true
                };
            }),
            channel_counts: state.channel_counts || []
        };
    }

    /**
     * @brief sends peer state to the current authenticated page after its login burst
     *
     * @return void
     */
    function send_peer_state()
    {
        if (current === null || current.needs_burst)
        {
            return;
        }
        const state = snapshot();
        if (state.authenticated && state.session_id === current.session_id)
        {
            send_message(current, { type: "local_peer_state", value: peer_state(state) });
        }
    }

    /**
     * @brief checks whether a page connection is current and open
     *
     * @param object owner -> page connection owner
     *
     * @return boolean -> true for the current open socket
     */
    function is_current(owner)
    {
        return current === owner && owner.socket.readyState === 1;
    }

    /**
     * @brief sends text only to the current page connection
     *
     * @param object owner -> page connection owner
     * @param string text -> serialized frame
     *
     * @return boolean -> true when the socket accepts the send
     */
    function send(owner, text)
    {
        if (!is_current(owner))
        {
            return false;
        }
        try
        {
            owner.socket.send(text);
            return true;
        }
        catch (error)
        {
            owner.socket.terminate();
            return false;
        }
    }

    /**
     * @brief wraps a local message and sends it to the current page
     *
     * @param object owner -> page connection owner
     * @param object message -> message fields
     *
     * @return boolean -> true when the socket accepts the send
     */
    function send_message(owner, message)
    {
        return send(owner, JSON.stringify({ message }));
    }

    /**
     * @brief forwards connection status to the current page
     *
     * @param object status -> remote connection phase and reason
     *
     * @return void
     */
    function status_to_ui(status)
    {
        if (current !== null)
        {
            send_message(current, { type: "loopback_status", value: status });
        }
    }

    /**
     * @brief revokes a page owner before canceling its transfers and upload
     *
     * @param object|null owner -> page connection to detach
     *
     * @return void
     */
    function revoke(owner)
    {
        if (current === owner)
        {
            current = null;
        }
        if (owner !== null)
        {
            owner.revoked = true;
            if (owner.transfers)
            {
                owner.transfers.close();
            }
            owner.requests = [];
            owner.request_bytes = 0;
            if (owner.pending_upload && typeof bundle.node_cancel_upload_request === "function")
            {
                bundle.node_cancel_upload_request();
            }
            owner.socket.close(1000, "Page detached");
            for (const entry of pending.values())
            {
                if (!entry.retained && entry.released)
                {
                    remove_pending(entry);
                }
            }
        }
    }

    /**
     * @brief releases a retained delivery and accepted dependents that no longer need it
     *
     * @param object entry -> delivery entry to remove
     *
     * @return void
     */
    function remove_pending(entry)
    {
        if (pending.delete(entry.id))
        {
            pending_bytes -= entry.bytes;
            for (const candidate of pending.values())
            {
                if (candidate.dependency_id === entry.id && current !== null && candidate.model_accepted_page_id === current.id)
                {
                    remove_pending(candidate);
                }
            }
        }
    }

    /**
     * @brief sends ready content in observation order while respecting dependencies
     *
     * @param object owner -> current page connection
     *
     * @return void
     */
    function send_pending(owner)
    {
        if (!is_current(owner) || owner.active_delivery != null)
        {
            return;
        }
        for (const entry of pending.values())
        {
            if (!entry.released)
            {
                // preserve observation order while earlier reservations are pending

                return;
            }
            if (entry.session_id === null && entry.epoch === transport_epoch)
            {
                entry.session_id = owner.session_id;
                entry.frame.local_delivery.session_id = owner.session_id;
                const text = JSON.stringify(entry.frame);
                pending_bytes += Buffer.byteLength(text, "utf8") - entry.bytes;
                entry.text = text;
                entry.bytes = Buffer.byteLength(text, "utf8");
                if (!track_delivery(entry, entry.frame))
                {
                    return;
                }
                entry.frame = null;
            }
            if (entry.session_id === owner.session_id && entry.sent_page_id !== owner.id && entry.failed_page_id !== owner.id)
            {
                const dependency = pending.get(entry.dependency_id);
                if (dependency && dependency.model_accepted_page_id !== owner.id)
                {
                    // wait for an original message before applying its edits or deletion

                    continue;
                }
                if (!send_entry(owner, entry))
                {
                    return;
                }
                if (owner.active_delivery != null)
                {
                    return;
                }
            }
        }
    }

    /**
     * @brief sends one delivery directly or starts its bounded chunk transfer
     *
     * @param object owner -> page connection
     * @param object entry -> prepared delivery entry
     *
     * @return boolean -> true when the direct send or transfer starts
     */
    function send_entry(owner, entry)
    {
        if (entry.bytes > local_transfer.RAW_CHUNK_BYTES)
        {
            entry.transfer_complete = false;
            owner.active_delivery = entry.id;
            const result = owner.transfers.send_frame(entry.text, entry.transfer_metadata);
            if (result === "started")
            {
                entry.sent_page_id = owner.id;
                return true;
            }
            owner.active_delivery = null;
            return false;
        }
        if (send(owner, entry.text))
        {
            entry.transfer_complete = true;
            entry.sent_page_id = owner.id;
            if (!entry.retained)
            {
                remove_pending(entry);
            }
            return true;
        }
        return false;
    }

    /**
     * @brief rebuilds an authenticated page from the current session and retained content
     *
     * @param object owner -> current page connection
     *
     * @return void
     */
    function send_burst(owner)
    {
        const state = snapshot();
        if (!is_current(owner) || state.authenticated !== true || state.g_is_client_list_retrieved !== true)
        {
            return;
        }
        const current_channel = state.g_channel_list.find(channel => channel.channel_id === state.current_channel_id);
        if (current_channel && current_channel.has_maintainer === true && (!Number.isSafeInteger(current_channel.maintainer_id) || current_channel.maintainer_id < 0 || !Number.isSafeInteger(current_channel.maintainer_generation) || current_channel.maintainer_generation < 0))
        {
            return;
        }
        const auth = cached_auth || bundle.get_auth_frame();
        if (typeof auth !== "string")
        {
            return;
        }
        owner.session_id = state.session_id;
        if (owner.transfers)
        {
            owner.transfers.close();
        }
        owner.transfers = local_transfer.create({
            session_id: owner.session_id,
            send: function(message)
            {
                return send_message(owner, message);
            },
            on_complete: function(metadata)
            {
                owner.active_delivery = null;
                const entry = pending.get(metadata.delivery_id);
                if (entry && is_current(owner) && entry.sent_page_id === owner.id)
                {
                    entry.transfer_complete = true;
                    if (!entry.retained)
                    {
                        remove_pending(entry);
                    }
                }
                setImmediate(function()
                {
                    if (is_current(owner))
                    {
                        send_pending(owner);
                    }
                });
            },
            on_failure: function(metadata, reason)
            {
                owner.active_delivery = null;
                const entry = pending.get(metadata.delivery_id);
                if (entry && is_current(owner) && owner.session_id === snapshot().session_id)
                {
                    entry.failed_page_id = owner.id;
                    entry.transfer_complete = false;
                    if (!entry.retained)
                    {
                        remove_pending(entry);
                    }
                    console.warn("loopback: local content transfer failed (" + reason + ")");
                    if (metadata.content_type === "direct_chat_file_decrypted" || metadata.content_type === "channel_chat_file_decrypted")
                    {
                        send_message(owner, { type: "chat_file_decrypt_failed", file_id: metadata.file_id,
                            reason: "could not receive this file" });
                    }
                    else if (metadata.content_type === "direct_chat_picture_decrypted" || metadata.content_type === "channel_chat_picture_decrypted")
                    {
                        send_message(owner, { type: "chat_picture_decrypt_failed", picture_id: metadata.picture_id,
                            reason: "could not receive this image" });
                    }
                    else
                    {
                        send_message(owner, { type: "loopback_status",
                            value: { state: "error", reason: "A message could not be received on this device." } });
                    }
                }
                setImmediate(function()
                {
                    if (is_current(owner))
                    {
                        send_pending(owner);
                    }
                });
            }
        });
        send_message(owner, Object.assign(peer_state(state), {
            type: "local_session_state",
            session_id: state.session_id,
            public_key_string: state.public_key_string,
            identity_revision: state.identity_revision,
            local_client_id: state.local_client_id,
            current_channel_id: state.current_channel_id
        }));
        send(owner, auth);
        send_message(owner, { type: "channel_list", channels: state.g_channel_list });
        send_message(owner, { type: "client_list", clients: state.g_client_list, local_username: state.g_local_username });
        for (const channel of state.g_channel_list)
        {
            if (channel.has_maintainer === true && Number.isSafeInteger(channel.maintainer_id) && channel.maintainer_id >= 0 && Number.isSafeInteger(channel.maintainer_generation) && channel.maintainer_generation >= 0)
            {
                send_message(owner, {
                    type: "channel_maintainer_id",
                    channel_id: channel.channel_id,
                    maintainer_id: channel.maintainer_id,
                    maintainer_generation: channel.maintainer_generation,
                    has_maintainer: true
                });
            }
        }
        send_message(owner, { type: "group_list", groups: state.g_groups, access: state.group_access || {}, channel_groups: state.channel_groups || [] });
        send_message(owner, {
            type: "icon_list",
            icons: state.g_icons.map(function(icon)
            {
                return { icon_id: icon.id, base64_icon: icon.base64_icon };
            })
        });
        if (state.server_policy)
        {
            send_message(owner, Object.assign({ type: "server_policy" }, state.server_policy));
        }
        for (const entry of cached_channel_keys.values())
        {
            send(owner, entry.text);
        }
        if (cached_media_keys !== null)
        {
            send(owner, cached_media_keys);
        }
        owner.needs_burst = false;
        send_pending(owner);
    }

    /**
     * @brief reserves bounded replay storage and binds content to its sender and session
     *
     * @param object frame -> frame to annotate and retain
     * @param object state -> shared state snapshot
     * @param boolean deferred -> true to wait for an explicit release
     * @param boolean retained -> true to retain content until the page accepts it
     *
     * @return object|null -> queued delivery, or null when discarded or refused
     */
    function retain(frame, state, deferred, retained = true)
    {
        const id = delivery_prefix + ":" + (++delivery_id);
        const session_id = state.authenticated === true ? state.session_id : null;
        const attachment = retained ? attachment_details(frame.message) : null;
        if (attachment)
        {
            const key = attachment_key({session_id, epoch: transport_epoch, attachment});
            if (discarded_attachments.has(key))
            {
                if (attachment.role === "terminal")
                {
                    discarded_attachments.delete(key);
                }
                return null;
            }
        }
        const sender = retained ? sender_projection(frame, state, attachment, session_id) : null;
        delete frame.local_sender;
        if (sender)
        {
            frame.local_sender = sender;
        }
        if (retained)
        {
            frame.local_delivery = { id, session_id };
        }
        const text = JSON.stringify(frame);

        const bytes = Buffer.byteLength(text, "utf8") + (session_id === null ? 128 : 0);
        if (pending.size + discarded_attachments.size >= limits.replayQueueCount || pending_bytes + cached_channel_key_bytes + bytes > limits.replayQueueBytes)
        {
            // stop intake when retained content exceeds memory capacity

            bundle.node_fail_delivery("Pending messages exceed this device's queue capacity. Reopen the chat.");
            return null;
        }
        const entry = { id, sequence: delivery_id, text, bytes, session_id, retained,
            observed_page_id: current !== null && !current.needs_burst ? current.id : 0,
            frame: session_id === null || deferred ? frame : null, released: !deferred,
            epoch: transport_epoch, sent_page_id: 0, failed_page_id: 0, transfer_complete: false,
            transfer_metadata: { delivery_id: id, content_type: frame.message.type,
                file_id: frame.message.file_id, picture_id: frame.message.picture_id } };
        entry.sender = sender;
        entry.attachment = attachment;
        entry.subject = retained ? delivery_subject(frame.message, sender, attachment) : null;
        entry.dependency_id = retained ? dependency_for(entry.subject, session_id) : null;
        if (entry.attachment)
        {
            entry.attachment_deadline = Date.now() + 300000;
            for (const candidate of pending.values())
            {
                if (same_attachment(entry, candidate))
                {
                    entry.attachment_deadline = Math.min(entry.attachment_deadline, candidate.attachment_deadline);
                    if (entry.attachment.prepared && (candidate.attachment.role === "chunk" || candidate.attachment.role === "direct_complete" || (candidate.attachment.role === "terminal" && !candidate.attachment.prepared)))
                    {
                        // retain prepared content with its metadata for page replay

                        remove_pending(candidate);
                    }
                }
            }
        }
        if (retained && session_id !== null && !track_delivery(entry, frame))
        {
            return null;
        }
        pending.set(id, entry);
        pending_bytes += bytes;
        return entry;
    }

    /**
     * @brief recognizes a decrypted channel key delivery
     *
     * @param object message -> message fields
     *
     * @return boolean|null|undefined -> truthy only for channel keys from a maintainer
     */
    function is_channel_keys(message)
    {
        return message.type === "direct_chat_message" && message.some_json && message.some_json.type === "channel_keys_from_maintainer";
    }

    /**
     * @brief caches session state needed by newly attached pages
     *
     * @param object frame -> parsed frame
     * @param string json_string -> serialized frame
     *
     * @return boolean -> true when the frame still needs ordinary delivery
     */
    function cache_state_frame(frame, json_string)
    {
        const message = frame.message;
        if (message.type === "local_identity_result")
        {
            cached_identity = json_string;
            if (current !== null)
            {
                send(current, cached_identity);
            }
            return false;
        }
        if (message.type === "authentication_status")
        {
            cached_auth = json_string;
        }
        if (message.type === "local_media_keys")
        {
            cached_media_keys = json_string;
        }
        if (is_channel_keys(message))
        {
            const id = String(message.some_json.channel_id);
            const previous = cached_channel_keys.get(id);
            const bytes = Buffer.byteLength(json_string, "utf8");
            const total = cached_channel_key_bytes - (previous ? previous.bytes : 0) + bytes;
            if ((!previous && cached_channel_keys.size >= limits.replayQueueCount) || total + pending_bytes > limits.replayQueueBytes)
            {
                bundle.node_fail_delivery("Channel key replay exceeds this device's queue capacity.");
                return false;
            }
            cached_channel_keys.set(id, {text: json_string, bytes});
            cached_channel_key_bytes = total;
        }
        return true;
    }

    /**
     * @brief reserves, releases or forwards a frame after shared client processing
     *
     * @param string json_string -> serialized frame
     * @param object|undefined options -> optional delivery reservation or release controls
     *
     * @return string|null|undefined -> reserved delivery ID, null on refusal, or undefined when no delivery is queued
     */
    function publish_frame(json_string, options)
    {
        if (options && typeof options.release_delivery === "string")
        {
            const token = options.release_delivery;
            const entry = pending.get(token);
            if (!entry || entry.released || entry.epoch !== transport_epoch)
            {
                return;
            }
            entry.released = true;
            if (!entry.retained)
            {
                const message = entry.frame.message;
                const needs_send = cache_state_frame(entry.frame, entry.text);
                const state = snapshot();
                if (!needs_send || current === null || current.needs_burst || !state.authenticated || current.session_id !== entry.session_id || state.session_id !== entry.session_id || (entry.observed_page_id !== current.id && !is_channel_keys(message) && message.type !== "local_media_keys"))
                {
                    remove_pending(entry);
                }
                entry.frame = null;
            }
            else if (entry.frame && entry.session_id !== null)
            {
                track_delivery(entry, entry.frame);
                entry.frame = null;
            }
            if (current !== null && !current.needs_burst)
            {
                send_pending(current);
            }
            return;
        }
        let frame;
        try
        {
            frame = JSON.parse(json_string);
        }
        catch (error)
        {
            return;
        }
        if (!frame || !frame.message || typeof frame.message.type !== "string")
        {
            return;
        }
        const state = snapshot();
        const message = frame.message;
        const retained = retained_types.has(message.type) && !is_channel_keys(message);
        const deferred = options && options.defer_delivery === true;
        if (deferred)
        {
            const entry = retain(frame, state, true, retained);
            return entry === null ? null : entry.id;
        }
        if (!cache_state_frame(frame, json_string))
        {
            return;
        }
        if (retained || (current !== null && !current.needs_burst && state.authenticated && current.session_id === state.session_id))
        {
            const entry = retain(frame, state, false, retained);
            if (entry !== null && entry.released && current !== null && !current.needs_burst && current.session_id === entry.session_id)
            {
                send_pending(current);
            }
            return entry === null ? null : entry.id;
        }
    }
    bundle.set_frame_listener(publish_frame);

    /**
     * @brief clears retained content and cached state for an ended session
     *
     * @return void
     */
    function reset_delivery_projection()
    {
        discarded_attachments.clear();
        cached_auth = null;
        cached_media_keys = null;
        cached_channel_keys.clear();
        cached_channel_key_bytes = 0;
        for (const [id, entry] of pending)
        {
            if (!entry.released)
            {
                pending.delete(id);
                pending_bytes -= entry.bytes;
            }
        }
        transport_epoch++;
        revoke(current);
    }

    bundle.set_on_message_processed(function(type, failed)
    {
        if (failed === true)
        {
            // revoke reservations before the remote close completes

            reset_delivery_projection();
            bundle.node_fail_delivery("Could not process an incoming message. Reconnect to continue.");
            return;
        }
        if (type === "websocket_worker_onclose" || type === "websocket_worker_onerror")
        {
            reset_delivery_projection();
        }
        const state = snapshot();
        if (state.authenticated)
        {
            for (const [context_id, delivery] of latest_delivery_by_context)
            {
                if (delivery.session_id !== state.session_id)
                {
                    latest_delivery_by_context.delete(context_id);
                }
            }
            for (const [id, entry] of pending)
            {
                if ((entry.session_id !== null && entry.session_id !== state.session_id) || (entry.session_id === null && entry.epoch !== transport_epoch))
                {
                    pending.delete(id);
                    pending_bytes -= entry.bytes;
                    console.warn("loopback: previous-session delivery expired");
                }
            }
        }
        if (current !== null && current.needs_burst)
        {
            send_burst(current);
        }
    });
    bundle.set_connection_status_listener(status_to_ui);
    bundle.set_unread_listener(send_peer_state);

    /**
     * @brief applies an allowed page command and reports whether it was accepted
     *
     * @param object owner -> page connection owner
     * @param object message -> command, arguments and request ID
     *
     * @return void
     */
    function handle_local_command(owner, message)
    {
        const args = message.arguments || {};
        const state = snapshot();
        if (message.action !== "connect" && message.action !== "disconnect" && (!state.authenticated || owner.needs_burst || owner.session_id !== state.session_id))
        {
            send_message(owner, { type: "local_command_result", request_id: message.request_id, accepted: false });
            return;
        }
        let accepted = false;
        switch (message.action)
        {
            case "connect":
                bundle.node_request_connect();
                accepted = true;
                break;
            case "disconnect":
                bundle.node_disconnect_by_user();
                accepted = true;
                break;
            case "set_local_ignore":
                accepted = bundle.node_set_local_ignore(args) === true;
                if (accepted)
                {
                    send_peer_state();
                }
                break;
            case "read_conversation":
                const latest = latest_delivery_by_context.get(args.context_id);
                if (latest && latest.session_id === owner.session_id && latest.acknowledged && latest.id === args.delivery_id)
                {
                    accepted = bundle.node_read_conversation(args) === true;
                }
                break;
        }
        send_message(owner, { type: "local_command_result", request_id: message.request_id, accepted });
    }

    /**
     * @brief applies the page's content acknowledgement
     *
     * @param object owner -> page connection that sent the request
     * @param object message -> parsed local request
     *
     * @return void
     */
    function handle_delivery_acknowledgement(owner, message)
    {
        const entry = pending.get(message.id);
        if (entry && entry.transfer_complete && entry.sent_page_id === owner.id && entry.session_id === owner.session_id && message.session_id === owner.session_id && (message.accepted === undefined || typeof message.accepted === "boolean") && (message.discard_attachment === undefined || typeof message.discard_attachment === "boolean"))
        {
            if (message.accepted === false)
            {
                entry.failed_page_id = owner.id;
                return;
            }
            entry.model_accepted_page_id = owner.id;
            if (message.discard_attachment === true)
            {
                if (!entry.attachment || entry.attachment.role !== "metadata")
                {
                    return;
                }
                discard_attachment(entry, owner);
            }
            else if (!entry.attachment)
            {
                if (!pending.has(entry.dependency_id))
                {
                    remove_pending(entry);
                }
            }
            else if (entry.attachment.role === "terminal")
            {
                finish_attachment(entry);
            }
            // retain metadata until the page accepts the complete attachment

            const latest = latest_delivery_by_context.get(entry.context_id);
            if (latest && latest.id === entry.id)
            {
                latest.acknowledged = true;
            }
            send_pending(owner);
        }
    }

    /**
     * @brief submits one upload for the current page
     *
     * @param object owner -> page connection that sent the request
     * @param object message -> parsed local request
     *
     * @return void
     */
    function handle_upload_request(owner, message)
    {
        const request_id = message.request_id;
        if (typeof request_id !== "string" || request_id.length > 128 || !message.request || typeof message.request !== "object" || Array.isArray(message.request))
        {
            return;
        }
        if (owner.pending_upload)
        {
            send_message(owner, { type: "local_upload_result", request_id, accepted: false });
            return;
        }
        owner.pending_upload = true;
        const session_id = owner.session_id;
        Promise.resolve().then(function()
        {
            if (!is_current(owner) || snapshot().session_id !== session_id)
            {
                return false;
            }
            return bundle.node_forward_upload_request(message.request);
        }).then(function(accepted)
        {
            if (is_current(owner) && snapshot().session_id === session_id)
            {
                send_message(owner, { type: "local_upload_result", request_id, accepted: accepted === true });
            }
        }).catch(function()
        {
            if (is_current(owner) && snapshot().session_id === session_id)
            {
                send_message(owner, { type: "local_upload_result", request_id, accepted: false });
            }
        }).finally(function()
        {
            owner.pending_upload = false;
        });
    }

    /**
     * @brief queues a bounded page request for ordered forwarding to the current session
     *
     * @param object owner -> page connection owner
     * @param string text -> serialized remote request
     *
     * @return void
     */
    function queue_request(owner, text)
    {
        const bytes = Buffer.byteLength(text, "utf8");
        if (owner.requests.length >= limits.requestQueueCount || owner.request_bytes + bytes > limits.websocketQueueBytes)
        {
            revoke(owner);
            return;
        }
        owner.requests.push({ text, bytes, session_id: owner.session_id });
        owner.request_bytes += bytes;
        if (owner.request_scheduled)
        {
            return;
        }
        owner.request_scheduled = true;
        // forward one current-session request per turn so other socket work can run
        setImmediate(function drain()
        {
            if (!is_current(owner) || owner.requests.length === 0)
            {
                owner.request_scheduled = false;
                owner.requests = [];
                owner.request_bytes = 0;
                return;
            }
            const item = owner.requests.shift();
            owner.request_bytes -= item.bytes;
            const state = snapshot();
            if (state.authenticated && state.session_id === item.session_id)
            {
                try
                {
                    bundle.node_forward_raw_request(item.text);
                }
                catch (error)
                {
                    console.warn("loopback: request failed");
                }
            }
            setImmediate(drain);
        });
    }

    server.on("connection", function(socket)
    {
        const owner = { socket, id: ++page_connection_id, session_id: "", needs_burst: true, revoked: false,
            pending_upload: false, requests: [], request_bytes: 0, request_scheduled: false };
        let authenticated = false;
        socket.on("message", function(data, binary)
        {
            try
            {
                if (binary || owner.revoked)
                {
                    socket.terminate();
                    return;
                }
                const text = data.toString("utf8");
                if (!authenticated)
                {
                    if (text !== token)
                    {
                        socket.terminate();
                        return;
                    }
                    authenticated = true;
                    server.authenticate(socket);
                    revoke(current);
                    current = owner;
                    status_to_ui(bundle.get_connection_status());
                    if (cached_identity !== null)
                    {
                        send(owner, cached_identity);
                    }
                    send_burst(owner);
                    return;
                }
                if (!is_current(owner))
                {
                    return;
                }
                const frame = JSON.parse(text);
                if (!frame || !frame.message || typeof frame.message !== "object" || Array.isArray(frame.message))
                {
                    return;
                }
                const message = frame.message;
                if (message.type === "local_transfer_ack")
                {
                    if (owner.transfers && owner.session_id === snapshot().session_id)
                    {
                        owner.transfers.acknowledge(message);
                    }
                    return;
                }
                if (message.type === "local_command")
                {
                    handle_local_command(owner, message);
                    return;
                }
                if (message.type === "local_delivery_ack")
                {
                    handle_delivery_acknowledgement(owner, message);
                    return;
                }
                const state = snapshot();
                if (owner.needs_burst || state.authenticated !== true || owner.session_id !== state.session_id)
                {
                    return;
                }
                if (message.type === "local_upload_request")
                {
                    handle_upload_request(owner, message);
                    return;
                }
                queue_request(owner, text);
            }
            catch (error)
            {
                console.warn("loopback: rejected malformed request");
            }
        });
        socket.on("close", function()
        {
            owner.revoked = true;
            if (owner.transfers)
            {
                owner.transfers.close();
            }
            owner.requests = [];
            owner.request_bytes = 0;
            if (current === owner)
            {
                current = null;
                if (owner.pending_upload && typeof bundle.node_cancel_upload_request === "function")
                {
                    bundle.node_cancel_upload_request();
                }
            }
        });
        socket.on("error", function()
        {
            // the socket library closes failed transports
        });
    });
    const watchdog = setInterval(function()
    {
        expire_incomplete_attachments();
        if (current !== null)
        {
            const bytes = current.socket.bufferedAmount;
            if (bytes > 0 && bytes >= current.last_buffered && Date.now() - current.last_progress >= limits.writeTimeoutMs)
            {
                current.socket.terminate();
                return;
            }
            if (bytes === 0 || bytes < current.last_buffered || current.last_progress === undefined)
            {
                current.last_progress = Date.now();
            }
            current.last_buffered = bytes;
        }
    }, 1000);
    watchdog.unref();
    server.on("listening", function()
    {
        if (typeof on_ready === "function")
        {
            on_ready(server.address().port, token);
        }
    });
    server.on("error", function()
    {
        console.error("loopback: UI listener unavailable; native control remains active");
    });
    return {
        /**
         * @brief forwards serialized output to the current page when attached
         *
         * @param string json_string -> frame to send
         *
         * @return void
         */
        forward_to_ui: function(json_string)
        {
            if (current !== null)
            {
                send(current, json_string);
            }
        },
        /**
         * @brief stops page expiry checks, revokes the page and closes the listener
         *
         * @param function|undefined callback -> optional listener close callback
         *
         * @return void
         */
        close: function(callback)
        {
            clearInterval(watchdog);
            revoke(current);
            server.close(callback);
        }
    };
}

module.exports = { start };
