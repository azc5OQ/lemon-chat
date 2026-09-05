// workers.js is embedded in template.html along with the other client files, and in the node bundle
// it holds the two worker handlers, each running inside its worker only: the data-processing worker
// (the rsa keypair, decrypting what the socket delivers, encrypting what the page sends) and the
// websocket worker (the socket to the server, frames relayed both ways)
// worker-entry.js picks the handler; connection.js posts to them, dispatch.js receives what they post

// state private to this file
// the server refuses uploads of 4096 base64 characters or less, so file bodies are padded to this
// many plaintext bytes (5462 characters encoded); without it a small file would be refused silently
var CHAT_FILE_BODY_MIN_PADDED_BYTES = 4096;

// when on, every decrypted server frame is also posted back raw (node uses it for the ui replay)
var dpw_forward_frames = false;

var websocket_instance = null;

// counts sockets ever created. events from an old (replaced) socket are ignored
var websocket_generation = 0;

// ---- data-processing worker ----

/**
 * @brief the data-processing worker's message handler
 *        generates the rsa keypair, decrypts what the socket delivers, encrypts what the page sends
 *        (chat messages, pictures, channel keys) and posts every result back to the main thread
 *
 * @param object e -> the worker message event; e.data.type picks the branch
 *
 * @return void
 */
function workers__data_processing_worker_onmessage(e)
{
    if (DBG_WORKER_BOOT_LOG) { console.log("[dpw] got: " + (e.data && e.data.type)); }
    if (e.data.type == "mainthread__set_frame_forwarding")
    {
        dpw_forward_frames = (e.data.value == true);
    }
    else if (e.data.type == "mainthread__set_loopback_mode")
    {
        // the worker builds and transport-encrypts direct messages itself; without this
        // its encrypt_all still wrapped them, node wrapped again, and the server kicked
        // the connection on the resulting garbage
        g_loopback_port = e.data.value;
    }
    else if (e.data.type == "mainthread__metadata_keys")
    {
        g_metadata_keys = [];
        g_metadata_keys.length = 0;
        g_metadata_keys = e.data.value;
        global.postMessage({
            type: "data_processing_worker__metadata_keys_accepted",
            value: null
        });
    }
    else if (e.data.type == "mainthread__generate_rsa_keypair")
    {
        utils__custom_log("generating random public/private key pair");
        if (DBG_WORKER_BOOT_LOG) { console.log("[keygen] starting generateRSAKey"); }

        const randomstring = (length = 8) =>
        {
            // Declare all characters
            let chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';

            // Pick characers randomly
            let str = '';
            for (let i = 0; i < length; i++)
            {
                str += chars.charAt(Math.floor(Math.random() * chars.length));
            }

            return str;

        };

        let identity_passphrase_string = "";

        if (e.data.from_identity_string == true && e.data.identity_passphrase_string != null && e.data.identity_passphrase_string.length > 0)
        {
            identity_passphrase_string = e.data.identity_passphrase_string; // also known as passphrase
        }
        else
        {
            identity_passphrase_string = randomstring(200); // also known as passphrase
        }

        // the size travels with the request: this worker has its own copy of every global,
        // so the main thread's preference does not reach it any other way
        let requested_rsa_key_bits = 2048;
        if (G_ALLOWED_RSA_KEY_BITS.indexOf(e.data.rsa_key_bits) >= 0) { requested_rsa_key_bits = e.data.rsa_key_bits; }

        try
        {
            g_my_rsa_key_object = lemon_crypto.generateRSAKey(identity_passphrase_string, requested_rsa_key_bits);
        }
        catch (err)
        {
            // the wasm module is the only rsa implementation; without it there is no identity
            utils__custom_log("FATAL: rsa keygen wasm failed (" + err + "), cannot create an identity");
            throw err;
        }
        g_rsa_public_key_string = lemon_crypto.publicKeyString(g_my_rsa_key_object);

        utils__custom_log("identity_passphrase_string now in use -> " + identity_passphrase_string);

        global.postMessage({
            type: "data_processing_worker__generate_rsa_keypair_result",
            value: g_rsa_public_key_string,
            identity_string: identity_passphrase_string
        });

        utils__custom_log("public key now in use: " + g_rsa_public_key_string);

    }
    else if (e.data.type == "mainthread__process_received_websocket_message")
    {
        workers__mainthread__process_received_websocket_message_continue(e);
    }
    else if (e.data.type == "mainthread_reset_data")
    {
        g_is_authenticated = false;
    }
    else if (e.data.type == "mainthread__channel_keys_for_data_processing_worker")
    {
        if (g_current_channel_keys != null)
        {
            g_historic_keys_of_current_channel.push(g_current_channel_keys);

            if (g_historic_keys_of_current_channel.length > 7)
            {
                g_historic_keys_of_current_channel.shift();
            }
        }
        g_current_channel_keys = e.data.value;
    }
    // a read receipt, encrypted for the original sender the same way channel keys are.
    // the server never learns what it says: to it this is an ordinary direct message
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

        let receipt_encryption = lemon_crypto.encrypt(JSON.stringify(receipt_keys), e.data.public_key);

        let receipt_value_object = {
            type: "message_seen",
            value: "" + e.data.server_chat_message_id
        };

        let receipt_data = {
            message_keys: receipt_encryption.cipher,
            message_value: keys__encrypt_with_aes_keys_and_convert_to_base64(receipt_keys, JSON.stringify(receipt_value_object))
        };

        let receipt_message_object = {
            message: {
                type: "direct_chat_message",
                value: JSON.stringify(receipt_data),
                receiver_type: null,
                receiver_id: parseInt(e.data.receiver_id),
                local_message_id: 1
            }
        };

        global.postMessage({
            type: "data_processing_worker__seen_receipt_message_result",
            seen_receipt_message_content: keys__encrypt_all_message_data_and_convert_to_base64(JSON.stringify(receipt_message_object))
        });
    }
    else if (e.data.type == "mainthread__create_websocket_channel_keys_message")
    {
        let new_channel_keys = [];

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

            new_channel_keys.push(single_key);
        }

        g_current_channel_keys = new_channel_keys;

        global.postMessage({
            type: "data_processing_worker__new_channel_keys_from_data_processing_worker",
            value: new_channel_keys
        });

        for (let x = 0; x < e.data.clients.length; x++)
        {
            if (e.data.clients[x].channel_id != e.data.current_channel_id)
            {
                continue;
            }
            // under node the maintainer also sends ITSELF a keys message: the server
            // echoes it back and the loopback forwards it, which is how the webview
            // learns the keys. in a browser the local copy suffices, skip as before
            if (e.data.clients[x].client_id == e.data.local_client_id && typeof process === "undefined")
            {
                continue;
            }

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

            let single_message_aes_keys_string = JSON.stringify(current_message_keys);

            let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.clients[x].public_key);

            let message_data = {
                message_keys: encryption_result.cipher,
                message_value: ""
            };

            let message_value_object = {
                type: "channel_keys_from_maintainer",
                value: JSON.stringify(g_current_channel_keys)
            };

            message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

            let message_object = {
                message: {
                    type: null,
                    value: null,
                    receiver_type: null,
                    receiver_id: null,
                    local_message_id: 1 // local_message_id is not used but still needed because channel keys messages are subform of direct messages, and this is processed the same as direct message, this object property is also needed even if not used
                    // because server side processes these type of messages same as direct messages, to prevent server from discarding this as wrong, this field is specified even if not needed
                }
            };

            message_object.message.type = "direct_chat_message";
            message_object.message.value = JSON.stringify(message_data);
            message_object.message.receiver_id = parseInt(e.data.clients[x].client_id);

            let message_json_string = JSON.stringify(message_object);
            let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

            // channel keys message created, its contents encrypted
            // send message data, to be sent over websocket, to main thread

            if (e.data.clients[x].client_id == e.data.local_client_id)
            {
                // our own copy: the server never echoes a direct message back to its
                // sender, so hand the ui the decrypted shape it now expects
                global.postMessage({
                    type: "data_processing_worker__local_channel_keys_for_ui",
                    value: JSON.stringify({
                        message: {
                            type: "direct_chat_message",
                            sender_id: e.data.local_client_id,
                            sender_username: e.data.clients[x].username,
                            value: null,
                            some_json: message_value_object
                        }
                    })
                });
            }
            else
            {
                global.postMessage({
                    type: "data_processing_worker__create_websocket_channel_keys_message_result",
                    channel_keys_message_content: data,
                    username: e.data.clients[x].username
                });
            }
        }
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

        let single_message_aes_keys_string = JSON.stringify(current_message_keys);
        let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.receiver_public_key);

        let message_data = {
            message_keys: encryption_result.cipher,
            message_value: ""
        };

        let message_value_object = {
            type: "direct_chat_message",
            value: e.data.chat_message_value
        };

        message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

        let message_object = {
            message: {
                type: null,
                value: null,
                receiver_id: null,
                local_message_id: e.data.local_message_id
            }
        };

        message_object.message.type = "direct_chat_message";
        message_object.message.value = JSON.stringify(message_data);
        message_object.message.receiver_id = parseInt(e.data.chat_message_receiver_id);

        let message_json_string = connection__process_message_before_sending(message_object);

        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        global.postMessage({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__create_offline_chat_message")
    {
        // identical crypto to a direct message - the recipient is simply not connected,
        // so it is addressed by their registered alias and the server parks the result
        // until they come back. the server sees the same opaque blob either way.
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

        let single_message_aes_keys_string = JSON.stringify(current_message_keys);
        let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.receiver_public_key);

        let message_data = {
            message_keys: encryption_result.cipher,
            message_value: ""
        };

        let message_value_object = {
            type: "direct_chat_message",
            value: e.data.chat_message_value
        };

        message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

        let message_object = {
            message: {
                type: "offline_chat_message",
                value: JSON.stringify(message_data),
                recipient_alias: e.data.recipient_alias
            }
        };

        let message_json_string = connection__process_message_before_sending(message_object);

        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        global.postMessage({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__create_direct_chat_picture")
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

        let single_message_aes_keys_string = JSON.stringify(current_message_keys);

        let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.receiver_public_key);

        // data_for_upload_process are the data server should not be possible to decrypt, server only forwards them to client
        // message_object contains metadata server can decrypt

        let message_data = {
            message_keys: encryption_result.cipher,
            message_value: ""
        };

        message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, e.data.base64_picture_string_to_send);

        let data_for_upload_process = JSON.stringify(message_data);

        let extra_data = {
            receiver_id: parseInt(e.data.chat_message_receiver_id),
            local_message_id: e.data.local_message_id
        };

        global.postMessage({
            type: "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts",
            data_for_upload_process: data_for_upload_process,
            extra_data: extra_data
        });
    }
    else if (e.data.type == "mainthread__create_channel_chat_picture")
    {
        let extra_data = {
            receiver_id: parseInt(e.data.current_channel_id),
            local_message_id: e.data.local_message_id
        };

        let data_for_upload_process = keys__encrypt_with_aes_keys_and_convert_to_base64(e.data.current_channel_keys, e.data.base64_picture_string_to_send);

        global.postMessage({
            type: "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts",
            data_for_upload_process: data_for_upload_process,
            extra_data: extra_data
        });
    }
    else if (e.data.type == "mainthread__create_channel_chat_message")
    {
        let message_object = {
            message: {
                type: "channel_chat_message",
                value: "",
                receiver_id: parseInt(e.data.current_channel_id),
                local_message_id: e.data.local_message_id // server needs this to inform client that message has been received.This is different from message ID stored by server
            }
        };

        message_object.message.value = keys__encrypt_with_aes_keys_and_convert_to_base64(e.data.current_channel_keys, e.data.chat_message_to_send_value);

        let message_json_string = connection__process_message_before_sending(message_object);
        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        global.postMessage({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_direct_chat_picture_data")
    {
        let message_data = JSON.parse(e.data.message_raw);
        let decryption_result = lemon_crypto.decrypt(message_data.message_keys, g_my_rsa_key_object);

        if (decryption_result.status == 'failure')
        {
            log("failed to decrypt direct chat picture");
            return;
        }

        let current_message_keys = JSON.parse(decryption_result.plaintext);
        let decrypted_base64_picture = keys__decrypt_base64_contents_with_aes_keys(current_message_keys, message_data.message_value);
        let decrypted_base64_picture_sanitized = chat__sanitize_image_data_url(decrypted_base64_picture);

        let msg = {
            message : {
                picture_id: e.data.picture_id,
                client_id: e.data.sender_id,
                value: null,
                decrypted_base64_picture: ""
            }
        };

        msg.message.decrypted_base64_picture = decrypted_base64_picture_sanitized;

        // loopback: the ui has no key for this, so send it the finished picture
        if (dpw_forward_frames == true)
        {
            global.postMessage({ type: "decrypted_frame", value: JSON.stringify({
                message: {
                    type: "direct_chat_picture_decrypted",
                    picture_id: e.data.picture_id,
                    client_id: e.data.sender_id,
                    decrypted_base64_picture: decrypted_base64_picture_sanitized
                }
            }) });
        }

        global.postMessage({
            type: "data_processing_worker__direct_chat_picture",
            value: msg
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_channel_chat_picture_data")
    {
        if (g_current_channel_keys != null)
        {
            let msg = {
                message : {
                    picture_id: e.data.picture_id,
                    client_id: e.data.sender_id,
                    value: null,
                    decrypted_base64_picture: ""
                }
            }

            let is_decrypted = false;
            let decrypted_base64_picture = keys__decrypt_base64_contents_with_aes_keys(g_current_channel_keys, e.data.message_raw);

            if (decrypted_base64_picture.startsWith("data:image"))
            {
                is_decrypted = true;
            }
            else
            {
                for (i = 0; i < g_historic_keys_of_current_channel.length; i++)
                {
                    decrypted_base64_picture = keys__decrypt_base64_contents_with_aes_keys(g_historic_keys_of_current_channel[i], e.data.message_raw);
                    if (decrypted_base64_picture.startsWith("data:image"))
                    {
                        console.log("managed to decrypt channel chat picture with channel key at" + i);
                        is_decrypted = true;
                        break;
                    }
                }
            }

            if (is_decrypted)
            {
                let decrypted_base64_picture_sanitized = chat__sanitize_image_data_url(decrypted_base64_picture);
                msg.message.decrypted_base64_picture = decrypted_base64_picture_sanitized;
                msg.message.value = null;
                global.postMessage({
                    type: "data_processing_worker__channel_chat_picture",
                    value: msg
                });
            }
        }
        else
        {
            utils__custom_log("channel_chat_picture decrypt failed, data_processing_worker thread missing current channel keys");
        }
    }
    else if (e.data.type == "mainthread__create_direct_chat_file")
    {
        // header and body get their own key sets: two plaintexts must never share a ctr keystream.
        // the body pads up so even a 1 kb file clears the server's minimum upload size
        let file_header = chat_files__create_direct_chat_file_envelope(e.data.receiver_public_key, chat_files__build_chat_file_header_json(e.data.file));
        let data_for_upload_process = chat_files__create_direct_chat_file_envelope(e.data.receiver_public_key, chat_files__build_chat_file_body_json(e.data.file), CHAT_FILE_BODY_MIN_PADDED_BYTES);

        if (file_header == null || data_for_upload_process == null)
        {
            global.postMessage({
                type: "data_processing_worker__chat_file_send_failed",
                local_message_id: e.data.local_message_id,
                reason: "could not encrypt the file for this receiver"
            });
            return;
        }

        global.postMessage({
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
        global.postMessage({
            type: "data_processing_worker__channel_chat_file_to_be_uploaded_by_parts",
            data_for_upload_process: chat_files__encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, chat_files__build_chat_file_body_json(e.data.file), CHAT_FILE_BODY_MIN_PADDED_BYTES),
            extra_data: {
                receiver_id: parseInt(e.data.current_channel_id),
                local_message_id: e.data.local_message_id,
                file_header: chat_files__encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, chat_files__build_chat_file_header_json(e.data.file))
            }
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_direct_chat_file_data")
    {
        let file = chat_files__parse_decrypted_chat_file_body(chat_files__open_direct_chat_file_envelope(e.data.message_raw));

        if (file == null)
        {
            utils__custom_log("failed to decrypt direct chat file");
            global.postMessage({
                type: "data_processing_worker__chat_file_decrypt_failed",
                value: { message: { file_id: e.data.file_id } }
            });
            return;
        }

        let msg = {
            message: {
                file_id: e.data.file_id,
                client_id: e.data.sender_id,
                file: file
            }
        };

        // loopback: the ui has no key for this, so send it the finished file
        if (dpw_forward_frames == true)
        {
            global.postMessage({ type: "decrypted_frame", value: JSON.stringify({
                message: {
                    type: "direct_chat_file_decrypted",
                    file_id: e.data.file_id,
                    client_id: e.data.sender_id,
                    file: file
                }
            }) });
        }

        global.postMessage({
            type: "data_processing_worker__direct_chat_file",
            value: msg
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_channel_chat_file_data")
    {
        let file = chat_files__decrypt_channel_chat_file_body(e.data.file_id, e.data.message_raw);

        if (file == null)
        {
            utils__custom_log("channel_chat_file decrypt failed, no channel key opened it");
            global.postMessage({
                type: "data_processing_worker__chat_file_decrypt_failed",
                value: { message: { file_id: e.data.file_id } }
            });
            return;
        }

        global.postMessage({
            type: "data_processing_worker__channel_chat_file",
            value: {
                message: {
                    file_id: e.data.file_id,
                    client_id: e.data.sender_id,
                    file: file
                }
            }
        });
    }
}

/**
 * @brief the announced minimum key size when a raw frame is the plaintext "your key is too weak" notice
 *        that notice is the only type accepted in the clear, because the server sends it before the
 *        diffie-hellman exchange has produced a shared secret
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

    try { parsed = JSON.parse(raw_value); }
    catch (parse_error) { return null; }

    if (parsed == null || parsed.message == null || parsed.message.type !== "rsa_key_too_weak")
    {
        return null;
    }
    return parsed.message.minimum_rsa_key_bits;
}

/**
 * @brief processes one frame that arrived on the socket
 *        recognises the plaintext key-size notice, decrypts everything else and posts each server
 *        message to the main thread under its data_processing_worker__*_from_server type
 *
 * @param object e -> the worker message event; e.data.value is the frame, e.data.is_plaintext marks a loopback frame
 *
 * @return void
 */
function workers__mainthread__process_received_websocket_message_continue(e)
{
    if (e.data.is_plaintext != true)
    {
        let announced_minimum_bits = workers__try_read_plaintext_rsa_key_notice(e.data.value);

        if (announced_minimum_bits != null)
        {
            global.postMessage({
                type: "data_processing_worker__rsa_key_too_weak",
                minimum_rsa_key_bits: announced_minimum_bits
            });
            return;
        }
    }

    // loopback frames are plaintext; everything downstream is identical
    let decrypted_metadata = (e.data.is_plaintext == true) ? e.data.value : keys__decrypt_message_metadata(e.data.value);

    let msg = null;

    try
    {
        msg = JSON.parse(decrypted_metadata);
    }
    catch (Exception)
    {
        console.log(Exception+ " decrypted string length: " + decrypted_metadata.length + " "+ decrypted_metadata);
    }

    // the webrtc handshake retries every 10s and its frames used to flood the 50kb log,
    // pushing the interesting events out before they could be read
    if (msg.message.type != "ice_candidate" && msg.message.type != "sdp_offer"
        && msg.message.type != "connection_check_response")
    {
        utils__custom_log('[R] msg.message.type : ' + msg.message.type);
    }

    // rsa-encrypted kinds are forwarded from their own branches instead, once node has
    // decrypted them - the ui never holds a private key
    if (dpw_forward_frames == true
        && msg.message.type != "direct_chat_message"
        && msg.message.type != "offline_chat_message"
        && msg.message.type != "direct_chat_file_metadata")
    {
        global.postMessage({ type: "decrypted_frame", value: decrypted_metadata });
    }

    // node's connection status for the login page - meta, not protocol, and it must
    // flow while unauthenticated, so it is handled before the auth gate
    if (msg.message.type == "loopback_status")
    {
        global.postMessage({
            type: "data_processing_worker__loopback_status",
            value: msg.message.value
        });
        return;
    }

    // handled before the auth gate, so a login replay is never dropped
    // (a stale "already authenticated" worker used to swallow it)
    if (msg.message.type == "authentication_status")
    {
        if (msg.message.value == "success")
        {
            g_is_authenticated = true;

            // the whole message rides along as the policy: server_settings_tab__apply_server_policy_fields takes the
            // fields it knows, so a new server field never needs a second list here
            global.postMessage({
                type: "data_processing_worker__authentication_status",
                is_idle_mode_allowed: msg.message.is_idle_mode_allowed,
                policy: msg.message,
                value: "success"
            });

            // the audio datachannel is needed when either client voice or music bots are on (the
            // server pushes bot audio through it); whether this client may transmit is a separate flag
            if (msg.message.is_voice_chat_active == true || msg.message.is_music_bot_audio_active == true)
            {
                global.postMessage({
                    type: "data_processing_worker__audio_enabled",
                    value: true,
                    client_voice_allowed: msg.message.is_voice_chat_active == true,
                    stun_port: msg.message.stun_port
                });
            }
        }
        return;
    }

    // a fast reconnect's answer comes right before authentication_status, i.e. while this worker is
    // still in handshake mode, so it is handled ahead of the authenticated / unauthenticated split
    if (msg.message.type == "fast_reconnect_ok")
    {
        global.postMessage({
            type: "data_processing_worker__fast_reconnect_ok",
            value: msg
        });
        return;
    }

    if (msg.message.type == "datachannel_cooldown")
    {
        global.postMessage({
            type: "data_processing_worker__datachannel_cooldown",
            value: msg
        });
        return;
    }

    if (g_is_authenticated == false)
    {
        if (msg.message.type == "public_key_challenge")
        {
            decryption_result = lemon_crypto.challenge_decrypt(msg.message.value, g_my_rsa_key_object); // has to be processed here, the main thread never holds the private key

            msg.message.decryption_result = decryption_result;

            global.postMessage({
                type: "data_processing_worker__public_key_challenge_from_server",
                value: msg
            });

        }
        else
        {
            global.postMessage({
                type: "data_processing_worker__authentication_status",
                value: "fail"
            });
        }
    }
    else
    {
        if (msg.message.type == "connection_check_response")
        {
            global.postMessage({
                type: "data_processing_worker__connection_check_response",
                value: msg
            });
        }
        else if (msg.message.type == "client_list")
        {
            global.postMessage({
                type: "data_processing_worker__client_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_avatar")
        {
            global.postMessage({
                type: "data_processing_worker__client_avatar_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "avatar_changed")
        {
            global.postMessage({
                type: "data_processing_worker__avatar_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "poke")
        {
            global.postMessage({
                type: "data_processing_worker__poke_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "access_denied")
        {
            global.postMessage({
                type: "data_processing_worker__access_denied_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "force_admin_password_change")
        {
            global.postMessage({
                type: "data_processing_worker__force_admin_password_change",
                value: msg
            });
        }
        else if (msg.message.type == "server_settings_values")
        {
            global.postMessage({
                type: "data_processing_worker__server_settings_values_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "admin_log")
        {
            global.postMessage({
                type: "data_processing_worker__admin_log_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_policy")
        {
            global.postMessage({
                type: "data_processing_worker__server_policy_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_info")
        {
            global.postMessage({
                type: "data_processing_worker__client_info_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_full")
        {
            global.postMessage({
                type: "data_processing_worker__channel_full_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_list")
        {
            global.postMessage({
                type: "data_processing_worker__channel_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_list")
        {
            global.postMessage({
                type: "data_processing_worker__icon_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_list")
        {
            global.postMessage({
                type: "data_processing_worker__tag_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "identity_list")
        {
            global.postMessage({
                type: "data_processing_worker__identity_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_add")
        {
            global.postMessage({
                type: "data_processing_worker__icon_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_add_to_client")
        {
            global.postMessage({
                type: "data_processing_worker__tag_add_to_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "remove_tag_from_client")
        {
            global.postMessage({
                type: "data_processing_worker__remove_tag_from_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_alias_changed")
        {
            global.postMessage({
                type: "data_processing_worker__client_alias_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_country_code_changed")
        {
            global.postMessage({
                type: "data_processing_worker__client_country_code_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "typing_indicator")
        {
            global.postMessage({
                type: "data_processing_worker__typing_indicator_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "stored_clients_list")
        {
            global.postMessage({
                type: "data_processing_worker__stored_clients_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_add")
        {
            global.postMessage({
                type: "data_processing_worker__tag_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_delete")
        {
            global.postMessage({
                type: "data_processing_worker__tag_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_delete")
        {
            global.postMessage({
                type: "data_processing_worker__icon_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_icon_changed")
        {
            global.postMessage({
                type: "data_processing_worker__tag_icon_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_icon_changed")
        {
            global.postMessage({
                type: "data_processing_worker__channel_icon_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "start_song_stream")
        {
            global.postMessage({
                type: "data_processing_worker__start_song_stream_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "stop_song_stream")
        {
            global.postMessage({
                type: "data_processing_worker__stop_song_stream_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "chat_message_delete")
        {
            global.postMessage({
                type: "data_processing_worker__chat_message_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "chat_message_edit")
        {
            global.postMessage({
                type: "data_processing_worker__chat_message_edit_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_chat_message_id_for_local_message_id")
        {
            global.postMessage({
                type: "data_processing_worker__server_chat_message_id_for_local_message_id_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_join")
        {
            global.postMessage({
                type: "data_processing_worker__channel_join_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_role_add")
        {
            global.postMessage({
                type: "data_processing_worker__client_role_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_connect")
        {
            global.postMessage({
                type: "data_processing_worker__client_connect_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_disconnect")
        {
            global.postMessage({
                type: "data_processing_worker__client_disconnect_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_rename")
        {
            global.postMessage({
                type: "data_processing_worker__client_rename_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "audio_state_of_single_client")
        {
            global.postMessage({
                type: "data_processing_worker__audio_state_of_single_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "current_channel_active_microphone_usage")
        {
            global.postMessage({
                type: "data_processing_worker__current_channel_active_microphone_usage_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "sdp_offer")
        {
            global.postMessage({
                type: "data_processing_worker__sdp_offer_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "ice_candidate")
        {
            global.postMessage({
                type: "data_processing_worker__ice_candidate_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_info_broadcast")
        {
            global.postMessage({
                type: "data_processing_worker__server_info_broadcast_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_edit")
        {
            global.postMessage({
                type: "data_processing_worker__channel_edit_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_going_to_idle_mode")
        {
            global.postMessage({
                type: "data_processing_worker__client_going_to_idle_mode",
                value: msg
            });
        }
        else if (msg.message.type == "client_coming_back_from_idle_mode")
        {
            global.postMessage({
                type: "data_processing_worker__client_coming_back_from_idle_mode",
                value: msg
            });
        }
        else if (msg.message.type == "channel_delete")
        {
            global.postMessage({
                type: "data_processing_worker__channel_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_create")
        {
            global.postMessage({
                type: "data_processing_worker__channel_create_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_picture_metadata")
        {
            global.postMessage({
                type: "data_processing_worker__channel_chat_picture_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_picture_metadata")
        {
            global.postMessage({
                type: "data_processing_worker__direct_chat_picture_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_file_metadata")
        {
            // the header is opened here (channel keys live in this worker); null leaves a nameless card
            msg.message.file_header_decrypted = (typeof msg.message.file_header === "string")
                ? chat_files__decrypt_channel_chat_file_header(msg.message.file_id, msg.message.file_header) : null;
            msg.message.file_header = null;

            global.postMessage({
                type: "data_processing_worker__channel_chat_file_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_file_metadata")
        {
            // loopback: node already opened the header, so just pass it on
            if (msg.message.file_header_decrypted == null)
            {
                msg.message.file_header_decrypted = (typeof msg.message.file_header === "string")
                    ? chat_files__parse_chat_file_header(chat_files__open_direct_chat_file_envelope(msg.message.file_header)) : null;
                msg.message.file_header = null;

                // opened shape, so the ui reads it without a key of its own
                if (dpw_forward_frames == true)
                {
                    global.postMessage({ type: "decrypted_frame", value: JSON.stringify(msg) });
                }
            }

            global.postMessage({
                type: "data_processing_worker__direct_chat_file_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_file_decrypted")
        {
            // loopback only: node decrypted it for us
            global.postMessage({
                type: "data_processing_worker__direct_chat_file",
                value: { message: {
                    file_id: msg.message.file_id,
                    client_id: msg.message.client_id,
                    file: msg.message.file
                } }
            });
        }
        else if (msg.message.type == "file_send_error")
        {
            global.postMessage({
                type: "data_processing_worker__file_send_error_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_maintainer_id")
        {
            global.postMessage({
                type: "data_processing_worker__channel_maintainer_id",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_message")
        {
            if (g_current_channel_keys != null)
            {
                let decrypted_value = keys__decrypt_base64_contents_with_aes_keys(g_current_channel_keys, msg.message.value);

                msg.message.decrypted_value = decrypted_value;
                msg.message.value = null;

                global.postMessage({
                    type: "data_processing_worker__channel_chat_message",
                    value: msg
                });
            }
            else
            {
                utils__custom_log("channel_chat_message decrypt failed");
            }
        }
        else if (msg.message.type == "direct_chat_message")
        {
            // loopback: node already decrypted this, so just pass it on
            if (msg.message.some_json != null)
            {
                global.postMessage({
                    type: "data_processing_worker__direct_chat_message",
                    value: msg
                });
                return;
            }

            let message_data = JSON.parse(msg.message.value);

            let decryption_result = lemon_crypto.decrypt(message_data.message_keys, g_my_rsa_key_object);

            if (decryption_result.status == 'failure')
            {
                utils__custom_log("! failed to decrypt direct chat message !");
                return;
            }

            let currect_direct_message_keys = JSON.parse(decryption_result.plaintext);
            let decrypted_text = keys__decrypt_base64_contents_with_aes_keys(currect_direct_message_keys, message_data.message_value);

            let some_json = JSON.parse(decrypted_text);

            if (some_json.type == "direct_chat_message")
            {
                some_json.value = some_json.value; // sanitize here ?
            }

            msg.message.some_json = some_json;
            msg.message.value = null;

            // decrypted shape, so the ui reads it without a key of its own
            if (dpw_forward_frames == true)
            {
                global.postMessage({ type: "decrypted_frame", value: JSON.stringify(msg) });
            }

            global.postMessage({
                type: "data_processing_worker__direct_chat_message",
                value: msg
            });
        }
        else if (msg.message.type == "offline_chat_message")
        {
            // loopback: node already decrypted this, so just pass it on
            if (msg.message.some_json != null)
            {
                global.postMessage({
                    type: "data_processing_worker__offline_chat_message",
                    value: msg
                });
                return;
            }

            // something that was said to us while we were away. same envelope as a direct
            // message, so the same decrypt; it just arrives on connect instead of live
            let message_data = JSON.parse(msg.message.value);

            let decryption_result = lemon_crypto.decrypt(message_data.message_keys, g_my_rsa_key_object);

            if (decryption_result.status == 'failure')
            {
                // queued for a different keypair than the one we hold now
                utils__custom_log("! failed to decrypt offline chat message !");
                return;
            }

            let current_offline_message_keys = JSON.parse(decryption_result.plaintext);
            let decrypted_text = keys__decrypt_base64_contents_with_aes_keys(current_offline_message_keys, message_data.message_value);

            msg.message.some_json = JSON.parse(decrypted_text);
            msg.message.value = null;

            // decrypted shape, so the ui reads it without a key of its own
            if (dpw_forward_frames == true)
            {
                global.postMessage({ type: "decrypted_frame", value: JSON.stringify(msg) });
            }

            global.postMessage({
                type: "data_processing_worker__offline_chat_message",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_picture_decrypted")
        {
            // loopback only: node decrypted it for us
            global.postMessage({
                type: "data_processing_worker__direct_chat_picture",
                value: { message: {
                    picture_id: msg.message.picture_id,
                    client_id: msg.message.client_id,
                    value: null,
                    decrypted_base64_picture: msg.message.decrypted_base64_picture
                } }
            });
        }
        else if (msg.message.type == "image_sent_status")
        {
            global.postMessage({
                type: "data_processing_worker__image_sent_status_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "call")
        {
            global.postMessage({
                type: "data_processing_worker__call_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "music_bot_song_list")
        {
            global.postMessage({
                type: "data_processing_worker__music_bot_song_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_send_success")
        {
            global.postMessage({
                type: "data_processing_worker__file_send_success_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_receive_chunk")
        {
            global.postMessage({
                type: "data_processing_worker__file_receive_chunk_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_receive_completed")
        {
            global.postMessage({
                type: "data_processing_worker__file_receive_completed_from_server",
                value: msg
            });
        }
    }
}

// ---- websocket worker ----

/**
 * @brief the websocket worker's message handler
 *        creates the socket (relaying its events to the main thread), sends data when the socket
 *        is open, and closes it on request
 *
 * @param object e -> the worker message event; e.data.type picks the branch
 *
 * @return void
 */
function workers__websocket_worker_onmessage(e)
{

    if (e.data.type == "create_websocket_object")
    {
        let generation = ++websocket_generation;

        // a replaced socket is closed, and its events below are ignored as stale
        if (websocket_instance != null)
        {
            try { websocket_instance.close(); } catch (close_error) { }
        }

        websocket_instance = new WebSocket(e.data.value.connection_string);

        websocket_instance.addEventListener('message', function (event)
        {
            if (generation != websocket_generation) { return; }

            global.postMessage({
                type: "websocket_worker_onmessage",
                value: event.data
            });
        });

        websocket_instance.onclose = function ()
        {
            if (generation != websocket_generation) { return; }

            console.log("websocket closed");
            global.postMessage({
                type: "websocket_worker_onclose"
            });
        }

        websocket_instance.onerror = function (error)
        {
            if (generation != websocket_generation) { return; }

            // node hands us the real socket error; its code says WHY (no network vs
            // refused vs timeout), which is the difference between a useful message
            // and a guess. a browser gives an opaque event and no code - that is fine
            let error_code = (error != null && typeof error.code === "string") ? error.code : "";

            console.error("websocket error event" + (error_code !== "" ? " (" + error_code + ")" : ""));
            global.postMessage({
                type: "websocket_worker_onerror",
                error_code: error_code
            });
        }

        websocket_instance.onopen = function ()
        {
            if (generation != websocket_generation) { return; }

            websocket_instance.send(e.data.value.onopen_data);
        }
    }

    else if (e.data.type == "send")
    {
        // null before the first connect: the loopback ui can forward a request while
        // node still has no socket, and dereferencing it threw out of the handler
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

    else if (e.data.type == "close")
    {
        // deliberate disconnect (identity switch): the onclose event that follows
        // drives the normal reset path on the main thread
        if (websocket_instance != null)
        {
            websocket_instance.close();
        }
    }
}
