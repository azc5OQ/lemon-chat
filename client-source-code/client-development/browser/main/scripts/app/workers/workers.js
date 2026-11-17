// handles socket transport and message encryption in separate workers
// worker-entry.js selects the handler. connection.js supplies each connection's ports

// pad file bodies above the server's minimum encoded upload length
var CHAT_FILE_BODY_MIN_PADDED_BYTES = 4096;

// forward decrypted frames to Node for page replay when enabled
var dpw_forward_frames = false;

var websocket_instance = null;

// count socket generations so replaced sockets cannot publish events
var websocket_generation = 0;

var dpw_channel_key_request = null;
var dpw_channel_members = new Map();
var dpw_local_client_id = -1;
var dpw_current_channel_id = -1;
var dpw_channel_maintainer_id = -1;
var dpw_maintainer_generation = -1;
var dpw_channel_keys_need_refresh = true;

var dpw_socket_port = null;
var dpw_connection_generation = 0;
var dpw_local_delivery_scope = null;
var dpw_local_session_id = "";
var dpw_local_transfer_receiver = null;
var dpw_connection_open = false;
var dpw_keepalive_timer = null;
var dpw_keepalive_last_reply = 0;
var dpw_keepalive_sent_at = 0;
var dpw_received_bytes = 0;

var websocket_data_port = null;
var websocket_connection_generation = 0;
var websocket_close_reason = "";
var websocket_upload_pending = null;
var websocket_upload_timer = null;
var WEBSOCKET_UPLOAD_BUFFER_LIMIT = 64 * 1024;
var WEBSOCKET_UPLOAD_WAIT_MS = 120000;

/**
 * @brief tracks channel membership in socket order so received keys need not wait for the interface
 *
 * @param object message -> a decrypted server message
 */
function workers__update_channel_members(message)
{
    let member = null;
    let local = null;
    let departing_member = dpw_channel_members.get(message.client_id);
    let was_maintainer = departing_member != null && departing_member.client_id === dpw_channel_maintainer_id;
    let left_channel = departing_member != null && !departing_member.is_idle && !departing_member.is_music_bot && departing_member.channel_id === dpw_current_channel_id && (message.type === "client_disconnect" || message.type === "client_going_to_idle_mode" || ((message.type === "channel_join" || message.type === "client_coming_back_from_idle_mode") && message.channel_id !== dpw_current_channel_id));
    if (message.type === "client_list")
    {
        dpw_channel_keys_need_refresh = true;
        dpw_channel_members.clear();
        dpw_local_client_id = -1;
        dpw_current_channel_id = -1;
        dpw_channel_maintainer_id = -1;
        dpw_maintainer_generation = -1;
        dpw_channel_key_request = null;
        for (member of message.clients)
        {
            dpw_channel_members.set(member.client_id,
            {
                client_id: member.client_id, channel_id: member.channel_id,
                is_idle: member.is_idle, is_music_bot: member.is_music_bot, public_key: member.public_key
            });
            if (member.public_key === g_rsa_public_key_string && !member.is_music_bot)
            {
                dpw_local_client_id = member.client_id;
                dpw_current_channel_id = member.channel_id;
            }
        }
    }
    else if (message.type === "client_connect")
    {
        dpw_channel_members.set(message.client_id,
        {
            client_id: message.client_id, channel_id: message.channel_id,
            is_idle: message.is_idle, is_music_bot: message.is_music_bot, public_key: message.public_key
        });
    }
    else if (message.type === "client_disconnect")
    {
        dpw_channel_members.delete(message.client_id);
    }
    else if (message.type === "channel_join" || message.type === "client_coming_back_from_idle_mode" || message.type === "client_going_to_idle_mode")
    {
        member = dpw_channel_members.get(message.client_id);
        if (member != null)
        {
            member.is_idle = message.type === "client_going_to_idle_mode";
            member.channel_id = member.is_idle ? -2 : message.channel_id;
            if (member.client_id === dpw_local_client_id && member.channel_id !== dpw_current_channel_id)
            {
                dpw_current_channel_id = member.channel_id;
                dpw_channel_maintainer_id = -1;
                dpw_maintainer_generation = -1;
                dpw_channel_keys_need_refresh = true;
                dpw_channel_key_request = null;
                keys__set_current_channel_keys(null);
            }
        }
    }
    if (message.type === "channel_maintainer_id" && message.channel_id === dpw_current_channel_id)
    {
        member = dpw_channel_members.get(message.maintainer_id);
        local = dpw_channel_members.get(dpw_local_client_id);
        if (message.has_maintainer && member != null && !member.is_idle && !member.is_music_bot && local != null && !local.is_idle && member.channel_id === dpw_current_channel_id && Number.isSafeInteger(message.maintainer_generation) && message.maintainer_generation >= 0)
        {
            if (dpw_channel_maintainer_id !== message.maintainer_id || dpw_maintainer_generation !== message.maintainer_generation)
            {
                dpw_channel_key_request = null;
                dpw_channel_keys_need_refresh = true;
            }
            dpw_channel_maintainer_id = message.maintainer_id;
            dpw_maintainer_generation = message.maintainer_generation;
        }
        else
        {
            dpw_channel_maintainer_id = -1;
            dpw_maintainer_generation = -1;
            dpw_channel_key_request = null;
        }
    }
    if (left_channel && (was_maintainer || !keys__may_reuse_channel_keys("leave", workers__channel_member_count())))
    {
        dpw_channel_keys_need_refresh = true;
        workers__cancel_channel_key_distribution();
    }
    member = dpw_channel_members.get(dpw_channel_maintainer_id);
    if (member == null || member.is_idle || member.is_music_bot || member.channel_id !== dpw_current_channel_id)
    {
        dpw_channel_maintainer_id = -1;
        dpw_maintainer_generation = -1;
        dpw_channel_key_request = null;
    }
}

/**
 * @brief counts active people using membership received directly from the socket
 *
 * @return number people in the worker's current channel
 */
function workers__channel_member_count()
{
    let count = 0;
    for (let member of dpw_channel_members.values())
    {
        if (member.channel_id === dpw_current_channel_id && !member.is_idle && !member.is_music_bot)
        {
            count++;
        }
    }
    return count;
}

/**
 * @brief stops obsolete key delivery and tells the interface that it has finished
 */
function workers__cancel_channel_key_distribution()
{
    let request = dpw_channel_key_request;
    dpw_channel_key_request = null;
    if (request != null)
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__channel_key_creation_failed",
            cancelled: true,
            generation: request.generation,
            channel_id: request.channel_id
        });
    }
}

/**
 * @brief applies key reuse settings immediately, including while the interface is busy
 *
 * @param object policy -> settings from login or a live server policy message
 */
function workers__apply_channel_key_policy(policy)
{
    let previous = keys__channel_key_reuse_policy_signature(g_server_policy);
    for (let field of ["is_channel_key_reuse_enabled", "is_channel_key_reuse_on_join_allowed", "is_channel_key_reuse_on_leave_allowed"])
    {
        if (typeof policy[field] === "boolean")
        {
            g_server_policy[field] = policy[field];
        }
    }
    for (let field of ["channel_key_reuse_join_min_clients", "channel_key_reuse_leave_min_clients"])
    {
        if (Number.isSafeInteger(policy[field]) && policy[field] >= 30 && policy[field] <= 1000000)
        {
            g_server_policy[field] = policy[field];
        }
    }
    if (previous !== keys__channel_key_reuse_policy_signature(g_server_policy))
    {
        dpw_channel_keys_need_refresh = true;
        workers__cancel_channel_key_distribution();
    }
}

/**
 * @brief checks a distribution request against membership already received from the socket
 *
 * @param object request -> the interface's request to create keys
 *
 * @return boolean true when this client still maintains that channel
 */
function workers__is_channel_key_request_current(request)
{
    return g_is_authenticated && request.local_client_id === dpw_local_client_id && request.channel_id === dpw_current_channel_id && dpw_channel_maintainer_id === dpw_local_client_id && request.maintainer_generation === dpw_maintainer_generation;
}

/**
 * @brief checks whether decrypted direct content matches its declared purpose
 *
 * @param string purpose -> the purpose passed on by the server
 * @param string content_type -> the type field of the decrypted content
 *
 * @return boolean true when the content may be handled
 */
function workers__is_direct_content_allowed_for_purpose(purpose, content_type)
{
    if (purpose === "channel_keys")
    {
        return content_type === "channel_keys_from_maintainer";
    }
    if (purpose === "seen_receipt")
    {
        return content_type === "message_seen";
    }
    return content_type !== "channel_keys_from_maintainer";
}

/**
 * @brief installs received keys before the next encrypted chat message is processed
 *
 * @param object msg -> a decrypted private message containing channel keys
 *
 * @return boolean true when the keys belong to this channel and its current maintainer
 */
function workers__receive_channel_keys(msg)
{
    let message = msg.message;
    let keys = null;
    if (message.sender_id !== dpw_channel_maintainer_id || dpw_channel_maintainer_id < 0)
    {
        return false;
    }
    keys = keys__read_channel_key_message(message, dpw_current_channel_id, dpw_maintainer_generation);
    if (keys == null)
    {
        return false;
    }
    dpw_channel_key_request = null;
    keys__set_current_channel_keys(keys);
    return true;
}

/**
 * @brief identifies the connection that produced a result, so an old result cannot change a new session
 *
 * @param object message -> the result sent to the page
 */
function workers__post_to_main(message)
{
    if (dpw_connection_generation > 0 && message.type !== "data_processing_worker__generate_rsa_keypair_result" && message.type !== "data_processing_worker__generate_rsa_keypair_failed")
    {
        message.connection_generation = dpw_connection_generation;
    }
    if (dpw_local_delivery_scope != null && message.type !== "decrypted_frame" && message.type !== "data_processing_worker__connection_activity" && message.type !== "data_processing_worker__local_transfer_ack")
    {
        message.local_delivery = dpw_local_delivery_scope.value;
        dpw_local_delivery_scope.outputs++;
    }
    global.postMessage(message);
}

/**
 * @brief stops checking the old connection before closing it or replacing its keys
 */
function workers__stop_keepalive()
{
    if (dpw_keepalive_timer != null)
    {
        clearInterval(dpw_keepalive_timer);
        dpw_keepalive_timer = null;
    }
}

/**
 * @brief sends the usual encrypted keepalive without waiting for the page to finish drawing
 */
function workers__send_keepalive()
{
    let now = performance.now();
    let data = null;

    if (!dpw_connection_open || !g_is_authenticated || dpw_socket_port == null)
    {
        return;
    }
    if (g_server_policy.is_connection_check_enforced !== false && now - dpw_keepalive_last_reply > g_connection_check.lost_threshold_ms)
    {
        workers__stop_keepalive();
        dpw_connection_open = false;
        dpw_socket_port.postMessage(
        {
            type: "close",
            connection_generation: dpw_connection_generation,
            reason: "no valid server message for " + Math.round(now - dpw_keepalive_last_reply) + " ms"
        });
        return;
    }
    data = keys__encrypt_all_message_data_and_convert_to_base64(JSON.stringify(
    {
        message: { type: "client_connection_check" }
    }));
    dpw_keepalive_sent_at = now;
    dpw_socket_port.postMessage(
    {
        type: "send",
        value: data,
        connection_generation: dpw_connection_generation
    });
    workers__post_to_main(
    {
        type: "data_processing_worker__connection_bytes",
        bytes_received: dpw_received_bytes,
        bytes_sent: data.length
    });
    dpw_received_bytes = 0;
}

/**
 * @brief applies the server's timeout setting without waiting for the settings window
 *
 * @param object policy -> fields from the login reply or a later server policy message
 */
function workers__apply_keepalive_policy(policy)
{
    if (dpw_socket_port == null || typeof policy.is_connection_check_enforced !== "boolean")
    {
        return;
    }
    if (g_server_policy.is_connection_check_enforced === false && policy.is_connection_check_enforced)
    {
        dpw_keepalive_last_reply = performance.now();
    }
    g_server_policy.is_connection_check_enforced = policy.is_connection_check_enforced;
}

/**
 * @brief receives socket frames and close events in socket order
 *
 * @param object e -> a message from this connection's WebSocket worker
 */
function workers__socket_port_onmessage(e)
{
    if (e.data.connection_generation !== dpw_connection_generation)
    {
        return;
    }
    if (e.data.type === "websocket_worker_onmessage")
    {
        if (!dpw_connection_open)
        {
            return;
        }
        dpw_received_bytes += e.data.value.length;
        workers__mainthread__process_received_websocket_message_continue(e);
    }
    else
    {
        workers__stop_keepalive();
        dpw_channel_key_request = null;
        dpw_connection_open = false;
        g_is_authenticated = false;
        workers__post_to_main(e.data);
    }
}

/**
 * @brief distributes channel keys without blocking incoming messages for the whole member list
 *
 * @param object request -> the current members, channel id and request generation
 *
 * @return Promise<void> finishes after delivery or cancellation
 */
async function workers__create_channel_keys(request)
{
    let reuse = request.reuse_existing_keys === true && !dpw_channel_keys_need_refresh && g_current_channel_keys != null && keys__may_reuse_channel_keys("join", workers__channel_member_count());
    let new_channel_keys = reuse ? g_current_channel_keys : keys__create_random_message_keys();
    let arrivals = new Set(request.arriving_client_ids || []);
    let recipients = reuse ? request.clients.filter(function(client)
    {
        return arrivals.has(client.client_id);
    }) : request.clients;
    let member = null;
    let recipient = null;
    let message_keys = null;
    let message_value =
    {
        type: "channel_keys_from_maintainer",
        channel_id: request.channel_id,
        maintainer_generation: request.maintainer_generation,
        value: JSON.stringify(new_channel_keys)
    };
    let message_data = null;
    let message = null;
    let x = 0;
    if (!workers__is_channel_key_request_current(request))
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__channel_key_creation_failed",
            generation: request.generation,
            channel_id: request.channel_id
        });
        return;
    }
    dpw_channel_key_request = request;
    // recipients can reply with these keys before distribution finishes
    keys__set_current_channel_keys(new_channel_keys);
    dpw_channel_keys_need_refresh = false;
    for (x = 0; x < recipients.length; x++)
    {
        // let received messages and keepalives run between recipient encryptions
        await new Promise(function(resolve)
        {
            setTimeout(resolve, 0);
        });
        if (dpw_channel_key_request !== request || !workers__is_channel_key_request_current(request))
        {
            return;
        }
        recipient = recipients[x];
        member = dpw_channel_members.get(recipient.client_id);
        if (member == null || member.is_idle || member.is_music_bot || member.channel_id !== request.channel_id || member.public_key !== recipient.public_key || (reuse && !arrivals.has(recipient.client_id)))
        {
            continue;
        }
        if (recipient.channel_id !== request.current_channel_id || (recipient.client_id === request.local_client_id && typeof process === "undefined"))
        {
            continue;
        }
        message_keys = keys__create_random_message_keys();
        message_data = keys__seal_direct_content(recipient.public_key, message_keys, keys__encrypt_direct_content(message_keys, JSON.stringify(message_value)));
        if (message_data == null)
        {
            throw new Error("cannot encrypt channel keys for client " + recipient.client_id);
        }
        message =
        {
            message:
            {
                type: "direct_chat_message",
                purpose: "channel_keys",
                value: JSON.stringify(message_data),
                receiver_type: null,
                receiver_id: recipient.client_id,
                local_message_id: 1
            }
        };
        if (recipient.client_id === request.local_client_id)
        {
            // the Android host gives its own copy directly to its attached interface
            workers__post_to_main(
            {
                type: "data_processing_worker__local_channel_keys_for_ui",
                generation: request.generation,
                maintainer_generation: request.maintainer_generation,
                channel_id: request.channel_id,
                value: JSON.stringify(
                {
                    message:
                    {
                        type: "direct_chat_message",
                        sender_id: request.local_client_id,
                        sender_username: recipient.username,
                        value: null,
                        some_json: message_value
                    }
                })
            });
        }
        else
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__create_websocket_channel_keys_message_result",
                generation: request.generation,
                maintainer_generation: request.maintainer_generation,
                channel_id: request.channel_id,
                channel_keys_message_content: keys__encrypt_all_message_data_and_convert_to_base64(JSON.stringify(message)),
                username: recipient.username
            });
        }
    }
    if (dpw_channel_key_request !== request || !workers__is_channel_key_request_current(request))
    {
        return;
    }
    dpw_channel_key_request = null;
    workers__post_to_main(
    {
        type: "data_processing_worker__new_channel_keys_from_data_processing_worker",
        generation: request.generation,
        maintainer_generation: request.maintainer_generation,
        channel_id: request.channel_id,
        reused_keys: reuse,
        value: new_channel_keys
    });
}

// ---- data-processing worker ----

/**
 * @brief encrypts a direct or channel picture and reports its preparation result
 *
 * @param data picture data, recipient and operation identifiers
 * @param direct true for a private picture. false for a channel picture
 */
function workers__prepare_picture_upload(data, direct)
{
    try
    {
        let sealed;
        if (direct)
        {
            const message_keys = [];
            for (let index = 0; index < 4; index++)
            {
                message_keys.push(
                {
                    key_bytes: Array.from(crypto.getRandomValues(new Uint8Array(32))),
                    iv_bytes: Array.from(crypto.getRandomValues(new Uint8Array(16)))
                });
            }
            const envelope = keys__seal_direct_content(data.receiver_public_key, message_keys, keys__encrypt_direct_content(message_keys, data.base64_picture_string_to_send));
            sealed = envelope == null ? null : JSON.stringify(envelope);
        }
        else
        {
            sealed = keys__seal_channel_content(data.current_channel_keys, keys__encrypt_with_aes_keys_and_convert_to_base64(data.current_channel_keys, data.base64_picture_string_to_send));
        }
        if (sealed == null)
        {
            throw new Error("Picture encryption failed");
        }
        workers__post_to_main(
        {
            type: direct ? "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts" : "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts",
            data_for_upload_process: sealed,
            extra_data:
            {
                receiver_id: parseInt(direct ? data.chat_message_receiver_id : data.current_channel_id),
                local_message_id: data.local_message_id,
                operation_id: data.operation_id
            }
        });
    }
    catch (error)
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__chat_picture_send_failed",
            local_message_id: data.local_message_id,
            operation_id: data.operation_id,
            reason: "The picture could not be encrypted. You can try sending it again."
        });
    }
}

/**
 * @brief checks delivery identifiers against the worker's current local session
 *
 * @param object|null delivery -> local delivery metadata
 *
 * @return boolean -> true for valid identifiers in the current session
 */
function workers__is_local_delivery_current(delivery)
{
    return delivery != null && typeof delivery === "object" && !Array.isArray(delivery) && typeof delivery.id === "string" && delivery.id.length > 0 && delivery.id.length <= 128 && typeof delivery.session_id === "string" && delivery.session_id.length > 0 && delivery.session_id.length <= 128 && delivery.session_id === dpw_local_session_id;
}

/**
 * @brief posts an attachment result before acknowledging its local delivery
 *
 * @param object message -> worker result to send
 * @param object|null request -> original request with optional local delivery metadata
 *
 * @return void
 */
function workers__post_attachment_result(message, request)
{
    const delivery = request == null ? null : request.local_delivery;
    if (delivery != null)
    {
        if (g_loopback_port <= 0 || !workers__is_local_delivery_current(delivery))
        {
            return;
        }
        message.local_delivery = {id: delivery.id, session_id: delivery.session_id};
    }
    workers__post_to_main(message);
    if (delivery != null)
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__local_delivery_accepted",
            value: {id: delivery.id, session_id: delivery.session_id}
        });
    }
}

/**
 * @brief reports file decryption failure to the page and local delivery owner
 *
 * @param number|string file_id -> failed file identifier
 * @param object request -> original decryption request
 *
 * @return void
 */
function workers__post_file_decrypt_failure(file_id, request)
{
    if (dpw_forward_frames === true)
    {
        workers__post_to_main(
        {
            type: "decrypted_frame",
            value: JSON.stringify({message: {type: "chat_file_decrypt_failed", file_id: file_id}})
        });
    }
    workers__post_attachment_result(
    {
        type: "data_processing_worker__chat_file_decrypt_failed",
        value: {message: {file_id: file_id}}
    }, request);
}

/**
 * @brief reports picture decryption failure to the page and local delivery owner
 *
 * @param number|string picture_id -> failed picture identifier
 * @param object request -> original decryption request
 *
 * @return void
 */
function workers__post_picture_decrypt_failure(picture_id, request)
{
    if (dpw_forward_frames === true)
    {
        workers__post_to_main(
        {
            type: "decrypted_frame",
            value: JSON.stringify({message: {type: "chat_picture_decrypt_failed", picture_id: picture_id}})
        });
    }
    workers__post_attachment_result(
    {
        type: "data_processing_worker__chat_picture_decrypt_failed",
        value: {message: {picture_id: picture_id}}
    }, request);
}

/**
 * @brief handles identity, encryption and decrypted socket messages in the data worker
 *
 * @param object e -> the worker message event
 *
 * @return void
 */
function workers__data_processing_worker_onmessage(e)
{
    if (e.data.type === "mainthread__begin_connection")
    {
        if (Number.isSafeInteger(e.data.connection_generation) && e.data.connection_generation > dpw_connection_generation)
        {
            workers__cancel_local_transfers("connection_replaced");
            dpw_connection_generation = e.data.connection_generation;
        }
        return;
    }
    if (e.data.connection_generation != null && e.data.connection_generation !== dpw_connection_generation && e.data.type !== "mainthread__connect_socket_port")
    {
        return;
    }
    if (e.data.local_delivery != null && (g_loopback_port <= 0 || !workers__is_local_delivery_current(e.data.local_delivery) || (e.data.type !== "mainthread__process_encrypted_channel_chat_file_data" && e.data.type !== "mainthread__process_encrypted_channel_chat_picture_data")))
    {
        return;
    }
    if (DBG_WORKER_BOOT_LOG)
    {
        console.log("[dpw] got: " + (e.data && e.data.type));
    }
    if (e.data.type == "mainthread__connect_socket_port")
    {
        workers__stop_keepalive();
        dpw_channel_key_request = null;
        if (dpw_socket_port != null)
        {
            dpw_socket_port.close();
        }
        dpw_socket_port = e.data.port;
        dpw_connection_generation = e.data.connection_generation;
        chat_file_keys_by_file_id = {};
        dpw_connection_open = true;
        dpw_received_bytes = 0;
        g_is_authenticated = false;
        dpw_channel_keys_need_refresh = true;
        dpw_channel_members.clear();
        dpw_local_client_id = -1;
        dpw_current_channel_id = -1;
        dpw_channel_maintainer_id = -1;
        dpw_maintainer_generation = -1;
        g_server_policy.is_connection_check_enforced = true;
        dpw_socket_port.onmessage = workers__socket_port_onmessage;
    }
    else if (e.data.type == "mainthread__set_frame_forwarding")
    {
        dpw_forward_frames = (e.data.value == true);
    }
    else if (e.data.type == "mainthread__set_loopback_mode")
    {
        if (g_loopback_port !== e.data.value)
        {
            workers__cancel_local_transfers("endpoint_replaced");
        }
        // direct messages already have connection encryption when they leave this worker
        g_loopback_port = e.data.value;
    }
    else if (e.data.type == "mainthread__metadata_keys")
    {
        if (e.data.connection_generation != null && e.data.connection_generation !== dpw_connection_generation)
        {
            return;
        }
        g_metadata_keys = [];
        g_metadata_keys.length = 0;
        g_metadata_keys = e.data.value;
        workers__post_to_main({
            type: "data_processing_worker__metadata_keys_accepted",
            value: null
        });
        // the server may reply immediately. install its new keys before sending the login response
        if (dpw_socket_port != null && dpw_connection_open && e.data.send_after_update != null)
        {
            dpw_socket_port.postMessage(
            {
                type: "send",
                value: e.data.send_after_update,
                connection_generation: dpw_connection_generation
            });
        }
    }
    else if (e.data.type == "mainthread__generate_rsa_keypair")
    {
        utils__custom_log("generating random public/private key pair");
        if (DBG_WORKER_BOOT_LOG)
        {
            console.log("[keygen] starting generateRSAKey");
        }

        /**
         * @brief creates an unbiased alphanumeric identity seed using secure random bytes
         *
         * @param number length -> required character count
         *
         * @return string -> random seed
         */
        const randomstring = (length = 8) =>
        {
            const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
            const random_bytes = new Uint8Array(256);
            const accepted_limit = 256 - (256 % chars.length);
            let str = '';
            while (str.length < length)
            {
                crypto.getRandomValues(random_bytes);
                for (const value of random_bytes)
                {
                    if (value < accepted_limit)
                    {
                        str += chars.charAt(value % chars.length);
                        if (str.length === length)
                        {
                            break;
                        }
                    }
                }
            }
            random_bytes.fill(0);
            return str;
        };

        let identity_passphrase_string = "";

        // workers have separate globals, so the request must include the selected size
        const requested_rsa_key_bits = e.data.rsa_key_bits;
        let generated_key = null;
        let generated_public_key = null;

        try
        {
            if (G_ALLOWED_RSA_KEY_BITS.indexOf(requested_rsa_key_bits) < 0)
            {
                throw new Error("Unsupported identity key size");
            }
            if (e.data.from_identity_string === true)
            {
                if (typeof e.data.identity_passphrase_string !== "string" || e.data.identity_passphrase_string.length < 199)
                {
                    throw new Error("Incomplete identity passphrase");
                }
                identity_passphrase_string = e.data.identity_passphrase_string;
            }
            else
            {
                identity_passphrase_string = randomstring(200);
            }
            generated_key = lemon_crypto.generateRSAKey(identity_passphrase_string, requested_rsa_key_bits);
            generated_public_key = lemon_crypto.publicKeyString(generated_key);
        }
        catch (err)
        {
            // neither missing secure randomness nor key generation failure may select a weaker identity
            utils__custom_log("Identity generation failed; no replacement identity was installed.");
            workers__post_to_main(
            {
                type: "data_processing_worker__generate_rsa_keypair_failed",
                identity_request_id: e.data.identity_request_id,
                identity_revision: e.data.identity_revision,
                rsa_key_bits: requested_rsa_key_bits
            });
            return;
        }
        // do not expose a new private key paired with the previous public key if serialization fails
        g_my_rsa_key_object = generated_key;
        g_rsa_public_key_string = generated_public_key;

        workers__post_to_main({
            type: "data_processing_worker__generate_rsa_keypair_result",
            value: g_rsa_public_key_string,
            identity_string: identity_passphrase_string,
            identity_request_id: e.data.identity_request_id,
            identity_revision: e.data.identity_revision,
            rsa_key_bits: requested_rsa_key_bits
        });

        utils__custom_log("Identity key generation finished.");

    }
    else if (e.data.type == "mainthread__process_received_websocket_message")
    {
        workers__mainthread__process_received_websocket_message_continue(e);
    }
    else if (e.data.type == "mainthread_reset_data")
    {
        workers__cancel_local_transfers("session_reset");
        workers__stop_keepalive();
        dpw_connection_open = false;
        dpw_channel_key_request = null;
        if (e.data.preserve_channel_keys !== true)
        {
            keys__clear_channel_keys();
        }
        g_is_authenticated = false;
        dpw_channel_keys_need_refresh = true;
        dpw_channel_members.clear();
        dpw_local_client_id = -1;
        dpw_current_channel_id = -1;
        dpw_channel_maintainer_id = -1;
        dpw_maintainer_generation = -1;
    }
    // encrypt the conversation close notice for its recipient
    else if (e.data.type == "mainthread__create_direct_chat_closed_message")
    {
        let notice_keys = keys__create_random_message_keys();
        let notice_data = keys__seal_direct_content(e.data.public_key, notice_keys,
            keys__encrypt_direct_content(notice_keys, JSON.stringify({ type: "direct_chat_closed" })));
        if (notice_data == null)
        {
            return;
        }

        // build the message
        let notice_message_object = {
            message: {
                type: "direct_chat_message",
                purpose: "conversation",
                value: JSON.stringify(notice_data),
                receiver_type: null,
                receiver_id: parseInt(e.data.receiver_id),
                local_message_id: 1
            }
        };

        // send the message
        workers__post_to_main({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: keys__encrypt_all_message_data_and_convert_to_base64(JSON.stringify(notice_message_object))
        });
    }
    // encrypt the read receipt so only the original sender can read it
    else if (e.data.type == "mainthread__create_seen_receipt_message")
    {
        let receipt_keys = [];

        for (let x = 0; x < 4; x++)
        {
            let single_key = { key_bytes: [], iv_bytes: [] };

            for (let ii = 0; ii < 32; ii++)
            {
                single_key.key_bytes.push(crypto.getRandomValues(new Uint8Array(1))[0]);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                single_key.iv_bytes.push(crypto.getRandomValues(new Uint8Array(1))[0]);
            }

            receipt_keys.push(single_key);
        }

        let receipt_value_object = {
            type: "message_seen",
            value: "" + e.data.server_chat_message_id
        };

        let receipt_data = keys__seal_direct_content(e.data.public_key, receipt_keys, keys__encrypt_direct_content(receipt_keys, JSON.stringify(receipt_value_object)));
        if (receipt_data == null)
        {
            return;
        }

        let receipt_message_object = {
            message: {
                type: "direct_chat_message",
                purpose: "seen_receipt",
                value: JSON.stringify(receipt_data),
                receiver_type: null,
                receiver_id: parseInt(e.data.receiver_id),
                local_message_id: 1
            }
        };

        workers__post_to_main({
            type: "data_processing_worker__seen_receipt_message_result",
            seen_receipt_message_content: keys__encrypt_all_message_data_and_convert_to_base64(JSON.stringify(receipt_message_object))
        });
    }
    else if (e.data.type == "mainthread__cancel_channel_key_creation")
    {
        dpw_channel_key_request = null;
    }
    else if (e.data.type == "mainthread__create_websocket_channel_keys_message")
    {
        workers__create_channel_keys(e.data).catch(function(error)
        {
            console.error("channel key creation failed", error);
            workers__post_to_main(
            {
                type: "data_processing_worker__channel_key_creation_failed",
                generation: e.data.generation,
                channel_id: e.data.channel_id
            });
        });
    }
    else if (e.data.type == "mainthread__create_direct_chat_message")
    {
        let current_message_keys = [];

        for (let i = 0; i < 4; i++)
        {
            let single_key = {
                key_bytes: [],
                iv_bytes: []
            };

            for (let ii = 0; ii < 32; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.key_bytes.push(single_byte);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.iv_bytes.push(single_byte);
            }

            current_message_keys.push(single_key);
        }

        let message_value_object = {
            type: "direct_chat_message",
            value: e.data.chat_message_value
        };

        let message_data = keys__seal_direct_content(e.data.receiver_public_key, current_message_keys, keys__encrypt_direct_content(current_message_keys, JSON.stringify(message_value_object)));
        if (message_data == null)
        {
            return;
        }

        let message_object = {
            message: {
                type: null,
                value: null,
                receiver_id: null,
                local_message_id: e.data.local_message_id
            }
        };

        message_object.message.type = "direct_chat_message";
        message_object.message.purpose = "conversation";
        message_object.message.value = JSON.stringify(message_data);
        message_object.message.receiver_id = parseInt(e.data.chat_message_receiver_id);

        let message_json_string = connection__process_message_before_sending(message_object);

        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        workers__post_to_main({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__create_offline_chat_message")
    {
        // use the same direct encryption for an offline recipient. the server stores only ciphertext
        let current_message_keys = [];

        for (let i = 0; i < 4; i++)
        {
            let single_key = {
                key_bytes: [],
                iv_bytes: []
            };

            for (let ii = 0; ii < 32; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.key_bytes.push(single_byte);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.iv_bytes.push(single_byte);
            }

            current_message_keys.push(single_key);
        }

        let message_value_object = {
            type: "direct_chat_message",
            value: e.data.chat_message_value
        };

        let message_data = keys__seal_direct_content(e.data.receiver_public_key, current_message_keys, keys__encrypt_direct_content(current_message_keys, JSON.stringify(message_value_object)));
        if (message_data == null)
        {
            return;
        }

        let message_object = {
            message: {
                type: "offline_chat_message",
                value: JSON.stringify(message_data),
                recipient_identity_hash: e.data.recipient_identity_hash
            }
        };

        let message_json_string = connection__process_message_before_sending(message_object);

        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        workers__post_to_main({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__create_direct_chat_picture")
    {
        workers__prepare_picture_upload(e.data, true);
    }
    else if (e.data.type == "mainthread__create_channel_chat_picture")
    {
        workers__prepare_picture_upload(e.data, false);
    }
    else if (e.data.type == "mainthread__create_channel_chat_message")
    {
        let message_object = {
            message: {
                type: "channel_chat_message",
                value: "",
                receiver_id: parseInt(e.data.current_channel_id),
                local_message_id: e.data.local_message_id // correlate the receipt with the local message ID, separate from the server's ID
            }
        };

        message_object.message.value = keys__seal_channel_content(e.data.current_channel_keys, keys__encrypt_with_aes_keys_and_convert_to_base64(e.data.current_channel_keys, e.data.chat_message_to_send_value));
        if (message_object.message.value == null)
        {
            return;
        }

        let message_json_string = connection__process_message_before_sending(message_object);
        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        workers__post_to_main({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_direct_chat_picture_data")
    {
        let decrypted_base64_picture_sanitized = "";
        try
        {
            const message_data = JSON.parse(e.data.message_raw);
            const direct_content = keys__open_direct_content(message_data, g_my_rsa_key_object);
            if (direct_content != null)
            {
                const picture = keys__decrypt_direct_content(direct_content.keys, direct_content.value);
                decrypted_base64_picture_sanitized = chat__sanitize_image_data_url(picture);
            }
        }
        catch (error)
        {
            decrypted_base64_picture_sanitized = "";
        }
        if (!decrypted_base64_picture_sanitized)
        {
            workers__post_picture_decrypt_failure(e.data.picture_id, e.data);
            return;
        }

        let msg = {
            message : {
                picture_id: e.data.picture_id,
                client_id: e.data.sender_id,
                value: null,
                decrypted_base64_picture: ""
            }
        };

        msg.message.decrypted_base64_picture = decrypted_base64_picture_sanitized;

        // Node sends the decrypted picture to the page
        if (dpw_forward_frames == true)
        {
            workers__post_to_main({ type: "decrypted_frame", value: JSON.stringify({
                message: {
                    type: "direct_chat_picture_decrypted",
                    picture_id: e.data.picture_id,
                    client_id: e.data.sender_id,
                    decrypted_base64_picture: decrypted_base64_picture_sanitized
                }
            }) });
        }

        workers__post_to_main({
            type: "data_processing_worker__direct_chat_picture",
            value: msg
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_channel_chat_picture_data")
    {
        let picture = "";
        try
        {
            picture = chat__sanitize_image_data_url(keys__decrypt_channel_contents(e.data.message_raw, true));
        }
        catch (error)
        {
            picture = "";
        }
        if (!picture)
        {
            workers__post_picture_decrypt_failure(e.data.picture_id, e.data);
            return;
        }
        if (dpw_forward_frames === true)
        {
            workers__post_to_main(
            {
                type: "decrypted_frame",
                value: JSON.stringify({message:
                {
                    type: "channel_chat_picture_decrypted",
                    picture_id: e.data.picture_id,
                    client_id: e.data.sender_id,
                    decrypted_base64_picture: picture
                }})
            });
        }
        workers__post_attachment_result(
        {
            type: "data_processing_worker__channel_chat_picture",
            value:
            {
                message:
                {
                    picture_id: e.data.picture_id,
                    client_id: e.data.sender_id,
                    value: null,
                    decrypted_base64_picture: picture
                }
            }
        }, e.data);
    }
    else if (e.data.type == "mainthread__create_direct_chat_file")
    {
        // use separate keys for the header and body to avoid repeating a CTR keystream
        // pad the body above the server's minimum upload size
        let file_header = chat_files__create_direct_chat_file_envelope(e.data.receiver_public_key, chat_files__build_chat_file_header_json(e.data.file));
        let data_for_upload_process = chat_files__create_direct_chat_file_envelope(e.data.receiver_public_key, chat_files__build_chat_file_body_json(e.data.file), CHAT_FILE_BODY_MIN_PADDED_BYTES);

        if (file_header == null || data_for_upload_process == null)
        {
            workers__post_to_main({
                type: "data_processing_worker__chat_file_send_failed",
                local_message_id: e.data.local_message_id,
                reason: "could not encrypt the file for this receiver"
            });
            return;
        }

        workers__post_to_main({
            type: "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts",
            data_for_upload_process: data_for_upload_process,
            extra_data: {
                receiver_id: parseInt(e.data.chat_message_receiver_id),
                local_message_id: e.data.local_message_id,
                file_header: file_header
            }
        });
    }
    else if (e.data.type == "mainthread__create_channel_chat_file")
    {
        let file_header = keys__seal_channel_content(e.data.current_channel_keys, chat_files__encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, chat_files__build_chat_file_header_json(e.data.file)));
        let data_for_upload_process = keys__seal_channel_content(e.data.current_channel_keys, chat_files__encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, chat_files__build_chat_file_body_json(e.data.file), CHAT_FILE_BODY_MIN_PADDED_BYTES));

        if (file_header == null || data_for_upload_process == null)
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__chat_file_send_failed",
                local_message_id: e.data.local_message_id,
                reason: "could not encrypt the file for this channel"
            });
            return;
        }

        workers__post_to_main({
            type: "data_processing_worker__channel_chat_file_to_be_uploaded_by_parts",
            data_for_upload_process: data_for_upload_process,
            extra_data: {
                receiver_id: parseInt(e.data.current_channel_id),
                local_message_id: e.data.local_message_id,
                file_header: file_header
            }
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_direct_chat_file_data")
    {
        let file = null;
        try
        {
            file = chat_files__parse_decrypted_chat_file_body(chat_files__open_direct_chat_file_envelope(e.data.message_raw));
        }
        catch (error)
        {
            file = null;
        }
        if (file == null)
        {
            workers__post_file_decrypt_failure(e.data.file_id, e.data);
            return;
        }

        let msg = {
            message: {
                file_id: e.data.file_id,
                client_id: e.data.sender_id,
                file: file
            }
        };

        // Node sends the decrypted file to the page
        if (dpw_forward_frames == true)
        {
            workers__post_to_main({ type: "decrypted_frame", value: JSON.stringify({
                message: {
                    type: "direct_chat_file_decrypted",
                    file_id: e.data.file_id,
                    client_id: e.data.sender_id,
                    file: file
                }
            }) });
        }

        workers__post_to_main({
            type: "data_processing_worker__direct_chat_file",
            value: msg
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_channel_chat_file_data")
    {
        let file = null;
        try
        {
            file = chat_files__decrypt_channel_chat_file_body(e.data.file_id, e.data.message_raw);
        }
        catch (error)
        {
            file = null;
        }
        if (file == null)
        {
            workers__post_file_decrypt_failure(e.data.file_id, e.data);
            return;
        }

        if (dpw_forward_frames === true)
        {
            workers__post_to_main(
            {
                type: "decrypted_frame",
                value: JSON.stringify({message:
                {
                    type: "channel_chat_file_decrypted",
                    file_id: e.data.file_id,
                    client_id: e.data.sender_id,
                    file: file
                }})
            });
        }
        workers__post_attachment_result({
            type: "data_processing_worker__channel_chat_file",
            value: {
                message: {
                    file_id: e.data.file_id,
                    client_id: e.data.sender_id,
                    file: file
                }
            }
        }, e.data);
    }
}

/**
 * @brief reads the key size rejection allowed before encrypted transport is established
 *
 * @param string raw_value -> the frame as it arrived on the socket
 *
 * @return number|null the minimum rsa key bits, null for any other frame
 */
function workers__try_read_plaintext_rsa_key_notice(raw_value)
{
    if (typeof raw_value !== "string" || raw_value.indexOf("rsa_key_too_weak") < 0)
    {
        return null;
    }

    let parsed = null;

    try
    {
        parsed = JSON.parse(raw_value);
    }
    catch (parse_error)
    {
        return null;
    }

    if (parsed == null || parsed.message == null || parsed.message.type !== "rsa_key_too_weak")
    {
        return null;
    }
    return parsed.message.minimum_rsa_key_bits;
}

/**
 * @brief decodes an incoming socket frame and dispatches its server message
 *
 * @param object e -> the worker message event. E.data.value is the frame, e.data.is_plaintext marks a loopback frame
 *
 * @return void
 */
function workers__mainthread__process_received_websocket_message_continue(e)
{
    dpw_local_delivery_scope = null;
    try
    {
        workers__process_received_frame(e);
        const delivery = dpw_local_delivery_scope;
        dpw_local_delivery_scope = null;
        if (delivery != null && delivery.outputs > 0 && delivery.deferred !== true)
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__local_delivery_accepted",
                value: delivery.value
            });
        }
    }
    finally
    {
        dpw_local_delivery_scope = null;
    }
}

/**
 * @brief cancels unfinished local transfers before their scope is replaced
 *
 * @param string reason -> failure reason for the affected transfers
 *
 * @return void
 */
function workers__cancel_local_transfers(reason)
{
    if (dpw_local_transfer_receiver != null)
    {
        dpw_local_transfer_receiver.cancelAll(reason);
    }
}

/**
 * @brief creates the scoped local receiver when needed and passes it a transfer message
 *
 * @param object message -> local begin, chunk or cancel message
 *
 * @return void
 */
function workers__receive_local_transfer(message)
{
    if (dpw_local_transfer_receiver == null)
    {
        dpw_local_transfer_receiver = local_transfer__create_receiver(
        {
            getScope: function()
            {
                return {session_id: dpw_local_session_id, generation: dpw_connection_generation};
            },
            setTimer: setTimeout,
            clearTimer: clearTimeout,
            onAck: function(acknowledgement)
            {
                workers__post_to_main(
                {
                    type: "data_processing_worker__local_transfer_ack",
                    value: {message: acknowledgement}
                });
            },
            onFailure: function(metadata, reason)
            {
                if (metadata == null)
                {
                    return;
                }
                const message = {reason: metadata.content_type.endsWith("_file_decrypted") ? "could not receive this file" : "could not receive this picture"};
                message[metadata.key] = metadata.id;
                workers__post_to_main(
                {
                    type: metadata.content_type.endsWith("_file_decrypted") ? "data_processing_worker__chat_file_decrypt_failed" : "data_processing_worker__chat_picture_decrypt_failed",
                    value: {message: message}
                });
            },
            onFrame: function(text, frame)
            {
                workers__process_received_frame({data: {is_plaintext: true, value: text}}, frame);
            }
        });
    }
    dpw_local_transfer_receiver.receive(message);
}

/**
 * @brief validates frame origin before processing protocol or trusted local content
 *
 * @param object e -> socket event with frame bytes and local origin flag
 * @param object|undefined parsed_local_frame -> already parsed local frame when supplied by the transfer receiver
 *
 * @return void
 */
function workers__process_received_frame(e, parsed_local_frame)
{
    const trusted_loopback = g_loopback_port > 0 && e.data.is_plaintext === true;
    if (e.data.is_plaintext === true && !trusted_loopback)
    {
        return;
    }
    if (e.data.is_plaintext != true)
    {
        let announced_minimum_bits = workers__try_read_plaintext_rsa_key_notice(e.data.value);

        if (announced_minimum_bits != null)
        {
            workers__post_to_main({
                type: "data_processing_worker__rsa_key_too_weak",
                minimum_rsa_key_bits: announced_minimum_bits
            });
            return;
        }
    }

    // only local socket frames may skip connection decryption
    let decrypted_metadata = (e.data.is_plaintext == true) ? e.data.value : keys__decrypt_message_metadata(e.data.value);

    let msg = null;

    try
    {
        msg = trusted_loopback && parsed_local_frame != null ? parsed_local_frame : JSON.parse(decrypted_metadata);
    }
    catch (Exception)
    {
        return;
    }
    if (msg == null || typeof msg !== "object" || Array.isArray(msg) || msg.message == null || typeof msg.message !== "object" || Array.isArray(msg.message) || typeof msg.message.type !== "string" || msg.message.type.length === 0)
    {
        return;
    }

    // reject remote claims of local origin before forwarding any frame to the page
    if (!trusted_loopback && (msg.local_delivery != null || msg.local_sender != null || msg.message.type.startsWith("local_") || msg.message.type.startsWith("loopback_") || msg.message.type === "direct_chat_file_decrypted" || msg.message.type === "direct_chat_picture_decrypted" || msg.message.type === "channel_chat_file_decrypted" || msg.message.type === "channel_chat_picture_decrypted" || msg.message.type === "chat_file_decrypt_failed" || msg.message.type === "chat_picture_decrypt_failed" || msg.message.some_json != null || msg.message.file_header_decrypted != null || msg.message.decrypted_base64_picture != null || msg.message.decrypted_value != null))
    {
        return;
    }
    if (trusted_loopback && (msg.message.type === "local_transfer_begin" || msg.message.type === "local_transfer_chunk" || msg.message.type === "local_transfer_cancel"))
    {
        workers__receive_local_transfer(msg.message);
        return;
    }
    if (trusted_loopback && msg.local_delivery != null)
    {
        const delivery = msg.local_delivery;
        if (!workers__is_local_delivery_current(delivery))
        {
            return;
        }
        dpw_local_delivery_scope = {value: {id: delivery.id, session_id: delivery.session_id}, outputs: 0};
    }
    if (msg.local_sender != null && (!trusted_loopback || !workers__is_local_delivery_current(msg.local_delivery)))
    {
        return;
    }

    if (trusted_loopback && msg.message.type === "local_session_state")
    {
        const state = msg.message.value || msg.message;
        if (typeof state.public_key_string !== "string" || !state.public_key_string)
        {
            return;
        }
        if (typeof state.session_id === "string" && dpw_local_session_id !== state.session_id)
        {
            workers__data_processing_worker_onmessage({data: {type: "mainthread_reset_data"}});
            dpw_local_session_id = state.session_id;
        }
        g_rsa_public_key_string = state.public_key_string;
        g_identity_revision = Number.isSafeInteger(state.identity_revision) ? state.identity_revision : 0;
        workers__post_to_main(
        {
            type: "data_processing_worker__local_session_state",
            value: state
        });
        return;
    }
    if (trusted_loopback && msg.message.type === "local_identity_result")
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__local_identity_result",
            value: msg.message.value || msg.message
        });
        return;
    }
    if (trusted_loopback && msg.message.type === "local_peer_state")
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__local_peer_state",
            value: msg.message.value || msg.message
        });
        return;
    }
    if (trusted_loopback && msg.message.type === "local_upload_result")
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__local_upload_result",
            value: msg.message.value || msg.message
        });
        return;
    }
    if (trusted_loopback && msg.message.type === "local_command_result")
    {
        workers__post_to_main(
        {
            type: "data_processing_worker__local_command_result",
            value: msg.message.value || msg.message
        });
        return;
    }

    // finish accepted local transfers even while recovery pauses authentication
    if (trusted_loopback && (msg.message.type === "chat_file_decrypt_failed" || msg.message.type === "chat_picture_decrypt_failed"))
    {
        workers__post_to_main(
        {
            type: msg.message.type === "chat_file_decrypt_failed" ? "data_processing_worker__chat_file_decrypt_failed" : "data_processing_worker__chat_picture_decrypt_failed",
            value: msg
        });
        return;
    }
    if (trusted_loopback && (msg.message.type === "direct_chat_file_decrypted" || msg.message.type === "channel_chat_file_decrypted"))
    {
        workers__post_to_main(
        {
            type: msg.message.type === "direct_chat_file_decrypted" ? "data_processing_worker__direct_chat_file" : "data_processing_worker__channel_chat_file",
            value:
            {
                message:
                {
                    file_id: msg.message.file_id,
                    client_id: msg.message.client_id,
                    file: msg.message.file
                }
            }
        });
        return;
    }
    if (trusted_loopback && (msg.message.type === "direct_chat_picture_decrypted" || msg.message.type === "channel_chat_picture_decrypted"))
    {
        workers__post_to_main(
        {
            type: msg.message.type === "direct_chat_picture_decrypted" ? "data_processing_worker__direct_chat_picture" : "data_processing_worker__channel_chat_picture",
            value:
            {
                message:
                {
                    picture_id: msg.message.picture_id,
                    client_id: msg.message.client_id,
                    value: null,
                    decrypted_base64_picture: msg.message.decrypted_base64_picture
                }
            }
        });
        return;
    }

    // any valid server message proves the connection is alive. heartbeats still measure RTT
    if (g_is_authenticated)
    {
        dpw_keepalive_last_reply = performance.now();
        if (dpw_socket_port == null)
        {
            workers__post_to_main({type: "data_processing_worker__connection_activity"});
        }
    }

    // keep repeated WebRTC negotiation messages out of the bounded log
    if (msg.message.type != "ice_candidate" && msg.message.type != "sdp_offer" && msg.message.type != "connection_check_response")
    {
        utils__custom_log('[R] msg.message.type : ' + msg.message.type);
    }

    // forward private content only after Node has decrypted it
    if (dpw_forward_frames == true && msg.message.type != "direct_chat_message" && msg.message.type != "offline_chat_message" && msg.message.type != "direct_chat_file_metadata" && msg.message.type != "channel_chat_message" && msg.message.type != "channel_chat_file_metadata" && msg.message.type != "file_receive_completed")
    {
        workers__post_to_main({ type: "decrypted_frame", value: decrypted_metadata });
    }

    // display local connection status before authentication completes
    if (msg.message.type == "loopback_status")
    {
        const state = msg.message.value;
        if (state != null && (state.state === "idle" || state.state === "error"))
        {
            // reset here before the next login burst can reach this worker
            workers__data_processing_worker_onmessage({data: {type: "mainthread_reset_data"}});
            dpw_local_session_id = "";
        }
        else if (state != null && state.state === "connecting")
        {
            g_is_authenticated = false;
        }
        workers__post_to_main({
            type: "data_processing_worker__loopback_status",
            value: msg.message.value
        });
        return;
    }

    // apply a login replay before checking the previous authentication state
    if (msg.message.type == "authentication_status")
    {
        if (msg.message.value == "success")
        {
            g_is_authenticated = true;
            workers__apply_keepalive_policy(msg.message);
            workers__apply_channel_key_policy(msg.message);

            // let the policy handler select fields from the full message
            workers__post_to_main({
                type: "data_processing_worker__authentication_status",
                is_idle_mode_allowed: msg.message.is_idle_mode_allowed,
                policy: msg.message,
                value: "success"
            });

            if (dpw_socket_port != null && dpw_connection_open && dpw_keepalive_timer == null)
            {
                dpw_keepalive_last_reply = performance.now();
                workers__send_keepalive();
                dpw_keepalive_timer = setInterval(workers__send_keepalive, g_connection_check.interval_ms);
            }

            // keep the audio data channel for voice or bot playback. microphone permission is separate
            if (msg.message.is_voice_chat_active == true || msg.message.is_music_bot_audio_active == true)
            {
                workers__post_to_main({
                    type: "data_processing_worker__audio_enabled",
                    value: true,
                    client_voice_allowed: msg.message.is_voice_chat_active == true,
                    stun_port: msg.message.stun_port
                });
            }
        }
        return;
    }

    // the resume response arrives before authentication_status
    if (msg.message.type == "fast_reconnect_ok")
    {
        workers__post_to_main({
            type: "data_processing_worker__fast_reconnect_ok",
            value: msg
        });
        return;
    }

    if (msg.message.type == "datachannel_cooldown")
    {
        workers__post_to_main({
            type: "data_processing_worker__datachannel_cooldown",
            value: msg
        });
        return;
    }

    if (g_is_authenticated == false)
    {
        if (msg.message.type == "crypto_policy")
        {
            workers__post_to_main({ type: "data_processing_worker__crypto_policy", value: msg.message });
        }
        else if (msg.message.type == "public_key_challenge")
        {
            decryption_result = lemon_crypto.challenge_decrypt(msg.message.value, g_my_rsa_key_object); // has to be processed here, the main thread never holds the private key

            msg.message.decryption_result = decryption_result;

            workers__post_to_main({
                type: "data_processing_worker__public_key_challenge_from_server",
                value: msg
            });

        }
        else
        {
            workers__post_to_main({
                type: "data_processing_worker__authentication_status",
                value: "fail"
            });
        }
    }
    else
    {
        workers__update_channel_members(msg.message);
        if (msg.message.type == "connection_check_response")
        {
            if (dpw_socket_port != null)
            {
                dpw_keepalive_last_reply = performance.now();
            }
            workers__post_to_main({
                type: "data_processing_worker__connection_check_response",
                received_at: Date.now(),
                ping_ms: dpw_socket_port == null ? null : Math.round(dpw_keepalive_last_reply - dpw_keepalive_sent_at),
                value: msg
            });
        }
        else if (msg.message.type == "client_list")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_avatar")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_avatar_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "avatar_changed")
        {
            workers__post_to_main({
                type: "data_processing_worker__avatar_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "poke")
        {
            workers__post_to_main({
                type: "data_processing_worker__poke_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "access_denied")
        {
            workers__post_to_main({
                type: "data_processing_worker__access_denied_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_description")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_description_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "force_admin_password_change")
        {
            workers__post_to_main({
                type: "data_processing_worker__force_admin_password_change",
                value: msg
            });
        }
        else if (msg.message.type == "server_settings_values")
        {
            workers__post_to_main({
                type: "data_processing_worker__server_settings_values_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_info" || msg.message.type == "server_banner_result")
        {
            workers__post_to_main({ type: "data_processing_worker__" + msg.message.type, value: msg.message });
        }
        else if (["server_emoji_list", "server_emoji_image", "server_emoji_added", "server_emoji_deleted", "server_emoji_error", "server_settings_saved"].includes(msg.message.type))
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__server_emoji",
                value: msg.message
            });
        }
        else if (msg.message.type == "admin_log")
        {
            workers__post_to_main({
                type: "data_processing_worker__admin_log_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "url_resolved")
        {
            workers__post_to_main({ type: "data_processing_worker__url_resolved", value: msg.message });
        }
        else if (msg.message.type == "admin_status")
        {
            workers__post_to_main({
                type: "data_processing_worker__admin_status",
                value: msg.message
            });
        }
        else if (msg.message.type == "server_policy")
        {
            workers__apply_keepalive_policy(msg.message);
            workers__apply_channel_key_policy(msg.message);
            workers__post_to_main({
                type: "data_processing_worker__server_policy_from_server",
                value: msg
            });
        }
        else if (["moderation_result", "channel_access", "client_context_access", "ban_list", "channel_group_changed"].includes(msg.message.type))
        {
            self.postMessage({ type: "data_processing_worker__moderation", value: msg.message });
        }
        else if (msg.message.type == "client_info")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_info_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_full")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_full_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_list")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_list")
        {
            workers__post_to_main({
                type: "data_processing_worker__icon_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "group_permissions_changed")
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__group_permissions_changed",
                value: msg
            });
        }
        else if (msg.message.type == "group_list")
        {
            workers__post_to_main({
                type: "data_processing_worker__group_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "identity_list")
        {
            workers__post_to_main({
                type: "data_processing_worker__identity_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_add")
        {
            workers__post_to_main({
                type: "data_processing_worker__icon_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "group_add_to_client")
        {
            workers__post_to_main({
                type: "data_processing_worker__group_add_to_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "remove_group_from_client")
        {
            workers__post_to_main({
                type: "data_processing_worker__remove_group_from_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_country_code_changed")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_country_code_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "typing_indicator")
        {
            workers__post_to_main({
                type: "data_processing_worker__typing_indicator_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "stored_clients_list")
        {
            workers__post_to_main({
                type: "data_processing_worker__stored_clients_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "group_add")
        {
            workers__post_to_main({
                type: "data_processing_worker__group_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "group_delete")
        {
            workers__post_to_main({
                type: "data_processing_worker__group_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_delete")
        {
            workers__post_to_main({
                type: "data_processing_worker__icon_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "group_icon_changed")
        {
            workers__post_to_main({
                type: "data_processing_worker__group_icon_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_icon_changed")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_icon_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "start_song_stream")
        {
            workers__post_to_main({
                type: "data_processing_worker__start_song_stream_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "stop_song_stream")
        {
            workers__post_to_main({
                type: "data_processing_worker__stop_song_stream_from_server",
                value: msg
            });
        }
        // video streaming: the six control messages pass through untouched, video.js reads them on the main thread
        else if (msg.message.type == "video_stream_state")
        {
            workers__post_to_main({ type: "data_processing_worker__video_stream_state_from_server", value: msg });
        }
        else if (msg.message.type == "video_stream_offer")
        {
            workers__post_to_main({ type: "data_processing_worker__video_stream_offer_from_server", value: msg });
        }
        else if (msg.message.type == "video_stream_viewer_request")
        {
            workers__post_to_main({ type: "data_processing_worker__video_stream_viewer_request_from_server", value: msg });
        }
        else if (msg.message.type == "video_stream_viewer_state")
        {
            workers__post_to_main({ type: "data_processing_worker__video_stream_viewer_state_from_server", value: msg });
        }
        else if (msg.message.type == "video_stream_keyframe_request")
        {
            workers__post_to_main({ type: "data_processing_worker__video_stream_keyframe_request_from_server", value: msg });
        }
        else if (msg.message.type == "video_stream_refused")
        {
            workers__post_to_main({ type: "data_processing_worker__video_stream_refused_from_server", value: msg });
        }
        else if (msg.message.type == "chat_message_delete")
        {
            workers__post_to_main({
                type: "data_processing_worker__chat_message_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "chat_message_edit")
        {
            workers__post_to_main({
                type: "data_processing_worker__chat_message_edit_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_chat_message_id_for_local_message_id")
        {
            workers__post_to_main({
                type: "data_processing_worker__server_chat_message_id_for_local_message_id_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_join")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_join_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_role_add")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_role_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_connect")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_connect_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_disconnect")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_disconnect_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_rename")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_rename_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "audio_state_of_single_client")
        {
            workers__post_to_main({
                type: "data_processing_worker__audio_state_of_single_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "current_channel_active_microphone_usage")
        {
            workers__post_to_main({
                type: "data_processing_worker__current_channel_active_microphone_usage_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "music_bot_key")
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__music_bot_key",
                value: msg.message
            });
        }
        else if (msg.message.type == "audio_websocket_session")
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__audio_websocket_session",
                value: msg.message
            });
        }
        else if (msg.message.type == "video_websocket_session")
        {
            workers__post_to_main(
            {
                type: "data_processing_worker__video_websocket_session",
                value: msg.message
            });
        }
        else if (msg.message.type == "local_media_keys" && g_loopback_port > 0)
        {
            // accept media keys only from the local Node owner
            workers__post_to_main(
            {
                type: "data_processing_worker__local_media_keys",
                value: msg.message
            });
        }
        else if (msg.message.type == "sdp_offer")
        {
            workers__post_to_main({
                type: "data_processing_worker__sdp_offer_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "ice_candidate")
        {
            workers__post_to_main({
                type: "data_processing_worker__ice_candidate_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_info_broadcast")
        {
            workers__post_to_main({
                type: "data_processing_worker__server_info_broadcast_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_edit")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_edit_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_going_to_idle_mode")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_going_to_idle_mode",
                value: msg
            });
        }
        else if (msg.message.type == "client_coming_back_from_idle_mode")
        {
            workers__post_to_main({
                type: "data_processing_worker__client_coming_back_from_idle_mode",
                value: msg
            });
        }
        else if (msg.message.type == "channel_delete")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_create")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_create_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_picture_metadata")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_chat_picture_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_picture_metadata")
        {
            workers__post_to_main({
                type: "data_processing_worker__direct_chat_picture_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_file_metadata")
        {
            // open the header with worker keys. keep a file card even if its name cannot be decrypted
            if (!trusted_loopback || msg.message.file_header_decrypted === undefined)
            {
                msg.message.file_header_decrypted = (typeof msg.message.file_header === "string") ? chat_files__decrypt_channel_chat_file_header(msg.message.file_id, msg.message.file_header) : null;
                msg.message.file_header = null;
            }
            if (dpw_forward_frames === true)
            {
                workers__post_to_main({type: "decrypted_frame", value: JSON.stringify(msg)});
            }

            workers__post_to_main({
                type: "data_processing_worker__channel_chat_file_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_file_metadata")
        {
            // loopback: node already opened the header, so just pass it on
            if (msg.message.file_header_decrypted == null)
            {
                msg.message.file_header_decrypted = (typeof msg.message.file_header === "string") ? chat_files__parse_chat_file_header(chat_files__open_direct_chat_file_envelope(msg.message.file_header)) : null;
                msg.message.file_header = null;

                // opened shape, so the ui reads it without a key of its own
                if (dpw_forward_frames == true)
                {
                    workers__post_to_main({ type: "decrypted_frame", value: JSON.stringify(msg) });
                }
            }

            workers__post_to_main({
                type: "data_processing_worker__direct_chat_file_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "file_send_error")
        {
            workers__post_to_main({
                type: "data_processing_worker__file_send_error_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_maintainer_id")
        {
            workers__post_to_main({
                type: "data_processing_worker__channel_maintainer_id",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_message")
        {
            let decrypted_value = trusted_loopback && typeof msg.message.decrypted_value === "string" ? msg.message.decrypted_value : keys__decrypt_channel_contents(msg.message.value, false);
            if (decrypted_value != null)
            {
                msg.message.decrypted_value = decrypted_value;
                msg.message.value = null;
                if (dpw_forward_frames === true)
                {
                    workers__post_to_main({type: "decrypted_frame", value: JSON.stringify(msg)});
                }
                workers__post_to_main(
                {
                    type: "data_processing_worker__channel_chat_message",
                    value: msg
                });
            }
        }
        else if (msg.message.type == "direct_chat_message")
        {
            // loopback: node already decrypted this, so just pass it on
            if (msg.message.some_json != null)
            {
                if (msg.message.some_json.type === "channel_keys_from_maintainer" && !workers__receive_channel_keys(msg))
                {
                    return;
                }
                workers__post_to_main({
                    type: "data_processing_worker__direct_chat_message",
                    value: msg
                });
                return;
            }

            let message_data = JSON.parse(msg.message.value);

            let direct_content = keys__open_direct_content(message_data, g_my_rsa_key_object);

            if (direct_content == null)
            {
                utils__custom_log("! failed to decrypt direct chat message !");
                return;
            }

            let decrypted_text = keys__decrypt_direct_content(direct_content.keys, direct_content.value);
            if (decrypted_text == null)
            {
                return;
            }

            let some_json = JSON.parse(decrypted_text);

            if (some_json.type == "direct_chat_message")
            {
                some_json.value = some_json.value; 
            }

            // content that does not match the purpose named by the sender is dropped
            if (workers__is_direct_content_allowed_for_purpose(msg.message.purpose, some_json.type) == false)
            {
                utils__custom_log("! dropped a direct message whose content does not match its purpose !");
                return;
            }

            msg.message.some_json = some_json;
            msg.message.value = null;
            // install keys before the next socket frame, even while the interface is busy
            if (some_json.type === "channel_keys_from_maintainer" && !workers__receive_channel_keys(msg))
            {
                return;
            }

            // decrypted shape, so the ui reads it without a key of its own
            if (dpw_forward_frames == true)
            {
                workers__post_to_main({ type: "decrypted_frame", value: JSON.stringify(msg) });
            }

            workers__post_to_main({
                type: "data_processing_worker__direct_chat_message",
                value: msg
            });
        }
        else if (msg.message.type == "offline_chat_message")
        {
            // loopback: node already decrypted this, so just pass it on
            if (msg.message.some_json != null)
            {
                workers__post_to_main({
                    type: "data_processing_worker__offline_chat_message",
                    value: msg
                });
                return;
            }

            // decrypt queued messages with the same envelope used for live direct messages
            let message_data = JSON.parse(msg.message.value);

            let direct_content = keys__open_direct_content(message_data, g_my_rsa_key_object);

            if (direct_content == null)
            {
                // queued for a different keypair than the one we hold now
                utils__custom_log("! failed to decrypt offline chat message !");
                return;
            }

            let decrypted_text = keys__decrypt_direct_content(direct_content.keys, direct_content.value);
            if (decrypted_text == null)
            {
                return;
            }

            msg.message.some_json = JSON.parse(decrypted_text);
            msg.message.value = null;

            // decrypted shape, so the ui reads it without a key of its own
            if (dpw_forward_frames == true)
            {
                workers__post_to_main({ type: "decrypted_frame", value: JSON.stringify(msg) });
            }

            workers__post_to_main({
                type: "data_processing_worker__offline_chat_message",
                value: msg
            });
        }
        else if (msg.message.type == "image_sent_status")
        {
            workers__post_to_main({
                type: "data_processing_worker__image_sent_status_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "call")
        {
            workers__post_to_main({
                type: "data_processing_worker__call_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "music_bot_song_list")
        {
            workers__post_to_main({
                type: "data_processing_worker__music_bot_song_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_send_success")
        {
            workers__post_to_main({
                type: "data_processing_worker__file_send_success_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_receive_chunk")
        {
            workers__post_to_main({
                type: "data_processing_worker__file_receive_chunk_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_receive_completed")
        {
            if (dpw_local_delivery_scope != null && (msg.message.receive_type === "channel_chat_file" || msg.message.receive_type === "channel_chat_picture"))
            {
                // wait for main thread decryption before acknowledging this local delivery
                dpw_local_delivery_scope.deferred = true;
            }
            workers__post_to_main({
                type: "data_processing_worker__file_receive_completed_from_server",
                value: msg
            });
        }
    }
}

// ---- websocket worker ----

/**
 * @brief sends received frames and connection events to the decryption thread, or to the Android host's existing path
 *
 * @param object message -> a received frame, socket error or close event
 */
function workers__deliver_socket_event(message)
{
    message.connection_generation = websocket_connection_generation;
    if (websocket_data_port != null)
    {
        websocket_data_port.postMessage(message);
    }
    else
    {
        global.postMessage(message);
    }
}

/**
 * @brief completes or cancels the one upload chunk waiting for socket capacity
 *
 * @param boolean sent -> whether this chunk was handed to the current socket
 */
function workers__finish_upload_send(sent)
{
    clearTimeout(websocket_upload_timer);
    websocket_upload_timer = null;
    const pending = websocket_upload_pending;
    websocket_upload_pending = null;
    if (pending != null)
    {
        pending.reply_port.postMessage({sent: sent});
        pending.reply_port.close();
    }
}

/**
 * @brief waits for socket capacity without blocking chat or heartbeat messages
 */
function workers__flush_upload_send()
{
    const pending = websocket_upload_pending;
    if (pending == null)
    {
        return;
    }
    if (pending.generation !== websocket_generation || websocket_instance == null || websocket_instance.readyState !== 1 || performance.now() >= pending.deadline)
    {
        workers__finish_upload_send(false);
        return;
    }
    if (websocket_instance.bufferedAmount > WEBSOCKET_UPLOAD_BUFFER_LIMIT)
    {
        websocket_upload_timer = setTimeout(workers__flush_upload_send, 25);
        return;
    }
    try
    {
        websocket_instance.send(pending.value);
        workers__finish_upload_send(true);
    }
    catch (error)
    {
        console.error("upload socket send failed", error);
        workers__finish_upload_send(false);
    }
}

/**
 * @brief handles socket creation, sending and closure requests
 *
 * @param object e -> the worker message event. E.data.type picks the branch
 *
 * @return void
 */
function workers__websocket_worker_onmessage(e)
{

    if (e.data.type == "create_websocket_object")
    {
        workers__finish_upload_send(false);
        let generation = ++websocket_generation;

        // a replaced socket is closed, and its events below are ignored as stale
        if (websocket_instance != null)
        {
            try
            {
                websocket_instance.close();
            }
            catch (close_error)
            {
            }
        }

        if (websocket_data_port != null)
        {
            websocket_data_port.close();
        }
        websocket_data_port = e.data.port || null;
        websocket_connection_generation = e.data.connection_generation;
        websocket_close_reason = "";
        if (websocket_data_port != null)
        {
            websocket_data_port.onmessage = function(port_event)
            {
                if (generation === websocket_generation && port_event.data.connection_generation === websocket_connection_generation)
                {
                    workers__websocket_worker_onmessage(port_event);
                }
            };
        }

        websocket_instance = new WebSocket(e.data.value.connection_string);

        websocket_instance.addEventListener('message', function (event)
        {
            if (generation != websocket_generation)
            {
                return;
            }

            workers__deliver_socket_event({
                type: "websocket_worker_onmessage",
                value: event.data
            });
        });

        websocket_instance.onclose = function (event)
        {
            if (generation != websocket_generation)
            {
                return;
            }
            workers__finish_upload_send(false);
            const code = event != null && typeof event.code === "number" ? event.code : 0;
            const peer_reason = event != null && typeof event.reason === "string" ? event.reason : "";
            const was_clean = event != null && event.wasClean === true;
            console.warn("websocket closed", {code: code, reason: peer_reason, wasClean: was_clean});
            workers__deliver_socket_event(
            {
                type: "websocket_worker_onclose",
                reason: websocket_close_reason,
                close_code: code,
                close_reason: peer_reason,
                was_clean: was_clean
            });
        };

        websocket_instance.onerror = function (error)
        {
            if (generation != websocket_generation)
            {
                return;
            }

            // use Node's socket error code when available. browsers may provide only an opaque event
            let error_code = (error != null && typeof error.code === "string") ? error.code : "";

            console.error("websocket error event" + (error_code !== "" ? " (" + error_code + ")" : ""));
            workers__deliver_socket_event({
                type: "websocket_worker_onerror",
                error_code: error_code
            });
        }

        websocket_instance.onopen = function ()
        {
            if (generation != websocket_generation)
            {
                return;
            }

            websocket_instance.send(e.data.value.onopen_data);
        }
    }

    else if (e.data.type == "send")
    {
        if (e.data.connection_generation != null && e.data.connection_generation !== websocket_connection_generation)
        {
            if (e.data.reply_port != null)
            {
                e.data.reply_port.postMessage({sent: false});
                e.data.reply_port.close();
            }
            return;
        }
        if (e.data.reply_port != null)
        {
            // the page waits for this acknowledgement before creating the next chunk
            if (websocket_upload_pending != null)
            {
                e.data.reply_port.postMessage({sent: false});
                e.data.reply_port.close();
                return;
            }
            websocket_upload_pending =
            {
                value: e.data.value,
                reply_port: e.data.reply_port,
                generation: websocket_generation,
                deadline: performance.now() + WEBSOCKET_UPLOAD_WAIT_MS
            };
            workers__flush_upload_send();
            return;
        }
        // the local page can send a request before Node has opened the remote socket
        if (websocket_instance == null)
        {
            console.warn("websocket send skipped: no socket yet");
        }
        else if (websocket_instance.readyState === 1)
        {
            websocket_instance.send(e.data.value);
        }
        else
        {
            console.warn("websocket send skipped: socket not open (readyState " + websocket_instance.readyState + ")");
        }
    }

    else if (e.data.type == "cancel_upload")
    {
        if (e.data.connection_generation == null || e.data.connection_generation === websocket_connection_generation)
        {
            workers__finish_upload_send(false);
        }
    }
    else if (e.data.type == "close")
    {
        if (e.data.connection_generation != null && e.data.connection_generation !== websocket_connection_generation)
        {
            return;
        }
        workers__finish_upload_send(false);
        websocket_close_reason = e.data.reason || "";
        // let onclose perform the normal reset after a deliberate disconnect
        if (websocket_instance != null)
        {
            websocket_instance.close();
        }
    }
}
