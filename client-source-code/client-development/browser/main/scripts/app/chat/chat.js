// composes messages and attachments and sends them through the encryption worker
// tracks typing, unread counts and seen receipts for each conversation
// UI code starts sends. messages.js and dispatch.js apply incoming state

// state private to this file
var picture_delivery_pending = false; // between the server's upload ack and image_sent_status (relay to the receivers finished)

var picture_delivery_hide_timer = null;

var upload_ack_timer = null;
var upload_generation = 0;
var picture_preparation = null;
var picture_operation_sequence = 0;
var chat_peer_local_state = new Map();
var chat_last_delivery_by_context = new Map();

/**
 * @brief finds the conversation key for a retained text or attachment delivery
 *
 * @param object frame -> received frame with its message and optional retained sender
 *
 * @return string -> conversation key, or an empty string for unsupported or invalid content
 */
function chat__delivery_context(frame)
{
    const message = frame && frame.message;
    if (!message || typeof message.type !== "string")
    {
        return "";
    }
    if (message.type === "offline_chat_message")
    {
        return typeof message.sender_identity_hash === "string" ? "identity:" + message.sender_identity_hash : "";
    }
    if (/^channel_chat_(message|picture|file)(?:_metadata|_decrypted)?$/.test(message.type))
    {
        return Number.isSafeInteger(message.channel_id) ? "chat-context-channel-" + message.channel_id : "";
    }
    if (/^direct_chat_(message|picture|file)(?:_metadata|_decrypted)?$/.test(message.type))
    {
        if (message.some_json && message.some_json.type !== "direct_chat_message")
        {
            return "";
        }
        const sender = chat__resolve_direct_sender(frame);
        return sender ? chat__peer_state_key(sender) : "";
    }
    return "";
}

/**
 * @brief reports a conversation read through its latest accepted delivery
 *
 * @param string context_id -> conversation key used for delivery tracking
 * @param object arguments_value -> additional local read command arguments
 *
 * @return void
 */
function chat__send_read_command(context_id, arguments_value)
{
    const delivery_id = chat_last_delivery_by_context.get(context_id);
    if (delivery_id)
    {
        connection__send_local_command("read_conversation", Object.assign({}, arguments_value,
        {
            context_id: context_id,
            delivery_id: delivery_id,
            send_seen_receipts: g_send_seen_receipts === true
        }));
    }
}

/**
 * @brief identifies peer state by identity hash, falling back to the public key
 *
 * @param object|null client -> client or resolved sender
 *
 * @return string -> stable peer key, or an empty string when unavailable
 */
function chat__peer_state_key(client)
{
    if (client == null)
    {
        return "";
    }
    return client.identity_hash ? "identity:" + client.identity_hash : (client.public_key ? "key:" + client.public_key : "");
}

/**
 * @brief parses chat text or a permitted encrypted edit or reaction
 *
 * @param string value -> decrypted JSON contents
 * @param boolean allow_controls -> whether edits and reactions may be returned
 *
 * @return object|null the parsed message or null for invalid content
 */
function chat__read_message_payload(value, allow_controls = true)
{
    if (typeof value !== "string")
    {
        return null;
    }
    let payload;
    try
    {
        payload = JSON.parse(value);
    }
    catch (invalid_content)
    {
        return null;
    }
    if (payload == null || typeof payload !== "object" || Array.isArray(payload))
    {
        return null;
    }
    if (allow_controls && (payload.type === "chat_message_edit" || payload.type === "chat_reaction"))
    {
        return payload;
    }
    return typeof payload.value === "string" ? payload : null;
}

// a retained message belongs to the identity that sent it, even after its transport slot is reused
// only trusted local delivery carries Node's sender snapshot. it does not create a live member
/**
 * @brief resolves a direct sender while checking retained sender data against the local session
 *
 * @param object frame -> direct message frame with optional local sender metadata
 *
 * @return object|null -> resolved sender and conversation, or null when invalid
 */
function chat__resolve_direct_sender(frame)
{
    const message = frame && frame.message;
    if (!message)
    {
        return null;
    }
    let client = channel_tree__get_client_by_client_id(message.sender_id);
    const retained = frame.local_sender;
    if (retained != null)
    {
        if (!android_host__is_ui_only_runtime() || !frame.local_delivery || frame.local_delivery.session_id !== node_session_id || typeof retained !== "object" || Array.isArray(retained) || !Number.isSafeInteger(retained.client_id) || retained.client_id !== message.sender_id || typeof retained.public_key !== "string" || !retained.public_key || retained.public_key.length > 16384 || typeof retained.identity_hash !== "string" || retained.identity_hash.length > 128 || (retained.identity_hash && !/^[A-Za-z0-9+/=]+$/.test(retained.identity_hash)) || typeof retained.username !== "string" || retained.username.length > 512)
        {
            return null;
        }
        client = g_client_list.find(function(candidate)
        {
            return candidate && candidate.public_key === retained.public_key;
        }) || null;
    }
    if (!retained && !client)
    {
        return null;
    }
    const source = retained || client;
    const sender =
    {
        client: client,
        client_id: client ? client.client_id : null,
        public_key: source.public_key || "",
        identity_hash: source.identity_hash || "",
        username: retained ? retained.username : (client.username || message.sender_username || "")
    };
    const saved = chat_peer_local_state.get(chat__peer_state_key(sender));
    sender.is_ignored_by_local_client = Boolean((client && client.is_ignored_by_local_client) || (saved && saved.is_ignored_by_local_client));
    if (client)
    {
        sender.context_id = "chat-context-pm-" + client.client_id;
    }
    else if (sender.identity_hash)
    {
        sender.context_id = "chat-context-offline-" + sender.identity_hash;
    }
    else
    {
        const existing = g_chat_context_array.find(function(context)
        {
            return context.type === "user" && context.peer_public_key === sender.public_key;
        });
        let suffix = g_chat_context_array.length;
        sender.context_id = existing ? existing.chat_context_id : "chat-context-offline-key-" + suffix;
        while (!existing && chat__get_chat_context_index_by_chat_context_id(sender.context_id) >= 0)
        {
            sender.context_id = "chat-context-offline-key-" + (++suffix);
        }
    }
    return sender;
}

/**
 * @brief creates or verifies a conversation bound to the resolved sender's public key
 *
 * @param object sender -> sender returned by chat__resolve_direct_sender
 *
 * @return boolean -> true when the sender's conversation is available
 */
function chat__ensure_direct_context(sender)
{
    let index = chat__get_chat_context_index_by_chat_context_id(sender.context_id);
    if (index >= 0)
    {
        const context = g_chat_context_array[index];
        if (context.peer_public_key && context.peer_public_key !== sender.public_key)
        {
            return false;
        }
        if (G_HAS_DOM && document.getElementById(sender.context_id) == null)
        {
            return false;
        }
        context.peer_public_key = sender.public_key;
        context.peer_identity_hash = sender.identity_hash;
        return true;
    }
    const container = document.getElementById("chat-context-container");
    if (container == null)
    {
        return false;
    }
    container.insertAdjacentHTML("beforeend", '<div class="chat-context" id="' + chat__sanitize_string(sender.context_id) + '" style="display: none;"><div class="single-server-message">now talking to user: ' + chat__sanitize_string(sender.username) + '</div><div class="single-server-message server-message-public-key">your public key: ' + chat__sanitize_string(g_rsa_public_key_string) + '</div><div class="single-server-message server-message-public-key">their public key: ' + chat__sanitize_string(sender.public_key) + '</div><div class="single-server-message"></div></div>');
    g_chat_context_array.push(
    {
        type: "user",
        chat_context_id: sender.context_id,
        peer_public_key: sender.public_key,
        peer_identity_hash: sender.identity_hash,
        peer_username: sender.username,
        last_known_message_sender_username: ""
    });
    if (sender.client == null)
    {
        const selector_id = sender.context_id.slice("chat-context-".length);
        const selectors = document.getElementById("chat-context-selectors-container");
        if (selectors && document.querySelector('[data-chat-context-selector-id="' + selector_id + '"]') == null)
        {
            selectors.insertAdjacentHTML("beforeend", '<div class="chat-context-selector" data-chat-context-selector-type="user" data-chat-context-selector-id="' + selector_id + '"><div class="p-container"><p>' + chat__sanitize_string(sender.username) + '</p></div><div class="remove-chat-context-selector" data-chat-context-remove-selector-type="user" data-chat-context-remove-selector-id="' + selector_id + '"></div></div>');
            for (const selector of document.getElementsByClassName("chat-context-selector"))
            {
                selector.onclick = UI.chat_context_selector_onclick;
            }
            for (const selector of document.getElementsByClassName("remove-chat-context-selector"))
            {
                selector.onclick = UI.chat_context_remove_onclick;
            }
        }
    }
    return true;
}

/**
 * @brief notifies Android and updates unread counts or read receipts for a direct message
 *
 * @param object sender -> resolved sender and conversation
 * @param number|null message_id -> message ID for a receipt, or null for activity only
 *
 * @return void
 */
function chat__record_direct_activity(sender, message_id)
{
    android_host__notify({type: "chat_activity", sender_id: sender.client_id, username: sender.username, context_id: sender.context_id});
    if (sender.client == null)
    {
        UI.schedule_member_list_sync();
        return;
    }
    if (g_current_chat_context_id !== sender.context_id || typeof process !== "undefined")
    {
        chat__increment_unread_count(sender.client_id);
        if (message_id != null)
        {
            chat__remember_message_awaiting_receipt(sender.client_id, message_id);
        }
        chat__render_unread_badge(sender.client_id, true);
    }
    else if (message_id != null)
    {
        if (chat__is_the_user_actually_looking())
        {
            chat__send_seen_receipt_for_message(sender.client_id, message_id);
        }
        else
        {
            chat__remember_message_awaiting_receipt(sender.client_id, message_id);
        }
    }
}

/**
 * @brief keeps unread, ignore and mute state by identity before client slots change
 *
 * @return void
 */
function chat__remember_peer_state()
{
    chat__bind_private_contexts();
    for (const client of g_client_list)
    {
        const key = chat__peer_state_key(client);
        if (key)
        {
            chat_peer_local_state.set(key,
            {
                unread_count: client.unread_count || 0,
                is_ignored_by_local_client: client.is_ignored_by_local_client === true,
                is_muted_by_local_client: client.is_muted_by_local_client === true
            });
        }
    }
}

/**
 * @brief restores saved peer state onto the current client list and refreshes the app badge
 *
 * @return void
 */
function chat__restore_peer_state()
{
    for (const client of g_client_list)
    {
        const state = chat_peer_local_state.get(chat__peer_state_key(client));
        if (state)
        {
            Object.assign(client, state);
        }
    }
    chat__update_app_unread_badge();
}

/**
 * @brief combines saved and connected peer state for page attachment
 *
 * @return array -> peer records keyed by identity hash or public key
 */
function chat__snapshot_peer_states()
{
    const states = new Map(chat_peer_local_state);
    for (const client of g_client_list)
    {
        const key = chat__peer_state_key(client);
        if (key)
        {
            states.set(key,
            {
                unread_count: client.unread_count || 0,
                is_ignored_by_local_client: client.is_ignored_by_local_client === true,
                is_muted_by_local_client: client.is_muted_by_local_client === true
            });
        }
    }
    return Array.from(states, function(entry)
    {
        return Object.assign({key: entry[0]}, entry[1]);
    });
}

/**
 * @brief validates the complete peer snapshot before replacing local peer state
 *
 * @param array states -> peer records from the local runtime
 *
 * @return void
 *
 * @throws Error -> a peer record has invalid keys, counters or flags
 */
function chat__hydrate_peer_states(states)
{
    if (!Array.isArray(states))
    {
        return;
    }
    const replacement = new Map();
    for (const state of states)
    {
        if (!state || typeof state.key !== "string" || !/^(identity:|key:).+/.test(state.key) || !Number.isSafeInteger(state.unread_count) || state.unread_count < 0 || typeof state.is_ignored_by_local_client !== "boolean" || typeof state.is_muted_by_local_client !== "boolean")
        {
            throw new Error("Invalid local peer state");
        }
        replacement.set(state.key,
        {
            unread_count: state.unread_count,
            is_ignored_by_local_client: state.is_ignored_by_local_client,
            is_muted_by_local_client: state.is_muted_by_local_client
        });
    }
    chat_peer_local_state = replacement;
}

/**
 * @brief validates channel unread counts before replacing and rendering them
 *
 * @param array states -> channel IDs and unread counts from the local runtime
 *
 * @return void
 *
 * @throws Error -> a channel ID or unread count is invalid
 */
function chat__hydrate_channel_counts(states)
{
    if (!Array.isArray(states))
    {
        return;
    }
    const counts = {};
    for (const state of states)
    {
        if (!state || !Number.isSafeInteger(state.channel_id) || state.channel_id < 0 || !Number.isSafeInteger(state.unread_count) || state.unread_count < 0)
        {
            throw new Error("Invalid local channel unread state");
        }
        counts[state.channel_id] = state.unread_count;
    }
    g_channel_unread_counts = counts;
    for (const key of Object.keys(counts))
    {
        chat__render_channel_unread_badge(Number(key));
    }
}

/**
 * @brief binds existing private conversations to their current peer's public key
 *
 * @return void
 */
function chat__bind_private_contexts()
{
    for (const context of g_chat_context_array)
    {
        const match = /^chat-context-pm-([0-9]+)$/.exec(context.chat_context_id);
        if (context.type !== "user" || !match || context.peer_public_key)
        {
            continue;
        }
        const client = channel_tree__get_client_by_client_id(Number(match[1]));
        if (client && client.public_key)
        {
            context.peer_public_key = client.public_key;
            context.peer_identity_hash = client.identity_hash || "";
        }
    }
}

/**
 * @brief checks that a private conversation still belongs to the client occupying its slot
 *
 * @param string context_id -> private conversation ID
 * @param number client_id -> current recipient slot
 *
 * @return string|null -> matching peer public key, or null after warning about a changed connection
 */
function chat__private_recipient_key(context_id, client_id)
{
    const index = chat__get_chat_context_index_by_chat_context_id(context_id);
    const client = channel_tree__get_client_by_client_id(client_id);
    const context = index < 0 ? null : g_chat_context_array[index];
    if (!context || !client || !context.peer_public_key || context.peer_public_key !== client.public_key)
    {
        utils__custom_alert("This person's connection changed. Open their conversation again before sending.");
        return null;
    }
    return context.peer_public_key;
}

/**
 * @brief rebinds private conversations by public key after clients leave or change slots
 *
 * @return void
 */
function chat__reconcile_private_contexts()
{
    const moves = [];
    for (const context of g_chat_context_array)
    {
        if (context.type !== "user" || !/^chat-context-pm-/.test(context.chat_context_id))
        {
            continue;
        }
        const client = g_client_list.find(function(candidate)
        {
            return context.peer_public_key && candidate.public_key === context.peer_public_key;
        });
        const previous = context.chat_context_id;
        let next = client ? "chat-context-pm-" + client.client_id : "chat-context-offline-" + (context.peer_identity_hash || "detached-" + previous.slice(16));
        if (!client && !context.peer_identity_hash)
        {
            const base = next;
            let suffix = 0;
            while (g_chat_context_array.some(function(candidate)
            {
                return candidate !== context && candidate.chat_context_id === next;
            }) || moves.some(function(move)
            {
                return move.next === next;
            }))
            {
                next = base + "-" + (++suffix);
            }
        }
        if (previous === next)
        {
            continue;
        }
        const previous_selector = "user-" + previous.slice("chat-context-pm-".length);
        moves.push(
        {
            context: context,
            client: client,
            next: next,
            element: document.getElementById(previous),
            selectors: Array.from(document.querySelectorAll('[data-chat-context-selector-id="' + previous_selector + '"]')),
            remove_selectors: Array.from(document.querySelectorAll('[data-chat-context-remove-selector-id="' + previous_selector + '"]')),
            was_open: g_current_chat_context_id === previous
        });
    }
    // capture all old DOM references before renaming: two peers may exchange transport slots
    for (const move of moves)
    {
        if (move.element)
        {
            move.element.id = move.next;
        }
        const selector_id = move.client ? "user-" + move.client.client_id : move.next.slice("chat-context-".length);
        for (const selector of move.selectors)
        {
            selector.setAttribute("data-chat-context-selector-id", selector_id);
        }
        for (const selector of move.remove_selectors)
        {
            selector.setAttribute("data-chat-context-remove-selector-id", selector_id);
        }
        move.context.chat_context_id = move.next;
        if (move.was_open)
        {
            g_current_chat_context_id = move.next;
            g_chat_message_receiver_id = move.client ? move.client.client_id : null;
            g_offline_chat_recipient_identity_hash = move.client ? "" : (move.context.peer_identity_hash || "");
        }
    }
}

var UPLOAD_ACK_TIMEOUT_MS = 300000;   // a slow 10mb upload legitimately takes minutes

var TYPING_SEND_INTERVAL_MS = 3000;   // at most one message per 3s of continuous typing

var TYPING_EXPIRY_MS = 6000;          // twice the send interval, so one lost message does not flicker

var CHANNEL_CONTEXT_SEND_WARNING = "Join this channel before sending messages or attachments.";

// track private message receipts so each message is acknowledged once to its author
var seen_receipts_already_sent = {};

// track whether each private conversation has had our latest message read
var seen_state_by_context = {};

// retain receipts for private messages until their conversation is read
var unreceipted_messages_by_sender = {};
var chat_scroll_pending = false;

/**
 * @brief uses the current theme's text style for displayed messages when the local setting is on
 *
 * @return void
 */
function chat__apply_theme_formatting()
{
    let container = document.getElementById("chat-context-container");
    let sample = null;
    let paragraph = null;
    let appearance = null;

    if (container == null || document.body == null)
    {
        return;
    }

    document.body.classList.remove("chat-use-theme-formatting");
    if (g_use_theme_chat_formatting == false)
    {
        return;
    }

    // themes set message styles in different places. read a plain message once,
    // when this setting or the theme changes, without touching the saved messages
    sample = document.createElement("div");
    sample.className = "single-chat-message";
    sample.style.cssText = "position: absolute; visibility: hidden; pointer-events: none;";
    sample.setAttribute("aria-hidden", "true");
    sample.innerHTML = '<div class="single-chat-message-content"><p class="single-chat-message-content-p">Aa</p></div>';
    container.appendChild(sample);
    paragraph = sample.querySelector(".single-chat-message-content-p");
    appearance = getComputedStyle(paragraph);
    container.style.setProperty("--chat-theme-font-family", appearance.fontFamily);
    container.style.setProperty("--chat-theme-font-size", appearance.fontSize);
    container.style.setProperty("--chat-theme-line-height", appearance.lineHeight);
    container.style.setProperty("--chat-theme-text-color", appearance.color);
    sample.remove();
    document.body.classList.add("chat-use-theme-formatting");
}

// message composition and sending

/**
 * @brief index of a chat context in g_chat_context_array
 *
 * @param string id -> the context id, "chat-context-channel-N", "chat-context-pm-N" or "chat-context-offline-<identity hash>"
 *
 * @return number the index, -1 when not found
 */
function chat__get_chat_context_index_by_chat_context_id(id)
{
    for (var i = 0; i < g_chat_context_array.length; i++)
    {
        if (g_chat_context_array[i].chat_context_id == id)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief matches an offline conversation to the same identity after it reconnects
 *
 * @param client connected client whose identity is matched
 *
 * @return object -> promotion result with the previous and current context IDs and open state
 */
function chat__promote_offline_chat_context_state(client)
{
    let promotion = {
        did_promote: false,
        had_live_context: false,
        was_open: false,
        offline_context_id: "",
        live_context_id: ""
    };

    if (client == null || typeof client.identity_hash !== "string" || client.identity_hash.length == 0)
    {
        return promotion;
    }

    let offline_context_id = "chat-context-offline-" + client.identity_hash;
    let live_context_id = "chat-context-pm-" + client.client_id;

    let offline_index = -1;
    let has_live_context = false;

    for (let i = 0; i < g_chat_context_array.length; i++)
    {
        if (g_chat_context_array[i].chat_context_id == offline_context_id)
        {
            offline_index = i;
        }

        if (g_chat_context_array[i].chat_context_id == live_context_id)
        {
            has_live_context = true;
        }
    }

    if (offline_index == -1)
    {
        return promotion;
    }

    promotion.did_promote = true;
    promotion.had_live_context = has_live_context;
    promotion.was_open = (g_current_chat_context_id == offline_context_id);
    promotion.offline_context_id = offline_context_id;
    promotion.live_context_id = live_context_id;

    if (has_live_context == false)
    {
        // no live thread yet: the offline one simply becomes it, history and all
        g_chat_context_array[offline_index].chat_context_id = live_context_id;
        g_chat_context_array[offline_index].peer_public_key = client.public_key;
        g_chat_context_array[offline_index].peer_identity_hash = client.identity_hash;
    }
    else
    {
        // both exist: the offline entry is folded into the live one and stops existing
        g_chat_context_array.splice(offline_index, 1);
    }

    // if we were reading or typing in it, keep sending live from now on
    if (promotion.was_open == true)
    {
        chat_files__cancel_pending_read();
        g_current_chat_context_id = live_context_id;
        chat_replies__refresh();
        g_chat_message_receiver_type = "user";
        g_chat_message_receiver_id = client.client_id;
        g_offline_chat_recipient_identity_hash = "";
    }

    return promotion;
}

/**
 * @brief releases the upload lock and clears its progress overlay
 * the parts loop only queues data. it must wait for acknowledgement before releasing the lock
 *
 * @return void
 */
function chat__release_file_upload_lock()
{
    if (picture_preparation != null)
    {
        clearTimeout(picture_preparation.timer);
        picture_preparation = null;
    }
    upload_generation++;
    connection__cancel_upload_send();
    if (upload_ack_timer != null)
    {
        clearTimeout(upload_ack_timer);
        upload_ack_timer = null;
    }

    document.getElementById("upload-progress-overlay").style.display = "none";
    document.getElementById("upload-progress-bar").style.width = '0%';
    document.getElementById("upload-progress-text").innerHTML = '0%';
    g_is_file_being_uploaded = false;
}

/**
 * @brief marks the matching picture preparation as failed and restores an unchanged draft
 *
 * @param object result -> worker failure carrying the preparation operation ID
 *
 * @return void
 */
function chat__picture_preparation_failed(result)
{
    if (picture_preparation == null || picture_preparation.operation_id !== result.operation_id)
    {
        return;
    }
    const failed = picture_preparation;
    chat__release_file_upload_lock();
    const image = document.querySelector('[data-single-chat-message-local-message-id="' + failed.local_message_id + '"]');
    if (image)
    {
        image.classList.add("message-send-failed");
        image.title = "Picture not sent. Select it again to retry.";
    }
    // do not overwrite a newer draft or put this picture into a different conversation
    if (g_current_chat_context_id === failed.context_id && !g_base64_picture_string_to_send && !g_pending_chat_file && !document.getElementById("chat-input-container-text-input").value)
    {
        g_base64_picture_string_to_send = failed.picture;
        document.getElementById("chat-input-container-text-input").value = failed.text;
        const preview = document.getElementById("image-upload-preview");
        preview.style.backgroundImage = 'url("' + failed.picture + '")';
        preview.style.display = "block";
        if (typeof UI.sync_chat_input_state === "function")
        {
            UI.sync_chat_input_state();
        }
    }
    utils__custom_alert("The picture could not be encrypted and was not sent. Your picture is ready to retry.");
}

/**
 * @brief accepts only the current picture preparation and stops its timeout
 *
 * @param object result -> worker result carrying the preparation operation ID
 *
 * @return boolean -> true when the result matches the active preparation
 */
function chat__picture_preparation_succeeded(result)
{
    if (picture_preparation == null || picture_preparation.operation_id !== result.operation_id)
    {
        return false;
    }
    clearTimeout(picture_preparation.timer);
    picture_preparation = null;
    return true;
}

/**
 * @brief shows that an accepted picture is still being relayed to its recipients
 * clear this state on image_sent_status or after the timeout
 *
 * @return void
 */
function chat__show_picture_delivery_status()
{
    picture_delivery_pending = true;

    document.getElementById("upload-progress-overlay").style.display = "block";
    document.getElementById("upload-progress-bar").style.width = "100%";
    document.getElementById("upload-progress-text").innerHTML = "image received by server, being received by users ...";

    if (picture_delivery_hide_timer != null)
    {
        clearTimeout(picture_delivery_hide_timer);
    }

    picture_delivery_hide_timer = setTimeout(chat__hide_picture_delivery_status, 120000);
}

/**
 * @brief clears picture delivery status unless a newer upload owns it
 *
 * @return void
 */
function chat__hide_picture_delivery_status()
{
    if (picture_delivery_hide_timer != null)
    {
        clearTimeout(picture_delivery_hide_timer);
        picture_delivery_hide_timer = null;
    }

    if (picture_delivery_pending == false)
    {
        return;
    }

    picture_delivery_pending = false;

    // leave the overlay unchanged when a newer upload owns it
    if (g_is_file_being_uploaded == false)
    {
        document.getElementById("upload-progress-overlay").style.display = "none";
        document.getElementById("upload-progress-bar").style.width = "0%";
        document.getElementById("upload-progress-text").innerHTML = "0%";
    }
}

/**
 * @brief checks whether another file upload can begin
 *
 * @return boolean true when no upload is running
 */
function chat__can_start_file_upload()
{
    if (g_is_file_being_uploaded == true)
    {
        utils__custom_alert("cant upload more than 1 file at a time");
        return false;
    }

    return true;
}

/**
 * @brief sends file parts with socket backpressure and retains the lock until acknowledgement
 *
 * @param array parts -> the base64 parts
 * @param number total_bytes_length -> the whole file's length, sent with every part
 * @param number delay_ms -> the minimum pause between accepted parts
 *
 * @return void
 */
function chat__send_file_by_parts(parts, total_bytes_length, delay_ms)
{

    if (g_is_file_being_uploaded == true)
    {
        utils__custom_alert("cant upload more than 1 file at a time");
        return;
    }

    let i = 0;
    document.getElementById("upload-progress-overlay").style.display = "block";

    const generation = ++upload_generation;
    /**
     * @brief sends the next chunk only for the current upload and waits for final server acknowledgement
     *
     * @return Promise<void> -> resolves after handling this chunk or the final acknowledgement wait
     */
    async function next_part_upload()
    {
        // a file_send_error released the lock mid-upload: the server dropped this file, stop feeding it
        if (generation !== upload_generation || (i > 0 && g_is_file_being_uploaded == false))
        {
            return;
        }

        g_is_file_being_uploaded = true;

        if (i >= parts.length)
        {
            // keep the upload lock until acknowledgement. a timeout prevents a lost reply from blocking future uploads
            document.getElementById("upload-progress-text").innerHTML = "waiting for server ...";
            upload_ack_timer = setTimeout(chat__release_file_upload_lock, UPLOAD_ACK_TIMEOUT_MS);
            return;
        }

        const part = parts[i];

        // send

        let sent = false;
        try
        {
            sent = await client_msg.send_file_send_request(total_bytes_length, part, i);
        }
        catch (error)
        {
            console.error("upload chunk failed", error);
        }
        if (generation !== upload_generation)
        {
            return;
        }
        if (!sent)
        {
            chat__release_file_upload_lock();
            utils__custom_alert("Upload stopped because the connection could not accept more data. Please try again.");
            return;
        }

        i++;

        let percent = i / (parts.length / 100);
        document.getElementById("upload-progress-bar").style.width = percent + '%';
        document.getElementById("upload-progress-text").innerHTML = Math.round(percent) + '%';

        setTimeout(next_part_upload, delay_ms);
    }

    next_part_upload();
}

/**
 * @brief sends the attached file separately from any draft text
 *
 * @param string receiver_type -> "channel" or "user"
 *
 * @return boolean true when the file was handed off
 */
function chat__send_pending_chat_file(receiver_type)
{
    if (receiver_type === "channel" && !chat__can_send_to_selected_channel())
    {
        return false;
    }
    if (g_pending_chat_file == null)
    {
        return false;
    }

    if (g_is_file_being_uploaded == true)
    {
        utils__custom_alert("cant upload more than 1 file at a time");
        return false;
    }

    let chat_context_index = chat__get_chat_context_index_by_chat_context_id(g_current_chat_context_id);

    if (chat_context_index == -1)
    {
        utils__custom_log("[send-file] STOP: no chat context for " + g_current_chat_context_id);
        return false;
    }

    let receiver_public_key = "";

    if (receiver_type == "user")
    {
        let receiver = channel_tree__get_client_by_client_id(g_chat_message_receiver_id);

        if (receiver == null)
        {
            utils__custom_alert("files can only be sent to people who are online");
            return false;
        }

        if (receiver.is_idle == true)
        {
            utils__custom_alert(channel_tree__get_display_name_by_client_id(g_chat_message_receiver_id, receiver.username) + " is idle, files can not be delivered to idle clients");
            return false;
        }

        receiver_public_key = chat__private_recipient_key(g_current_chat_context_id, g_chat_message_receiver_id);

        if (receiver_public_key == "" || receiver_public_key == null)
        {
            utils__custom_alert("no public key for this user yet, try again in a moment");
            return false;
        }
    }
    else if (g_current_channel_keys == null)
    {
        utils__custom_alert("no channel keys from channel maintainer, cant send the file");
        return false;
    }

    let file = g_pending_chat_file;
    let key = "local-" + g_local_message_id;

    chat_files__clear_pending_chat_file();

    let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer').length;

    for (let i = 0; i < element_count; i++)
    {
        document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer')[0].remove();
    }

    let html_to_append = "<div class=\"single-chat-message\">\n\
                            <div class=\"single-message-content\">\n\
                                <div class=\"single-chat-message-sender-local-username-container\">\n\
                                    " + android_host__generate_message_sender_html(g_local_client_id, g_local_username) + "\n\
                                </div>\n\
                                <div class=\"single-chat-message-sender-time\">\n\
                                    <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                </div>\n\
                                <div class=\"single-chat-message-content\">\n\
                                    " + chat_files__generate_chat_file_card_html({ key: key, name: file.name, size: file.size, is_local: true, local_message_id: g_local_message_id }) + "\n\
                                </div>\n\
                            </div>\n\
                        </div>";

    document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
    document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
    chat__scroll_chat_to_end(true);

    g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;

    // our own copy is downloadable right away
    g_chat_files_by_message_id[key] = file;

    let card = chat_files__get_chat_file_card_by_key(key);

    if (card != null)
    {
        chat_files__wire_chat_file_download_button(card, key);
    }

    // lock before encryption starts so a second send cannot replace the pending upload
    g_is_file_being_uploaded = true;
    document.getElementById("upload-progress-overlay").style.display = "block";
    document.getElementById("upload-progress-text").innerHTML = "encrypting ...";

    if (receiver_type == "user")
    {
        chat__clear_seen_indicator_for_client(g_chat_message_receiver_id);

        g_data_processing_worker.postMessage({
            type: "mainthread__create_direct_chat_file",
            file: file,
            receiver_public_key: receiver_public_key,
            chat_message_receiver_id: g_chat_message_receiver_id,
            local_message_id: g_local_message_id
        });
    }
    else
    {
        g_data_processing_worker.postMessage({
            type: "mainthread__create_channel_chat_file",
            file: file,
            current_channel_keys: g_current_channel_keys,
            current_channel_id: g_current_channel_id,
            local_message_id: g_local_message_id
        });
    }

    g_local_message_id++;
    return true;
}

/**
 * @brief converts line breaks in outgoing text to spaces
 *
 * @param value text from the message input
 *
 * @return text ready for the local message and encrypted payload
 */
function chat__normalize_composed_text(value)
{
    return value.replace(/[ \t]*[\r\n\u2028\u2029]+[ \t]*/g, " ").trim();
}

/**
 * @brief sends the pending attachment or draft text to the selected conversation
 *
 * @param string target -> "channel" or "user"
 *
 * @return void
 */
function chat__send_composed_chat_message(target)
{
    if (target === "channel" && !chat__can_send_to_selected_channel())
    {
        return;
    }
    if (g_pending_chat_file != null)
    {
        chat__send_pending_chat_file(target);
        return;
    }

    if (target === "channel")
    {
        let action = g_base64_picture_string_to_send.length > 0 ? "picture" : "message";
        if (!groups__can_send_to_channel(action, g_current_channel_id))
        {
            groups__notify_channel_power_denied(action);
            return;
        }
    }

    let text_input = document.getElementById("chat-input-container-text-input");
    let chat_message_to_send_value = chat__normalize_composed_text(text_input.value);

    if (g_base64_picture_string_to_send.length == 0 && chat_message_to_send_value.trim().length == 0)
    {
        return;
    }

    let chat_context_index = chat__get_chat_context_index_by_chat_context_id(g_current_chat_context_id);
    if (chat_context_index == -1)
    {
        utils__custom_log("[send] no chat context for " + g_current_chat_context_id);
        return;
    }

    let receiver_public_key = "";
    if (target == "user")
    {
        receiver_public_key = chat__private_recipient_key(g_current_chat_context_id, g_chat_message_receiver_id);
        if (receiver_public_key == "" || receiver_public_key == null)
        {
            utils__custom_log("[send] no public key for receiver " + g_chat_message_receiver_id);
            return;
        }

        // a new message of ours is unread by definition, so the eye goes until they read it
        chat__clear_seen_indicator_for_client(g_chat_message_receiver_id);
    }

    let chat_context = document.getElementById(g_current_chat_context_id);
    text_input.value = "";
    let sender_html = android_host__generate_message_sender_html(g_local_client_id, g_local_username);
    let time_html = new Date().toLocaleTimeString();

    if (g_base64_picture_string_to_send.length > 0)
    {
        if (chat__can_start_file_upload() == false)
        {
            return;
        }

        let picture_html = "<img class='imgnotsentyet local-client-chat-picture-img chat-picture-img' data-single-chat-message-local-message-id='" + g_local_message_id + "' data-single-chat-message-server-message-id='unspecified' src=" + g_base64_picture_string_to_send + "></img>";
        chat__append_local_chat_message_block(chat_context, sender_html, time_html, picture_html);

        document.getElementById("image-upload-preview").style.backgroundImage = "";
        document.getElementById("image-upload-preview").style.display = "none";

        // lock before picture encryption starts so a second send cannot replace it
        g_is_file_being_uploaded = true;
        const operation_id = ++picture_operation_sequence;
        picture_preparation =
        {
            operation_id: operation_id,
            local_message_id: g_local_message_id,
            context_id: g_current_chat_context_id,
            picture: g_base64_picture_string_to_send,
            text: chat_message_to_send_value,
            timer: setTimeout(function()
            {
                chat__picture_preparation_failed({operation_id: operation_id});
            }, 120000)
        };
        document.getElementById("upload-progress-overlay").style.display = "block";
        document.getElementById("upload-progress-text").innerHTML = "processing image for send ...";

        if (target == "user")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_direct_chat_picture",
                operation_id: operation_id,
                base64_picture_string_to_send: g_base64_picture_string_to_send,
                receiver_public_key: receiver_public_key,
                chat_message_receiver_id: g_chat_message_receiver_id,
                local_message_id: g_local_message_id
            });
        }
        else
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_channel_chat_picture",
                operation_id: operation_id,
                base64_picture_string_to_send: g_base64_picture_string_to_send,
                current_channel_keys: g_current_channel_keys,
                current_channel_id: g_current_channel_id,
                local_message_id: g_local_message_id
            });
        }

        g_base64_picture_string_to_send = "";
    }
    else
    {
        // the server never sees the font or color, they travel inside the encrypted body
        let reaction_id = chat_reactions__new_id();
        let reply_to = chat_replies__take(chat_context);
        let actual_chat_message_to_send_value = JSON.stringify({
            reaction_id: reaction_id,
            reply_to: reply_to,
            font: g_selected_font,
            font_size: g_selected_font_size,
            font_color: g_selected_font_color,
            value: chat_message_to_send_value
        });

        let text_html = "<p class='single-chat-message-content-p local-single-chat-message-content-p " + g_selected_font + "' data-single-chat-message-local-message-id='" + g_local_message_id + "' data-single-chat-message-server-message-id='unspecified' style='color: " + g_selected_font_color + "; font-size: " + g_selected_font_size + "px;'>" + chat__sanitize_string(chat_message_to_send_value) + "</p>";

        chat__append_local_chat_message_block(chat_context, sender_html, time_html, text_html);

        chat_reactions__register(chat_context.querySelector('[data-single-chat-message-local-message-id="' + g_local_message_id + '"]'), reaction_id, g_rsa_public_key_string, g_local_username, reply_to);

        if (target == "user")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_direct_chat_message",
                chat_message_value: actual_chat_message_to_send_value,
                receiver_public_key: receiver_public_key,
                chat_message_receiver_id: g_chat_message_receiver_id,
                local_message_id: g_local_message_id
            });
        }
        else
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_channel_chat_message",
                chat_message_to_send_value: actual_chat_message_to_send_value,
                current_channel_keys: g_current_channel_keys,
                current_channel_id: g_current_channel_id,
                local_message_id: g_local_message_id
            });
        }
    }

    chat__scroll_chat_to_end(true);
    g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;
    g_local_message_id++;
}

/**
 * @brief installs delegated clicks for expanding public keys in private chat headers
 *
 * @return void
 */
function chat__setup_public_key_expand()
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    let chat = document.getElementById("chat-context-container");

    if (chat == null)
    {
        return;
    }

    chat.addEventListener("click", function(event)
    {
        let key = (event.target != null && typeof event.target.closest === "function") ? event.target.closest(".server-message-key") : null;

        if (key == null)
        {
            return;
        }

        key.classList.toggle("server-message-key-expanded");
        key.title = key.classList.contains("server-message-key-expanded") ? "click to collapse" : "click to expand";
    });
}

/**
 * @brief restores and wires the setting that hides public key details in private conversations
 *
 * @return void
 */
function chat__setup_public_key_lines_setting()
{
    let checkbox = null;

    // only with a page
    if (G_HAS_DOM == false)
    {
        return;
    }
    checkbox = document.getElementById("local-settings-hide-public-keys");

    // restore saved value
    checkbox.checked = utils__storage_get("lemon_is_public_key_info_hidden", "1") === "1";
    document.body.classList.toggle("public-key-info-hidden", checkbox.checked);

    // save on change
    checkbox.onchange = function()
    {
        utils__storage_set("lemon_is_public_key_info_hidden", checkbox.checked ? "1" : "0");
        document.body.classList.toggle("public-key-info-hidden", checkbox.checked);
    };
}

/**
 * @brief scrolls to the newest message when forced or enabled by the local setting
 *
 * @param boolean is_forced -> true ignores the setting
 *
 * @return void
 */
function chat__scroll_chat_to_end(is_forced)
{
    if (typeof chat_history__is_search_open === "function" && chat_history__is_search_open())
    {
        return;
    }
    if (is_forced != true && g_auto_scroll_chat_to_end == false)
    {
        return;
    }

    let container = document.getElementById("chat-context-container");

    if (container == null)
    {
        return;
    }

    if (is_forced === true)
    {
        container.scrollTop = container.scrollHeight;
        return;
    }
    if (chat_scroll_pending)
    {
        return;
    }
    // scroll once for messages arriving together. reading the height after
    // every message makes the browser lay out the whole chat repeatedly
    chat_scroll_pending = true;
    (typeof requestAnimationFrame === "function" ? requestAnimationFrame : setTimeout)(function()
    {
        chat_scroll_pending = false;
        if (container.isConnected && g_auto_scroll_chat_to_end && !(typeof chat_history__is_search_open === "function" && chat_history__is_search_open()))
        {
            container.scrollTop = container.scrollHeight;
        }
    });
}

/**
 * @brief appends a local message while keeping the bottom spacer last
 *
 * @param Element chat_context -> the context element
 * @param string sender_html -> the sender header
 * @param string time_html -> the time text
 * @param string content_html -> the message body
 *
 * @return void
 */
function chat__append_local_chat_message_block(chat_context, sender_html, time_html, content_html)
{
    let spacers = chat_context.getElementsByClassName("chat-spacer");
    while (spacers.length > 0)
    {
        spacers[0].remove();
    }

    chat_context.insertAdjacentHTML("beforeend", "<div class=\"single-chat-message\">\n\
            <div class=\"single-message-content\">\n\
                <div class=\"single-chat-message-sender-local-username-container\">\n\
                    " + sender_html + "\n\
                </div>\n\
                <div class=\"single-chat-message-sender-time\">\n\
                    <p>" + time_html + "</p>\n\
                </div>\n\
                <div class=\"single-chat-message-content\">\n\
                    " + content_html + "\n\
                </div>\n\
            </div>\n\
        </div>");
    chat_context.insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
}

/**
 * @brief sends the input to the current channel
 *
 * @return void
 */
function chat__send_channel_chat_message()
{
    chat__send_composed_chat_message("channel");
}

/**
 * @brief sends the input (or the pending picture) to the selected client: the local echo is rendered, then the worker RSA+AES encrypts it for the receiver's public key
 *
 * @return void
 */
function chat__send_direct_chat_message()
{
    let receiver = channel_tree__get_client_by_client_id(g_chat_message_receiver_id);
    if (receiver != null && receiver.is_idle && !groups__is_allowed(channel_tree__get_client_by_client_id(g_local_client_id), "message_idle_clients"))
    {
        utils__custom_alert("insufficient permissions");
        return;
    }
    chat__send_composed_chat_message("user");
}

/**
 * @brief encrypts a message for a saved identity and asks the server to hold it until that user returns
 *
 * @param offline_contact saved recipient and their public key
 */
function chat__send_offline_chat_message(offline_contact)
{
    if (!groups__is_allowed(channel_tree__get_client_by_client_id(g_local_client_id), "send_offline_messages"))
    {
        utils__custom_alert("insufficient permissions");
        return;
    }
    let chat_message_to_send_value = chat__normalize_composed_text(document.getElementById('chat-input-container-text-input').value);
    document.getElementById('chat-input-container-text-input').value = "";

    if (chat_message_to_send_value.trim().length == 0)
    {
        return;
    }

    let context_id = "chat-context-offline-" + offline_contact.identity_hash;

    let actual_data_to_send = {
        reaction_id: chat_reactions__new_id(),
        reply_to: chat_replies__take(document.getElementById(context_id)),
        font: g_selected_font,
        font_size: g_selected_font_size,
        font_color: g_selected_font_color,
        value: chat_message_to_send_value
    };

    if (document.getElementById(context_id) != null)
    {
        let html_to_append = "<div class=\"single-chat-message\">\n\
                                <div class=\"single-message-content\">\n\
                                    <div class=\"single-chat-message-sender-local-username-container\">\n\
                                        " + android_host__generate_message_sender_html(g_local_client_id, g_local_username) + "\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <p class='single-chat-message-content-p " + g_selected_font + "' style='color: " + g_selected_font_color + "; font-size: " + g_selected_font_size + "px;'>" + chat__sanitize_string(chat_message_to_send_value) + "</p>\n\
                                    </div>\n\
                                </div>\n\
                            </div>";

        document.getElementById(context_id).insertAdjacentHTML("beforeend", html_to_append);
        if (G_HAS_DOM)
        {
            let element = document.getElementById(context_id).querySelector(".single-chat-message:last-child .single-chat-message-content-p");
            chat_reactions__register(element, actual_data_to_send.reaction_id, g_rsa_public_key_string, g_local_username, actual_data_to_send.reply_to);
        }
        chat__scroll_chat_to_end(true);
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__create_offline_chat_message",
        chat_message_value: JSON.stringify(actual_data_to_send),
        receiver_public_key: offline_contact.public_key,
        recipient_identity_hash: offline_contact.identity_hash
    });
}

/**
 * @brief prevents a channel send while another conversation is selected, before consuming the draft
 *
 * @return boolean true when the selected conversation belongs to the joined channel
 */
function chat__can_send_to_selected_channel()
{
    if (g_current_chat_context_id === "chat-context-channel-" + g_current_channel_id)
    {
        return true;
    }
    utils__custom_log("[send] STOP: selected conversation is not the joined channel");
    utils__custom_alert(CHANNEL_CONTEXT_SEND_WARNING, "channel-context");
    return false;
}

// a channel message that waits for the channel key: the chat context it was written in and the timer that
// ends the wait, or null while no message waits
var chat_channel_keys_wait = null;
var CHAT_CHANNEL_KEYS_WAIT_MS = 15000;

/**
 * @brief Keeps a channel message in the message field until the channel key arrives. After a login or a
 *        channel change the key arrives from the channel maintainer a moment later; a message sent in that
 *        time is sent by chat__send_message_waiting_for_channel_keys when the key is stored.
 *
 * @return boolean -> true while the message waits, false when it has waited CHAT_CHANNEL_KEYS_WAIT_MS without a key
 */
function chat__wait_for_channel_keys()
{
    // waited too long
    if (chat_channel_keys_wait === false)
    {
        chat_channel_keys_wait = null;
        return false;
    }

    // already waiting
    if (chat_channel_keys_wait != null)
    {
        return true;
    }

    // start the wait
    chat_channel_keys_wait = {
        context_id: g_current_chat_context_id,
        timer: setTimeout(function()
        {
            chat_channel_keys_wait = false;
            if (g_current_channel_keys == null)
            {
                chat__send_chat_message();
            }
            chat_channel_keys_wait = null;
        }, CHAT_CHANNEL_KEYS_WAIT_MS)
    };
    utils__custom_alert("waiting for the channel key, the message is sent when it arrives", "channel-key-wait");
    return true;
}

/**
 * @brief Sends the channel message that waits for the channel key, if the user is still in the chat context it
 *        was written in. Called when a channel key is stored.
 *
 * @return void
 */
function chat__send_message_waiting_for_channel_keys()
{
    let wait = chat_channel_keys_wait;

    // no message waits
    if (wait == null || wait === false || g_current_channel_keys == null)
    {
        return;
    }

    // end the wait
    clearTimeout(wait.timer);
    chat_channel_keys_wait = null;

    // send the message
    if (wait.context_id === g_current_chat_context_id)
    {
        utils__hide_custom_alert("", "channel-key-wait");
        chat__send_chat_message();
    }
}

/**
 * @brief routes Send to channel, direct or offline sending, with fallbacks when a direct recipient changes status
 *
 * @return void
 */
function chat__send_chat_message()
{
    if (!g_is_authenticated)
    {
        utils__custom_alert("Connect to the server before sending. Your draft has been kept.");
        return;
    }
    // log each exit so refused sends can be traced
    utils__custom_log("[send] type=" + g_chat_message_receiver_type + " receiver_id=" + g_chat_message_receiver_id + " (" + (typeof g_chat_message_receiver_id) + ")" + " context=" + g_current_chat_context_id);

    if (g_chat_message_receiver_type == "channel")
    {
        if (!chat__can_send_to_selected_channel())
        {
            return;
        }
        if (g_current_channel_keys == null)
        {
            utils__custom_log("[send] STOP: no channel keys");

            // wait for key
            if (chat__wait_for_channel_keys())
            {
                return;
            }
            utils__custom_alert("no channel keys from channel maintainer, cant send the message");
            return;
        }
        chat__send_channel_chat_message();
    }
    else if (g_chat_message_receiver_type == "user")
    {
        if (channel_tree__get_client_by_client_id(g_chat_message_receiver_id) == null)
        {
            // nobody is there to decrypt a file, and the server keeps nothing this big
            if (g_pending_chat_file != null)
            {
                utils__custom_log("[send] STOP: file attached but the receiver is offline");
                utils__custom_alert("files can only be sent to people who are online");
                return;
            }

            utils__custom_log("[send] receiver not in client list, trying the offline paths" + " (identity='" + g_offline_chat_recipient_identity_hash + "')");
            // they may have come online since this context was opened: then send it live instead
            // of queueing (the server refuses to queue for a connected client)
            let live_client = channel_tree__get_client_by_identity_hash(g_offline_chat_recipient_identity_hash);

            if (live_client != null)
            {
                // update conversation state before rendering because Node has no UI implementation
                let promotion = chat__promote_offline_chat_context_state(live_client);
                UI.promote_offline_chat_context_render(promotion, live_client);

                utils__custom_log("[send] promoted offline context to live, sending direct");
                chat__send_direct_chat_message();
                return;
            }

            let offline_contact = channel_tree__get_stored_client_by_identity_hash(g_offline_chat_recipient_identity_hash);

            if (offline_contact != null && typeof offline_contact.public_key === "string" && offline_contact.public_key.length > 0)
            {
                utils__custom_log("[send] sending as OFFLINE message");
                chat__send_offline_chat_message(offline_contact);
                return;
            }

            if (offline_contact != null)
            {
                // an offline send needs the recipient public key before encryption can begin
                utils__custom_log("[send] STOP: known contact but no public key");
                utils__custom_alert("this server does not keep messages for people who are offline");
                return;
            }

            utils__custom_log("[send] STOP: receiver unknown and no offline contact");
            utils__custom_alert("this user is offline, the message can not be delivered");
            return;
        }

        utils__custom_log("[send] receiver found, going direct");
        chat__send_direct_chat_message();
    }
}

/**
 * @brief reads edited text without the final line break used to position the caret
 *
 * @param element message being edited
 *
 * @return text with the line breaks entered by the user
 */
function chat__get_edited_text(element)
{
    let text = element.innerText.replace(/\r\n?/g, "\n");
    let last = element.lastChild;
    while (last && last.lastChild)
    {
        last = last.lastChild;
    }
    if (last && (last.nodeName === "BR" || (last.nodeType === Node.TEXT_NODE && last.nodeValue === "\n")) && text.endsWith("\n"))
    {
        text = text.slice(0, -1);
    }
    return text;
}

/**
 * @brief saves or cancels an edit in its original conversation
 *
 * @param element message being edited
 * @param save whether to send changed text
 */
function chat__finish_message_edit(element, save)
{
    let edit = chat_reactions.edits.get(element);
    if (!edit)
    {
        return;
    }
    let text = chat__get_edited_text(element);
    element.removeEventListener("blur", chat__local_chat_message_onblur);
    element.removeEventListener("keydown", chat__local_chat_message_onkeydown);
    chat_reactions.edits.delete(element);
    element.removeAttribute("contenteditable");
    if (!save || !chat_reactions__can_modify(edit.state) || !text.trim() || text === edit.original_text)
    {
        element.innerHTML = edit.original;
        return;
    }
    let payload = JSON.stringify(
    {
        type: "chat_message_edit",
        message_id: edit.state.id,
        author_public_key: edit.state.author,
        new_message_value: text
    });
    let request = null;
    if (edit.receiver_type === "user")
    {
        request =
        {
            type: "mainthread__create_direct_chat_message",
            receiver_public_key: edit.state.peer_key,
            chat_message_receiver_id: edit.receiver_id,
            chat_message_value: payload
        };
    }
    else
    {
        if (!keys__are_message_keys_valid(g_current_channel_keys))
        {
            element.innerHTML = edit.original;
            utils__custom_alert("Wait for this channel's encryption keys before editing a message.");
            return;
        }
        request =
        {
            type: "mainthread__create_channel_chat_message",
            current_channel_id: edit.receiver_id,
            current_channel_keys: g_current_channel_keys,
            chat_message_to_send_value: payload
        };
    }
    request.local_message_id = g_local_message_id++;
    // the worker applies the same content encryption as a new message before transport encryption
    g_data_processing_worker.postMessage(request);
    element.textContent = text;
    element.classList.add("chat-message-edited");
}

/**
 * @brief applies an edit only after decrypting a chat message from the original author
 *
 * @param object payload -> decrypted edit contents, including the original message's random identifier
 * @param object message -> sender, channel and sequence supplied by the server
 * @param boolean direct -> whether this arrived through the private message route
 * @param object|null retained_sender -> trusted sender snapshot for retained local delivery, or null for a live sender
 *
 * @return boolean true when the payload is an edit, including an invalid edit that must be discarded
 */
function chat__receive_message_edit(payload, message, direct, retained_sender)
{
    if (!payload || payload.type !== "chat_message_edit")
    {
        return false;
    }
    if (!G_HAS_DOM || message.sender_id === g_local_client_id || typeof payload.message_id !== "string" || !/^[a-f0-9]{32}$/.test(payload.message_id) || typeof payload.author_public_key !== "string" || typeof payload.new_message_value !== "string" || !payload.new_message_value.trim() || !Number.isSafeInteger(message.server_chat_message_id) || message.server_chat_message_id < 0)
    {
        return true;
    }
    let sender = retained_sender || channel_tree__get_client_by_client_id(message.sender_id);
    let actor = retained_sender ? retained_sender.public_key : channel_tree__get_public_key_by_client_id(message.sender_id);
    if (!sender || sender.is_ignored_by_local_client || !actor || actor !== payload.author_public_key)
    {
        return true;
    }
    let context = document.getElementById(direct ? (retained_sender ? retained_sender.context_id : "chat-context-pm-" + message.sender_id) : "chat-context-channel-" + message.channel_id);
    let element = chat_replies__find_original(context, payload);
    let state = element && chat_reactions.entries.get(element);
    if (!state || state.author !== actor || state.generation !== g_worker_connection_generation || (direct && state.peer_key !== actor) || (state.last_edit_sequence != null && state.last_edit_sequence >= message.server_chat_message_id))
    {
        return true;
    }
    state.last_edit_sequence = message.server_chat_message_id;
    element.textContent = payload.new_message_value;
    element.classList.add("chat-message-edited");
    return true;
}

/**
 * @brief saves an edit when focus moves elsewhere
 *
 * @param event focus leaving the edited message
 */
function chat__local_chat_message_onblur(event)
{
    chat__finish_message_edit(event.currentTarget, true);
}

/**
 * @brief saves with Enter, keeps Shift and Enter for new lines, and cancels with Escape
 *
 * @param event key pressed while editing a message
 */
function chat__local_chat_message_onkeydown(event)
{
    if (event.isComposing || event.keyCode === 229)
    {
        return;
    }
    if ((event.key === "Enter" && !event.shiftKey) || event.key === "Escape")
    {
        event.preventDefault();
        event.stopPropagation();
        chat__finish_message_edit(event.currentTarget, event.key === "Enter");
        event.currentTarget.blur();
    }
}

/**
 * @brief escapes HTML text and quoted attribute values
 *
 * @param string str -> the text, anything else is coerced
 *
 * @return string the escaped text, "" for null or undefined
 */
function chat__sanitize_string(str)
{
    if (str === null || str === undefined)
    {
        return "";
    }
    str = "" + str; // coerce other values before calling replace
    var map = {
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#039;'
    };
    var new_string = str.replace(/[&<>"']/g, function (m)
    {
        return map[m];
    });
    return new_string;
}

/**
 * @brief allows supported base64 image URLs and replaces other sources with a placeholder
 *
 * @param string str -> the candidate src
 *
 * @return string the src itself, else a 1x1 transparent png data url, "" for null or undefined
 */
function chat__sanitize_image_data_url(str)
{
    if (str === null || str === undefined)
    {
        return "";
    }
    str = "" + str;

    // data:image/<subtype>;base64,<base64 payload>
    if (/^data:image\/[a-zA-Z0-9.+-]+;base64,[A-Za-z0-9+/=\s]+$/.test(str) === true)
    {
        return str;
    }

    // use a transparent pixel when the source is not a valid image data URL
    return "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M8AAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
}

/**
 * @brief removes the prefix before a data URL's base64 contents
 *
 * @param string base64string -> the data url
 *
 * @return string the bare base64, the input unchanged when there is no comma
 */
function chat__remove_data_url_prefix_from_base64_string(base64string)
{
    const commaindex = base64string.indexOf(',');
    if (commaindex === -1)
    {
        return base64string;
    }
    else
    {
        return base64string.slice(commaindex + 1);
    }
}

/**
 * @brief splits an upload into pieces of at most 16 KiB, so small files need fewer messages
 *
 * @param string string_to_split -> the text
 *
 * @return array the slices
 */
function chat__split_upload_into_chunks(string_to_split)
{
    const result = [];
    const partsize = 16384;

    let start = 0;

    while (start < string_to_split.length)
    {
        result.push(string_to_split.slice(start, start + partsize));
        start += partsize;
    }

    return result;
}

/**
 * @brief the file extension without the dot
 *
 * @param string filename -> the file name
 *
 * @return string the extension, "" when the name has none
 */
function chat__get_file_extension(filename)
{
    var ext = /^.+\.([^.]+)$/.exec(filename);
    return ext == null ? "" : ext[1];
}

// typing indicators

/**
 * @brief sends a rate limited typing notice for the current conversation
 *
 * @return void
 */
function chat__send_typing_indicator()
{
    if (g_server_policy.is_typing_indicator_allowed == false || g_is_authenticated == false)
    {
        return;
    }

    // an offline conversation has nobody to tell
    if (g_offline_chat_recipient_identity_hash != null && g_offline_chat_recipient_identity_hash.length > 0)
    {
        return;
    }

    let now = new Date().valueOf();
    if (now - g_typing.last_sent_at < TYPING_SEND_INTERVAL_MS)
    {
        return;
    }
    g_typing.last_sent_at = now;

    let receiver_id = (g_chat_message_receiver_type == "channel") ? g_current_channel_id : parseInt(g_chat_message_receiver_id);

    if (isNaN(receiver_id))
    {
        return;
    }

    connection__send_message_object({
        message: {
            type: "typing_indicator_request",
            receiver_type: g_chat_message_receiver_type,
            receiver_id: receiver_id
        }
    });
}

/**
 * @brief finds the conversation context for a typing notice
 *
 * @param string receiver_type -> "channel" or "user"
 * @param number receiver_id -> the channel, or us
 * @param number sender_client_id -> who is typing
 *
 * @return string the context id
 */
function chat__get_chat_context_id_for_typing(receiver_type, receiver_id, sender_client_id)
{
    if (receiver_type == "channel")
    {
        return "chat-context-channel-" + receiver_id;
    }

    // he is writing to US, so the conversation is the one with HIM
    return "chat-context-pm-" + sender_client_id;
}

/**
 * @brief remembers that somebody is typing in a conversation and (re)starts the ticker that repaints and expires the notice
 *
 * @param number sender_client_id -> who is typing
 * @param string receiver_type -> "channel" or "user"
 * @param number receiver_id -> the channel, or us
 *
 * @return void
 */
function chat__note_typing_from_client(sender_client_id, receiver_type, receiver_id)
{
    let context_id = chat__get_chat_context_id_for_typing(receiver_type, receiver_id, sender_client_id);

    if (g_typing.state[context_id] == null)
    {
        g_typing.state[context_id] = {};
    }

    g_typing.state[context_id][sender_client_id] = new Date().valueOf() + TYPING_EXPIRY_MS;

    chat__render_typing_indicator();

    if (g_typing.render_timer == null)
    {
        g_typing.render_timer = window.setInterval(chat__render_typing_indicator, 1000);
    }
}

/**
 * @brief somebody's message arrived (or he left): whatever he was typing is over
 *
 * @param number sender_client_id -> the client
 *
 * @return void
 */
function chat__clear_typing_from_client(sender_client_id)
{
    for (let context_id in g_typing.state)
    {
        if (g_typing.state[context_id] != null)
        {
            delete g_typing.state[context_id][sender_client_id];
        }
    }

    chat__render_typing_indicator();
}

/**
 * @brief builds a typing indicator with animated dots
 *
 * @param string text_without_dots -> the "x is typing" text, set as textContent since a username is user-supplied
 *
 * @return Element the line
 */
function chat__build_typing_line(text_without_dots)
{
    let line = document.createElement("div");
    line.className = "typing-indicator-line";

    let label = document.createElement("span");
    // use textContent because usernames may contain markup
    label.textContent = text_without_dots;
    line.appendChild(label);

    let dots = document.createElement("span");
    dots.className = "typing-indicator-dots";

    for (let i = 0; i < 3; i++)
    {
        let dot = document.createElement("span");
        dot.textContent = ".";
        dots.appendChild(dot);
    }

    line.appendChild(dots);
    return line;
}

/**
 * @brief renders current typing notices and expires stale entries
 *
 * @return void
 */
function chat__render_typing_indicator()
{
    let element = document.getElementById("typing-indicator-container");
    if (element == null)
    {
        return;
    }

    let now = new Date().valueOf();
    let names = [];
    let anything_left = false;

    for (let context_id in g_typing.state)
    {
        let typers = g_typing.state[context_id];
        if (typers == null)
        {
            continue;
        }

        for (let client_id in typers)
        {
            if (typers[client_id] <= now)
            {
                delete typers[client_id];
                continue;
            }

            anything_left = true;

            if (context_id == g_current_chat_context_id)
            {
                names.push(channel_tree__get_display_name_by_client_id(parseInt(client_id), ""));
            }
        }
    }

    // show at most three names and count the remaining typists on a final line
    element.textContent = "";

    if (names.length == 0)
    {
        element.style.display = "none";
    }
    else
    {
        let lines_to_show = (names.length > 3) ? 3 : names.length;

        for (let i = 0; i < lines_to_show; i++)
        {
            element.appendChild(chat__build_typing_line(names[i] + " is typing"));
        }

        if (names.length > lines_to_show)
        {
            element.appendChild(chat__build_typing_line("and " + (names.length - lines_to_show) + " others are typing"));
        }

        // use an explicit display value because the stylesheet hides this element by default
        element.style.display = "block";
    }

    if (anything_left == false && g_typing.render_timer != null)
    {
        window.clearInterval(g_typing.render_timer);
        g_typing.render_timer = null;
    }
}

// unread counts and seen receipts

/**
 * @brief shows or hides the seen eye of the conversation on screen, from seen_state_by_context and the local setting
 *
 * @return void
 */
function chat__render_seen_indicator()
{
    let indicator = document.getElementById("chat-seen-indicator");

    if (indicator == null)
    {
        return;
    }

    let is_seen = (g_current_chat_context_id != null) && (seen_state_by_context[g_current_chat_context_id] == true) && (g_show_seen_indicator == true);

    indicator.classList.toggle("chat-seen-visible", is_seen);
}

/**
 * @brief we just wrote to this person, so our newest message is unread again
 *
 * @param number client_id -> the person
 *
 * @return void
 */
function chat__clear_seen_indicator_for_client(client_id)
{
    seen_state_by_context["chat-context-pm-" + client_id] = false;
    chat__render_seen_indicator();
}

/**
 * @brief the other side read our latest private message: the eye goes on for that conversation
 *
 * @param number sender_client_id -> who read it
 *
 * @return void
 */
function chat__mark_message_as_seen(sender_client_id)
{
    seen_state_by_context["chat-context-pm-" + sender_client_id] = true;
    chat__render_seen_indicator();
}

/**
 * @brief queues a private message receipt under its sender's identity
 *
 * @param number sender_client_id -> the author
 * @param number server_chat_message_id -> the message
 *
 * @return void
 */
function chat__remember_message_awaiting_receipt(sender_client_id, server_chat_message_id)
{
    if (android_host__is_ui_only_runtime() || server_chat_message_id == null || sender_client_id == g_local_client_id)
    {
        return;
    }
    const peer_key = chat__peer_state_key(channel_tree__get_client_by_client_id(sender_client_id));
    if (!peer_key)
    {
        return;
    }
    if (unreceipted_messages_by_sender[peer_key] == null)
    {
        unreceipted_messages_by_sender[peer_key] = [];
    }
    if (!unreceipted_messages_by_sender[peer_key].includes(server_chat_message_id))
    {
        unreceipted_messages_by_sender[peer_key].push(server_chat_message_id);
    }
}

/**
 * @brief sends the receipts owed to one sender, once, when their conversation is opened
 *
 * @param number sender_client_id -> the author
 * @param boolean authorized_read -> whether the current page authorized this Node read
 * @param boolean send_receipts -> whether the page allows seen receipts
 *
 * @return void
 */
function chat__send_pending_seen_receipts(sender_client_id, authorized_read = false, send_receipts = g_send_seen_receipts)
{
    // Node requires an authorized page read. browsers check visibility directly
    if (android_host__is_ui_only_runtime() || (typeof process !== "undefined" ? !authorized_read || !g_node_has_attached_ui : !chat__is_the_user_actually_looking()))
    {
        return;
    }
    // find receipts by identity so a reused client slot cannot inherit them
    const peer_key = chat__peer_state_key(channel_tree__get_client_by_client_id(sender_client_id));
    let pending = unreceipted_messages_by_sender[peer_key];

    if (pending == null)
    {
        return;
    }

    // consume the read even when the local receipt preference is disabled
    delete unreceipted_messages_by_sender[peer_key];

    for (let x = 0; x < pending.length; x++)
    {
        chat__send_seen_receipt_for_message(sender_client_id, pending[x], authorized_read, send_receipts);
    }
}

/**
 * @brief sends one receipt after checking read authorization and the local preference
 *
 * @param number sender_client_id -> the author
 * @param number server_chat_message_id -> the message
 * @param boolean authorized_read -> whether the current page authorized this Node read
 * @param boolean send_receipts -> whether the page allows seen receipts
 *
 * @return void
 */
function chat__send_seen_receipt_for_message(sender_client_id, server_chat_message_id, authorized_read = false, send_receipts = g_send_seen_receipts)
{
    if (android_host__is_ui_only_runtime())
    {
        return;
    }
    if (typeof process !== "undefined" ? !authorized_read || !g_node_has_attached_ui : !chat__is_the_user_actually_looking())
    {
        return;
    }

    if (send_receipts !== true)
    {
        return;
    }

    if (server_chat_message_id == null || sender_client_id == g_local_client_id)
    {
        return;
    }

    if (seen_receipts_already_sent[server_chat_message_id] == true)
    {
        return;
    }

    let public_key = channel_tree__get_public_key_by_client_id(sender_client_id);

    if (public_key == null || public_key == "")
    {
        return;
    }

    seen_receipts_already_sent[server_chat_message_id] = true;

    g_data_processing_worker.postMessage({
        type: "mainthread__create_seen_receipt_message",
        receiver_id: sender_client_id,
        public_key: public_key,
        server_chat_message_id: server_chat_message_id
    });
}

/**
 * @brief sends an encrypted conversation closure notice to an eligible live peer
 *
 * @param number client_id -> the client on the other side of the conversation
 *
 * @return void
 */
function chat__send_direct_chat_closed_notice(client_id)
{
    let client = channel_tree__get_client_by_client_id(client_id);

    // no receiver available
    if (client == null || client.is_music_bot == true || !client.public_key || g_is_authenticated != true)
    {
        return;
    }

    // sending not allowed
    if (g_server_policy.is_private_messaging_allowed == false || (typeof groups__has_permission === "function" && !groups__has_permission("send_direct_messages")))
    {
        return;
    }

    // build and send
    g_data_processing_worker.postMessage({
        type: "mainthread__create_direct_chat_closed_message",
        receiver_id: client_id,
        public_key: client.public_key
    });
}

/**
 * @brief adds a closure notice to an existing private conversation
 *
 * @param number sender_client_id -> the client that closed its window
 * @param string sender_username -> its name as sent by the server, used when the client list has no name
 *
 * @return void
 */
function chat__show_direct_chat_closed(sender_client_id, sender_username)
{
    let context = document.getElementById("chat-context-pm-" + sender_client_id);
    let client = channel_tree__get_client_by_client_id(sender_client_id);
    let line = null;

    // conversation not open
    if (context == null || (client != null && client.is_ignored_by_local_client == true))
    {
        return;
    }

    // add the line
    line = document.createElement("div");
    line.className = "single-server-message direct-chat-closed-message";
    line.textContent = channel_tree__get_display_name_by_client_id(sender_client_id, sender_username) + " closed the conversation. " + new Date().toLocaleTimeString();
    context.appendChild(line);

    // scroll to end
    if (g_current_chat_context_id == context.id)
    {
        chat__scroll_chat_to_end(false);
    }
}

// process receipts for the visible conversation when the user returns to the app
if (typeof document !== "undefined" && typeof document.addEventListener === "function")
{
    document.addEventListener("visibilitychange", function()
    {
        if (document.hidden === true || g_current_chat_context_id == null)
        {
            return;
        }

        if (g_current_chat_context_id.indexOf("chat-context-pm-") != 0)
        {
            return;
        }

        chat__send_pending_seen_receipts(parseInt(g_current_chat_context_id.split("chat-context-pm-")[1]));
    });
}

/**
 * @brief checks whether the user can see the current page
 * Node always returns false because it has no visible conversation
 *
 * @return boolean true when the user can see the page
 */
function chat__is_the_user_actually_looking()
{
    if (typeof process !== "undefined")
    {
        return false;
    }

    if (typeof document !== "undefined" && document.hidden === true)
    {
        return false;
    }

    return true;
}

/**
 * @brief the unread count of a channel
 *
 * @param number channel_id -> the channel
 *
 * @return number the count, 0 when nothing is stored
 */
function chat__get_channel_unread_count(channel_id)
{
    let stored = g_channel_unread_counts[channel_id];
    return (typeof stored === "number") ? stored : 0;
}

/**
 * @brief counts an unread channel message and updates its badge
 *
 * @param number channel_id -> the channel the message arrived in
 *
 * @return void
 */
function chat__increment_channel_unread_count(channel_id)
{
    if (android_host__is_ui_only_runtime())
    {
        return;
    }
    // only a visible conversation counts as read. headless Node keeps counting unread messages
    if (android_host__is_someone_watching_the_ui() == true && g_current_chat_context_id === ("chat-context-channel-" + channel_id))
    {
        return;
    }

    g_channel_unread_counts[channel_id] = chat__get_channel_unread_count(channel_id) + 1;
    chat__render_channel_unread_badge(channel_id);
    chat__update_app_unread_badge();
}

/**
 * @brief zeroes a channel's unread count and repaints its badge
 *
 * @param number channel_id -> the channel
 *
 * @return void
 */
function chat__clear_channel_unread_count(channel_id)
{
    if (android_host__is_ui_only_runtime())
    {
        chat__send_read_command("chat-context-channel-" + channel_id, {channel_id: channel_id});
        return;
    }
    g_channel_unread_counts[channel_id] = 0;
    chat__render_channel_unread_badge(channel_id);
    chat__update_app_unread_badge();
}

/**
 * @brief updates a channel unread badge and mirrors it into the member list
 *
 * @param number channel_id -> the channel
 *
 * @return void
 */
function chat__render_channel_unread_badge(channel_id)
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let badge = document.querySelector('[data-channel-unread-for="' + channel_id + '"]');

    if (badge == null)
    {
        return;
    }

    let count = chat__get_channel_unread_count(channel_id);
    badge.innerHTML = count;

    // use a class so the theme retains control of badge visibility
    if (count > 0)
    {
        badge.classList.remove("unread-empty");
    }
    else
    {
        badge.classList.add("unread-empty");
    }

    // rebuild the member list badge for themes that hide the channel tree
    if (typeof UI !== "undefined" && typeof UI.schedule_member_list_sync === "function")
    {
        UI.schedule_member_list_sync();
    }
}

/**
 * @brief publishes the total unread count for channel and private conversations
 *
 * @return void
 */
function chat__update_app_unread_badge()
{
    let total = 0;

    for (let channel_id in g_channel_unread_counts)
    {
        total += chat__get_channel_unread_count(channel_id);
    }

    const counted = new Set();
    for (const client of g_client_list)
    {
        const key = chat__peer_state_key(client);
        counted.add(key);
        total += client.unread_count || 0;
    }
    for (const [key, state] of chat_peer_local_state)
    {
        if (!counted.has(key))
        {
            total += state.unread_count || 0;
        }
    }

    // WebView reports counts through Android. Node reports them through its host while the page is absent
    if (typeof Android !== "undefined" && typeof Android.JavaExportSetUnreadBadge === "function" && g_loopback_port <= 0)
    {
        Android.JavaExportSetUnreadBadge(total);
    }
    else if (g_node_unread_listener != null)
    {
        g_node_unread_listener(total);
    }
}

/**
 * @brief the unread count of a private conversation
 *
 * @param number client_id -> the other person
 *
 * @return number the count, 0 when the client is unknown
 */
function chat__get_unread_count(client_id)
{
    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object == null || typeof client_object.unread_count !== "number")
    {
        return 0;
    }

    return client_object.unread_count;
}

/**
 * @brief counts one more unread private message from a client
 *
 * @param number client_id -> the author
 *
 * @return void
 */
function chat__increment_unread_count(client_id)
{
    if (android_host__is_ui_only_runtime())
    {
        return;
    }
    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null)
    {
        client_object.unread_count = chat__get_unread_count(client_id) + 1;
        chat_peer_local_state.set(chat__peer_state_key(client_object),
        {
            unread_count: client_object.unread_count,
            is_ignored_by_local_client: client_object.is_ignored_by_local_client === true,
            is_muted_by_local_client: client_object.is_muted_by_local_client === true
        });
        chat__update_app_unread_badge();
    }
}

/**
 * @brief marks a private conversation read through the connection owner
 *
 * @param number client_id -> the other person
 * @param boolean authorized_read -> whether the current page authorized this Node read
 * @param boolean send_receipts -> whether the page allows seen receipts
 *
 * @return void
 */
function chat__clear_unread_count(client_id, authorized_read = false, send_receipts = g_send_seen_receipts)
{
    // opening it is reading it, so anything that arrived while it was closed is
    // receipted now
    chat__send_pending_seen_receipts(client_id, authorized_read, send_receipts);
    chat__render_seen_indicator();

    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null)
    {
        if (android_host__is_ui_only_runtime())
        {
            chat__send_read_command(chat__peer_state_key(client_object),
            {
                identity_hash: client_object.identity_hash,
                public_key: client_object.public_key
            });
            return;
        }
        client_object.unread_count = 0;
        const key = chat__peer_state_key(client_object);
        const state = chat_peer_local_state.get(key);
        if (state)
        {
            state.unread_count = 0;
        }
        chat__update_app_unread_badge();
    }
}

/**
 * @brief formats an unread count for its badge and accessible label
 *
 * @param count number of unread messages
 *
 * @return object -> badge text, digit count and accessible label
 */
function chat__get_unread_badge_content(count)
{
    let text = count > 99 ? "99+" : String(count);
    return (
    {
        text: text,
        digits: text.length,
        label: count + (count === 1 ? " unread message" : " unread messages")
    });
}

/**
 * @brief creates the unread badge when a client row is drawn
 *
 * @param count number of unread messages
 *
 * @return string -> unread badge markup
 */
function chat__generate_unread_badge_html(count)
{
    let content = chat__get_unread_badge_content(count);
    return '<p class="connected-client-p-received-messages-number" data-unread-digits="' + content.digits + '" title="' + content.label + '" aria-label="' + content.label + '"' + (count > 0 ? '' : ' hidden') + '>' + content.text + '</p>';
}

/**
 * @brief updates the client row and any copies shown in the member list
 *
 * @param client_id client whose unread count changed
 * @param is_visible whether to show the badge
 */
function chat__render_unread_badge(client_id, is_visible)
{
    let count = chat__get_unread_count(client_id);
    let content = chat__get_unread_badge_content(count);
    let badges = document.querySelectorAll('.connected-client[data-connected-client-id="' + client_id + '"] > .connected-client-p-received-messages-number');
    for (let badge of badges)
    {
        badge.hidden = !is_visible || count <= 0;
        badge.textContent = content.text;
        badge.dataset.unreadDigits = String(content.digits);
        badge.title = content.label;
        badge.setAttribute("aria-label", content.label);
    }
}

// remote formatting is restricted to one color and one known font class
/**
 * @brief restricts message colors to supported CSS color syntax
 *
 * @param string value -> requested message color
 *
 * @return string -> accepted color, or inherit
 */
function chat__sanitize_color(value)
{
    if (typeof value !== "string" || value.length > 80)
    {
        return "inherit";
    }
    if (/^#[0-9a-f]{3,4}$/i.test(value) || /^#[0-9a-f]{6}([0-9a-f]{2})?$/i.test(value) || /^[a-z]{1,24}$/i.test(value) || /^(rgb|rgba|hsl|hsla)\([0-9.,%+ /-]+\)$/i.test(value))
    {
        return value;
    }
    return "inherit";
}

/**
 * @brief accepts a bundled font class or an encoded local font class
 *
 * @param string value -> requested message font class
 *
 * @return string -> accepted class, or the default message font class
 */
function chat__sanitize_font(value)
{
    const fonts = ["default", "Yellowtail", "Bruno-Ace-SC", "Raleway", "Kavoon", "Reenie-Beanie",
        "Sacramento", "Sofia", "Nova-Mono", "Codystar", "Londrina-Outline", "Mystery-Quest",
        "Nosifer", "Simonetta", "Hanalei", "Just-Me-Again-Down-Here", "Inter", "Manrope", "Plus-Jakarta-Sans"];
    if (typeof value === "string" && (fonts.some(font => value === "custom-font-usage-" + font) || /^custom-font-usage-local-(?:[0-9a-f]{2}){1,63}$/.test(value)))
    {
        return value;
    }
    return "custom-font-usage-default";
}
