// chat.js is embedded in template.html along with the other client files, and in the node bundle
// it is the chat side: composing and sending (a text, a picture or a file goes from the input to the
// worker for encryption and lands in a chat context), the typing indicator, and the read state of
// conversations (unread counters, badges, seen receipts)
// ui.js calls chat__send_chat_message and the typing side; messages.js and dispatch.js feed the incoming state

// state private to this file
var picture_delivery_pending = false; // between the server's upload ack and image_sent_status (relay to the receivers finished)

var picture_delivery_hide_timer = null;

var upload_ack_timer = null;

var UPLOAD_ACK_TIMEOUT_MS = 300000;   // a slow 10mb upload legitimately takes minutes

var TYPING_SEND_INTERVAL_MS = 3000;   // at most one message per 3s of continuous typing

var TYPING_EXPIRY_MS = 6000;          // twice the send interval, so one lost message does not flicker

// the private-message ids this client already receipted; each is sent once, only to its author
var seen_receipts_already_sent = {};

// which conversations have had our latest message read. one eye in the corner of the chat,
// not one on every bubble
var seen_state_by_context = {};

// private messages that arrived while their conversation was closed. a receipt is owed
// for each, and is sent when the user actually opens it
var unreceipted_messages_by_sender = {};

// ---- composing and sending ----

/**
 * @brief index of a chat context in g_chat_context_array
 *
 * @param string id -> the context id, "chat-context-channel-N", "chat-context-pm-N" or "chat-context-offline-<alias>"
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
 * @brief the state half of promoting an offline conversation into the live one, when its owner connects
 *        every decision (anything to promote, a live thread already there, were we reading it) is
 *        asked of g_chat_context_array, never of the dom; UI.promote_offline_chat_context_render
 *        then only moves the markup
 *
 * @param object client -> the connected client, whose alias names the offline context
 *
 * @return object { did_promote, had_live_context, was_open, offline_context_id, live_context_id }
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

    if (client == null || typeof client.alias !== "string" || client.alias.length == 0)
    {
        return promotion;
    }

    let offline_context_id = "chat-context-offline-" + client.alias;
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
    }
    else
    {
        // both exist: the offline entry is folded into the live one and stops existing
        g_chat_context_array.splice(offline_index, 1);
    }

    // if we were reading or typing in it, keep sending live from now on
    if (promotion.was_open == true)
    {
        g_current_chat_context_id = live_context_id;
        g_chat_message_receiver_type = "user";
        g_chat_message_receiver_id = client.client_id;
        g_offline_chat_recipient_alias = "";
    }

    return promotion;
}

/**
 * @brief clears the upload lock and the progress overlay
 *        the parts loop must not do this itself: it only queues, so releasing there let a second
 *        upload start and wipe the server buffer of the first
 *
 * @return void
 */
function chat__release_file_upload_lock()
{
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
 * @brief the upload ack arrived but the relay to the receivers is still running: says so instead of leaving the grayed thumbnail unexplained
 *        image_sent_status ends it, a two-minute timer is the fallback
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
 * @brief ends the picture delivery status; the overlay stays when a new upload already owns it
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

    // a new upload may already own the overlay; leave it to that one then
    if (g_is_file_being_uploaded == false)
    {
        document.getElementById("upload-progress-overlay").style.display = "none";
        document.getElementById("upload-progress-bar").style.width = "0%";
        document.getElementById("upload-progress-text").innerHTML = "0%";
    }
}

/**
 * @brief whether an upload may start now, one at a time
 *        the intent globals are written before chat__send_file_by_parts runs its guard, so a
 *        refused upload relabelled the one already in flight; call sites ask here first and touch
 *        nothing on refusal
 *
 * @return boolean true when no upload is in flight
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
 * @brief uploads a file in parts, one every delay_ms, with the progress overlay; the lock stays until the server's ack
 *        a file_send_error mid-upload releases the lock, and the loop stops feeding a file the server dropped
 *
 * @param array parts -> the base64 parts
 * @param number total_bytes_length -> the whole file's length, sent with every part
 * @param number delay_ms -> the pause between parts
 *
 * @return void
 */
function chat__send_file_by_parts(parts, total_bytes_length, delay_ms) {

    if (g_is_file_being_uploaded == true)
    {
        utils__custom_alert("cant upload more than 1 file at a time");
        return;
    }

    let i = 0;
    document.getElementById("upload-progress-overlay").style.display = "block";

    function next_part_upload ()
    {
        // a file_send_error released the lock mid-upload: the server dropped this file, stop feeding it
        if (i > 0 && g_is_file_being_uploaded == false)
        {
            return;
        }

        g_is_file_being_uploaded = true;

        if (i >= parts.length)
        {
            // queued, not delivered. hold the lock until the server acks, with a timer so a lost
            // ack cannot block uploads for the rest of the session
            document.getElementById("upload-progress-text").innerHTML = "waiting for server ...";
            upload_ack_timer = setTimeout(chat__release_file_upload_lock, UPLOAD_ACK_TIMEOUT_MS);
            return;
        }

        const part = parts[i];

        // send

        client_msg.send_file_send_request(total_bytes_length, part, i);

        i++;

        let percent = i / (parts.length / 100);
        document.getElementById("upload-progress-bar").style.width = percent + '%';
        document.getElementById("upload-progress-text").innerHTML = Math.round(percent) + '%';

        setTimeout(next_part_upload, delay_ms);
    }

    next_part_upload();
}

/**
 * @brief sends the attached file as a message of its own; typed text stays in the box for the next send
 *        every way this can fail tells the user and keeps the file attached
 *
 * @param string receiver_type -> "channel" or "user"
 *
 * @return boolean true when the file was handed off
 */
function chat__send_pending_chat_file(receiver_type)
{
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
            utils__custom_alert(chat__sanitize_string(channel_tree__get_display_name_by_client_id(g_chat_message_receiver_id, receiver.username)) + " is idle, files can not be delivered to idle clients");
            return false;
        }

        receiver_public_key = channel_tree__get_public_key_by_client_id(g_chat_message_receiver_id);

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

    // lock now, not when the worker answers: encrypting 20 MB takes a moment and a second
    // send in that window must be refused instead of relabelling this one
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
 * @brief the one send path for what sits in the input, for both targets
 *        "channel" is the current channel (keys already known), "user" the selected person
 *        (encrypted to their public key); a pending file goes first, on its own.
 *        chat__send_chat_message decides the target and the offline cases
 *
 * @param string target -> "channel" or "user"
 *
 * @return void
 */
function chat__send_composed_chat_message(target)
{
    if (g_pending_chat_file != null)
    {
        chat__send_pending_chat_file(target);
        return;
    }

    let text_input = document.getElementById("chat-input-container-text-input");
    let chat_message_to_send_value = text_input.value;
    text_input.value = "";

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
        receiver_public_key = channel_tree__get_public_key_by_client_id(g_chat_message_receiver_id);
        if (receiver_public_key == "" || receiver_public_key == null)
        {
            utils__custom_log("[send] no public key for receiver " + g_chat_message_receiver_id);
            return;
        }

        // a new message of ours is unread by definition, so the eye goes until they read it
        chat__clear_seen_indicator_for_client(g_chat_message_receiver_id);
    }

    let chat_context = document.getElementById(g_current_chat_context_id);
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

        // lock now, like the file path: encrypting a big picture takes a while and a second
        // send in that window must be refused, not relabel this one
        g_is_file_being_uploaded = true;
        document.getElementById("upload-progress-overlay").style.display = "block";
        document.getElementById("upload-progress-text").innerHTML = "processing image for send ...";

        if (target == "user")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_direct_chat_picture",
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
        let actual_chat_message_to_send_value = JSON.stringify({
            font: g_selected_font,
            font_size: g_selected_font_size,
            font_color: g_selected_font_color,
            value: chat_message_to_send_value
        });

        let text_html = "<p class='single-chat-message-content-p local-single-chat-message-content-p " + g_selected_font + "' data-single-chat-message-local-message-id='" + g_local_message_id + "' data-single-chat-message-server-message-id='unspecified' style='color: " + g_selected_font_color + "; font-size: " + g_selected_font_size + "px;'>" + chat__sanitize_string(chat_message_to_send_value) + "</p>";

        if (g_chat_context_array[chat_context_index].last_known_message_sender_username == g_local_username)
        {
            // a follow-up line by the same sender joins their last block
            let blocks = chat_context.getElementsByClassName("single-chat-message");
            blocks[blocks.length - 1].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", text_html);
        }
        else
        {
            chat__append_local_chat_message_block(chat_context, sender_html, time_html, text_html);
        }

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
 * @brief scrolls the chat to its newest message
 *        a received message only does so while the local "auto-scroll to the end" setting is on, so a
 *        person reading older messages is not yanked down; the user's own send or tab switch always does
 *
 * @param boolean is_forced -> true ignores the setting
 *
 * @return void
 */
function chat__scroll_chat_to_end(is_forced)
{
    if (is_forced != true && g_auto_scroll_chat_to_end == false)
    {
        return;
    }

    let container = document.getElementById("chat-context-container");

    if (container == null)
    {
        return;
    }

    container.scrollTop = container.scrollHeight;
}

/**
 * @brief a fresh message block of our own at the end of a chat context
 *        the spacer that keeps the bottom padding is dropped first and re-added last, so it stays the final element
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

    chat_context.insertAdjacentHTML("beforeend",
        "<div class=\"single-chat-message\">\n\
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
    chat__send_composed_chat_message("user");
}

/**
 * @brief text to somebody who is not connected: the same encryption as a direct message (their public key came with the offline roster), addressed by alias
 *        the server parks it and hands it over when they return
 *
 * @param object offline_contact -> the entry of g_offline_client_list
 *
 * @return void
 */
function chat__send_offline_chat_message(offline_contact)
{
    let chat_message_to_send_value = document.getElementById('chat-input-container-text-input').value;
    document.getElementById('chat-input-container-text-input').value = "";

    if (chat_message_to_send_value.trim().length == 0)
    {
        return;
    }

    let context_id = "chat-context-offline-" + offline_contact.alias;

    let actual_data_to_send = {
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
        chat__scroll_chat_to_end(true);
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__create_offline_chat_message",
        chat_message_value: JSON.stringify(actual_data_to_send),
        receiver_public_key: offline_contact.public_key,
        recipient_alias: offline_contact.alias
    });
}

/**
 * @brief the send-button entry point: routes to channel, direct or offline sending by the current receiver type, with fallbacks when the direct receiver went offline or came back
 *        every exit is logged, because a message that quietly goes nowhere is the hardest kind of bug to chase
 *
 * @return void
 */
function chat__send_chat_message()
{
    // EVERY exit from this function is logged. a message that quietly goes nowhere is
    // the hardest kind of bug to chase, and this path has several silent returns
    utils__custom_log("[send] type=" + g_chat_message_receiver_type
        + " receiver_id=" + g_chat_message_receiver_id + " (" + (typeof g_chat_message_receiver_id) + ")"
        + " context=" + g_current_chat_context_id);

    if (g_chat_message_receiver_type == "channel")
    {
        if (g_current_channel_keys == null)
        {
            utils__custom_log("[send] STOP: no channel keys");
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

            utils__custom_log("[send] receiver not in client list, trying the offline paths"
                + " (alias='" + g_offline_chat_recipient_alias + "')");
            // they may have come online since this context was opened: then send it live instead
            // of queueing (the server refuses to queue for a connected client)
            let live_client = channel_tree__get_client_by_alias(g_offline_chat_recipient_alias);

            if (live_client != null)
            {
                // state first, markup second. routing this through UI.* alone would lose the
                // whole promotion in a runtime with no ui to paint
                let promotion = chat__promote_offline_chat_context_state(live_client);
                UI.promote_offline_chat_context_render(promotion, live_client);

                utils__custom_log("[send] promoted offline context to live, sending direct");
                chat__send_direct_chat_message();
                return;
            }

            let offline_contact = channel_tree__get_stored_client_by_alias(g_offline_chat_recipient_alias);

            if (offline_contact != null && typeof offline_contact.public_key === "string" && offline_contact.public_key.length > 0)
            {
                utils__custom_log("[send] sending as OFFLINE message");
                chat__send_offline_chat_message(offline_contact);
                return;
            }

            if (offline_contact != null)
            {
                // we know who they are but were never given their public key, so nothing
                // can be encrypted for them - the server does not hold messages here
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
 * @brief finishing an inline chat-message edit: locks the element again and sends the edit_chat_message_request with the new content to the server
 *
 * @param object event -> the blur event of the edited element
 *
 * @return void
 */
function chat__local_chat_message_onblur(event)
{
    event.target.setAttribute("contenteditable", "false");
    event.target.removeEventListener('blur', chat__local_chat_message_onblur);

    let message_object = {
        message: {
            type: "edit_chat_message_request",
            message_id: g_selected_server_chat_message_id,
            new_message_value: event.target.innerHTML,
            receiver_type: g_chat_message_receiver_type,
            receiver_id: parseInt(g_chat_message_receiver_id)
        }
    };

    connection__send_message_object(message_object);
}

/**
 * @brief html-entity escape for element text and quoted attribute values: < > & and both quotes
 *        not enough for unquoted attributes, javascript: urls, event handlers or script/style
 *        bodies, and not idempotent, so escape exactly once on the way into the dom; an <img> src
 *        takes chat__sanitize_image_data_url
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
    str = "" + str; // coerce so a non-string (malformed/attacker field) cannot throw on .replace
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
 * @brief the allowlist for a decrypted picture used as an <img> src: only a base64 image data url passes
 *        anything else (an external url that would beacon the viewer, a javascript: or html
 *        scheme, junk) becomes a blank placeholder; .src does not decode entities, so
 *        chat__sanitize_string is the wrong tool
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

    // 1x1 transparent png, shown in place of anything that is not a well-formed image data URL
    return "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M8AAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
}

/**
 * @brief strips the "data:...;base64," prefix
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
 * @brief splits a string into up to required_parts_count roughly equal slices
 *
 * @param string string_to_split -> the text
 * @param number required_parts_count -> how many slices
 *
 * @return array the slices
 */
function chat__split_string_into_smaller_parts(string_to_split, required_parts_count)
{
    const result = [];
    const partsize = Math.ceil(string_to_split.length / required_parts_count);

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

// ---- typing indicator ----

/**
 * @brief tells whoever is on the other side of the open conversation that we are writing
 *        called on every keystroke but sends at most once every few seconds, so a long message
 *        is a handful of tiny messages, not one per character; carries no text, only where it belongs
 *
 * @return void
 */
function chat__send_typing_indicator()
{
    if (g_server_policy.allow_typing_indicator == false || g_is_authenticated == false)
    {
        return;
    }

    // an offline conversation has nobody to tell
    if (g_offline_chat_recipient_alias != null && g_offline_chat_recipient_alias.length > 0)
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
 * @brief the chat context id a typing notice belongs to, matching the ids the contexts are built with, so the notice shows only while that conversation is on screen
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
 * @brief one typing line: the text, then three dots that animate on their own
 *        the dots are separate spans so css can fade them in one after another; rewriting the
 *        text on a timer would repaint the whole line several times a second
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
    // textContent, never innerHTML: a username is user-supplied
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
 * @brief paints the "x is typing ..." line for the conversation on screen, drops entries that were not refreshed, and stops its own ticker once nothing is left to show
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
        if (typers == null) { continue; }

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

    // one line per person, stacked. capped at three so a busy channel cannot push the chat
    // upwards without end - the rest is counted on a final line
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

        // "block", never "": clearing the inline style falls back to the stylesheet,
        // where this element is display:none - so it would never actually appear
        element.style.display = "block";
    }

    if (anything_left == false && g_typing.render_timer != null)
    {
        window.clearInterval(g_typing.render_timer);
        g_typing.render_timer = null;
    }
}

// ---- unread and seen ----

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

    let is_seen = (g_current_chat_context_id != null)
        && (seen_state_by_context[g_current_chat_context_id] == true)
        && (g_show_seen_indicator == true);

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
 * @brief queues a private message that arrived while its conversation was closed; a receipt is owed for it and sent when the user opens the conversation
 *
 * @param number sender_client_id -> the author
 * @param number server_chat_message_id -> the message
 *
 * @return void
 */
function chat__remember_message_awaiting_receipt(sender_client_id, server_chat_message_id)
{
    if (server_chat_message_id == null || sender_client_id == g_local_client_id)
    {
        return;
    }

    if (unreceipted_messages_by_sender[sender_client_id] == null)
    {
        unreceipted_messages_by_sender[sender_client_id] = [];
    }

    unreceipted_messages_by_sender[sender_client_id].push(server_chat_message_id);
}

/**
 * @brief sends the receipts owed to one sender, once, when their conversation is opened
 *
 * @param number sender_client_id -> the author
 *
 * @return void
 */
function chat__send_pending_seen_receipts(sender_client_id)
{
    let pending = unreceipted_messages_by_sender[sender_client_id];

    if (pending == null)
    {
        return;
    }

    unreceipted_messages_by_sender[sender_client_id] = [];

    for (let x = 0; x < pending.length; x++)
    {
        chat__send_seen_receipt_for_message(sender_client_id, pending[x]);
    }
}

/**
 * @brief tells a sender we read their private message, at most once per message, only while somebody is looking and the local setting allows it
 *
 * @param number sender_client_id -> the author
 * @param number server_chat_message_id -> the message
 *
 * @return void
 */
function chat__send_seen_receipt_for_message(sender_client_id, server_chat_message_id)
{
    if (chat__is_the_user_actually_looking() == false)
    {
        return;
    }

    if (g_send_seen_receipts == false)
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

// coming back to the app pays whatever was owed for the conversation on screen. without
// this a message read on return only receipted if the chat was clicked a second time
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
 * @brief whether the screen is on and this page is in front of it
 *        headless node always answers no: nobody is looking at it, and if it answered yes every
 *        read would be receipted twice
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
 * @brief counts one more unread message in a channel and repaints its badge
 *        the channel being read never accumulates a badge, but "being read" needs somebody
 *        looking; under node with no ui attached nobody is, so everything counts
 *
 * @param number channel_id -> the channel the message arrived in
 *
 * @return void
 */
function chat__increment_channel_unread_count(channel_id)
{
    // the channel being read never accumulates a badge, but "being read" needs somebody looking;
    // under node with no ui attached nobody is, so everything counts
    if (android_host__is_someone_watching_the_ui() == true
        && g_current_chat_context_id === ("chat-context-channel-" + channel_id))
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
    g_channel_unread_counts[channel_id] = 0;
    chat__render_channel_unread_badge(channel_id);
    chat__update_app_unread_badge();
}

/**
 * @brief paints a channel's unread badge and mirrors it into the member list
 *        a class, not style.display: whether the badge shows at all is the theme's call (mobile
 *        themes only), and an inline display would override that decision
 *
 * @param number channel_id -> the channel
 *
 * @return void
 */
function chat__render_channel_unread_badge(channel_id)
{
    let badge = document.querySelector('[data-channel-unread-for="' + channel_id + '"]');

    if (badge == null)
    {
        return;
    }

    let count = chat__get_channel_unread_count(channel_id);
    badge.innerHTML = count;

    // a class, not style.display: whether the badge shows at all is the theme's call
    // (mobile themes only), and an inline display would override that decision
    if (count > 0)
    {
        badge.classList.remove("unread-empty");
    }
    else
    {
        badge.classList.add("unread-empty");
    }

    // the strip themes hide the channel tree and show a circle instead; the count
    // lives on that circle, which is rebuilt by the member-list mirror
    if (typeof UI !== "undefined" && typeof UI.schedule_member_list_sync === "function")
    {
        UI.schedule_member_list_sync();
    }
}

/**
 * @brief the launcher icon badge: every unread channel plus every unread private conversation
 *        android shows it through the notification count via the java bridge; node hands the
 *        number to its host, which keeps the badge working while the app is idle or closed
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

    for (let i = 0; i < g_client_list.length; i++)
    {
        if (typeof g_client_list[i].unread_count === "number")
        {
            total += g_client_list[i].unread_count;
        }
    }

    // the webview reports through the java bridge; node has no Android object and hands the number
    // to its host, which is what keeps the badge working while the app is idle or closed
    if (typeof Android !== "undefined" && typeof Android.JavaExportSetUnreadBadge === "function")
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
    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null)
    {
        client_object.unread_count = chat__get_unread_count(client_id) + 1;
    chat__update_app_unread_badge();
    }
}

/**
 * @brief opening a private conversation is reading it: the count goes to zero and the receipts owed are sent
 *
 * @param number client_id -> the other person
 *
 * @return void
 */
function chat__clear_unread_count(client_id)
{
    // opening it is reading it, so anything that arrived while it was closed is
    // receipted now
    chat__send_pending_seen_receipts(client_id);
    chat__render_seen_indicator();

    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null)
    {
        client_object.unread_count = 0;
    chat__update_app_unread_badge();
    }
}

/**
 * @brief paints a client row's unread badge from the state; visibility is a parameter, because one caller hides the badge without clearing it (a message grouped under the same sender)
 *
 * @param number client_id -> the client
 * @param boolean is_visible -> whether the badge shows
 *
 * @return void
 */
function chat__render_unread_badge(client_id, is_visible)
{
    let row = document.querySelector('[data-connected-client-id="' + client_id + '"]');

    if (row == null)
    {
        return;
    }

    let badge = row.getElementsByClassName("connected-client-p-received-messages-number")[0];

    if (badge == null)
    {
        return;
    }

    badge.style.display = (is_visible == true) ? "inline-block" : "none";
    badge.innerHTML = chat__get_unread_count(client_id);
}
