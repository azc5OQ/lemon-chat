
// data processing worker message handler: generates the rsa keypair, encrypts outgoing
// chat messages/pictures and decrypts incoming ones, posts every result to the main thread
// when on, every decrypted server frame is also posted back raw (node uses it for the ui replay)
var g_dpw_forward_frames = false;

function data_processing_worker_onmessage(e)
{
    if (DBG_WORKER_BOOT_LOG) { console.log("[dpw] got: " + (e.data && e.data.type)); }
    if (e.data.type == "mainthread__set_frame_forwarding")
    {
        g_dpw_forward_frames = (e.data.value == true);
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
        custom_log("generating random public/private key pair");
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
            custom_log("FATAL: rsa keygen wasm failed (" + err + "), cannot create an identity");
            throw err;
        }
        g_rsa_public_key_string = lemon_crypto.publicKeyString(g_my_rsa_key_object);

        custom_log("identity_passphrase_string now in use -> " + identity_passphrase_string);

        global.postMessage({
            type: "data_processing_worker__generate_rsa_keypair_result",
            value: g_rsa_public_key_string,
            identity_string: identity_passphrase_string
        });

        custom_log("public key now in use: " + g_rsa_public_key_string);

    }
    else if (e.data.type == "mainthread__process_received_websocket_message")
    {
        mainthread__process_received_websocket_message_continue(e);
    }
    else if (e.data.type == "mainthread_reset_data")
    {
        g_is_authenticated = false;
    }
    else if (e.data.type == "mainthread__channel_keys_for_data_processing_worker")
    {
        if (current_channel_keys != null)
        {
            g_historic_keys_of_current_channel.push(current_channel_keys);

            if (g_historic_keys_of_current_channel.length > 7)
            {
                g_historic_keys_of_current_channel.shift();
            }
        }
        current_channel_keys = e.data.value;
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
            message_value: encrypt_with_aes_keys_and_convert_to_base64(receipt_keys, JSON.stringify(receipt_value_object))
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
            seen_receipt_message_content: encrypt_all_message_data_and_convert_to_base64(JSON.stringify(receipt_message_object))
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

        current_channel_keys = new_channel_keys;

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
                value: JSON.stringify(current_channel_keys)
            };

            message_data.message_value = encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

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
            let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

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

        message_data.message_value = encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

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

        let message_json_string = process_message_before_sending(message_object);

        let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

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

        message_data.message_value = encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

        let message_object = {
            message: {
                type: "offline_chat_message",
                value: JSON.stringify(message_data),
                recipient_alias: e.data.recipient_alias
            }
        };

        let message_json_string = process_message_before_sending(message_object);

        let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

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

        message_data.message_value = encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, e.data.base64_picture_string_to_send);

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

        let data_for_upload_process = encrypt_with_aes_keys_and_convert_to_base64(e.data.current_channel_keys, e.data.base64_picture_string_to_send);

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

        message_object.message.value = encrypt_with_aes_keys_and_convert_to_base64(e.data.current_channel_keys, e.data.chat_message_to_send_value);

        let message_json_string = process_message_before_sending(message_object);
        let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

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
        let decrypted_base64_picture = decrypt_base64_contents_with_aes_keys(current_message_keys, message_data.message_value);
        let decrypted_base64_picture_sanitized = sanitize_image_data_url(decrypted_base64_picture);

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
        if (g_dpw_forward_frames == true)
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
        if (current_channel_keys != null)
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
            let decrypted_base64_picture = decrypt_base64_contents_with_aes_keys(current_channel_keys, e.data.message_raw);

            if (decrypted_base64_picture.startsWith("data:image"))
            {
                is_decrypted = true;
            }
            else
            {
                for (i = 0; i < g_historic_keys_of_current_channel.length; i++)
                {
                    decrypted_base64_picture = decrypt_base64_contents_with_aes_keys(g_historic_keys_of_current_channel[i], e.data.message_raw);
                    if (decrypted_base64_picture.startsWith("data:image"))
                    {
                        console.log("managed to decrypt channel chat picture with channel key at" + i)
                        is_decrypted = true;
                        break;
                    }
                }
            }

            if (is_decrypted)
            {
                let decrypted_base64_picture_sanitized = sanitize_image_data_url(decrypted_base64_picture);
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
            custom_log("channel_chat_picture decrypt failed, data_processing_worker thread missing current channel keys");
        }
    }
    else if (e.data.type == "mainthread__create_direct_chat_file")
    {
        // header and body get their own key sets: two plaintexts must never share a ctr keystream.
        // the body pads up so even a 1 kb file clears the server's minimum upload size
        let file_header = create_direct_chat_file_envelope(e.data.receiver_public_key, build_chat_file_header_json(e.data.file));
        let data_for_upload_process = create_direct_chat_file_envelope(e.data.receiver_public_key, build_chat_file_body_json(e.data.file), G_CHAT_FILE_BODY_MIN_PADDED_BYTES);

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
            data_for_upload_process: encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, build_chat_file_body_json(e.data.file), G_CHAT_FILE_BODY_MIN_PADDED_BYTES),
            extra_data: {
                receiver_id: parseInt(e.data.current_channel_id),
                local_message_id: e.data.local_message_id,
                file_header: encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, build_chat_file_header_json(e.data.file))
            }
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_direct_chat_file_data")
    {
        let file = parse_decrypted_chat_file_body(open_direct_chat_file_envelope(e.data.message_raw));

        if (file == null)
        {
            custom_log("failed to decrypt direct chat file");
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
        if (g_dpw_forward_frames == true)
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
        let file = decrypt_channel_chat_file_body(e.data.file_id, e.data.message_raw);

        if (file == null)
        {
            custom_log("channel_chat_file decrypt failed, no channel key opened it");
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

// runs in the data processing worker: decrypts an incoming server message's metadata, handles
// the auth handshake, decrypts chat payloads, and forwards everything typed to the main thread
// returns the announced minimum key size when a raw frame is the plaintext "your key is too
// weak" notice, and null for everything else. that notice is the ONLY type accepted in the
// clear: the server sends it before the diffie-hellman exchange finishes, so neither side has
// a shared secret to encrypt with yet. every other type stays on the encrypted path
function try_read_plaintext_rsa_key_notice(raw_value)
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

function mainthread__process_received_websocket_message_continue(e)
{
    if (e.data.is_plaintext != true)
    {
        let announced_minimum_bits = try_read_plaintext_rsa_key_notice(e.data.value);

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
    let decrypted_metadata = (e.data.is_plaintext == true) ? e.data.value : decrypt_message_metadata(e.data.value);

    let msg = null;

    try
    {
        msg = JSON.parse(decrypted_metadata);
    }
    catch (Exception)
    {
        console.log(Exception+ " decrypted string length: " + decrypted_metadata.length + " "+ decrypted_metadata);
    }

    // let str = '[R] msg.message.type : ' + msg.message.type + ' | size in bytes: ' + decrypted_metadata.length;
    // the webrtc handshake retries every 10s and its frames used to flood the 50kb log,
    // pushing the interesting events out before they could be read
    if (msg.message.type != "ice_candidate" && msg.message.type != "sdp_offer"
        && msg.message.type != "connection_check_response")
    {
        custom_log('[R] msg.message.type : ' + msg.message.type);
    }

    // rsa-encrypted kinds are forwarded from their own branches instead, once node has
    // decrypted them - the ui never holds a private key
    if (g_dpw_forward_frames == true
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

            global.postMessage({
                type: "data_processing_worker__authentication_status",
                is_idle_mode_allowed: msg.message.is_idle_mode_allowed,
                is_alias_registration_allowed: msg.message.is_alias_registration_allowed,
                allow_typing_indicator: msg.message.allow_typing_indicator,
                allow_client_renames: msg.message.allow_client_renames,
                allow_avatars: msg.message.allow_avatars,
                avatar_max_size: msg.message.avatar_max_size,
                allow_file_uploads: msg.message.allow_file_uploads,
                file_upload_max_size: msg.message.file_upload_max_size,
                chat_picture_max_size: msg.message.chat_picture_max_size,
                allow_chat_pictures: msg.message.allow_chat_pictures,
                value: "success"
            });

            // set up the audio datachannel whenever audio is on for EITHER clients or music bots:
            // the datachannel is the path the server uses to push music-bot audio to this client,
            // so it must exist even when client voice is disabled. whether this client may
            // transmit its own mic is a separate flag (client_voice_allowed) honoured below
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
                ? decrypt_channel_chat_file_header(msg.message.file_id, msg.message.file_header) : null;
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
                    ? parse_chat_file_header(open_direct_chat_file_envelope(msg.message.file_header)) : null;
                msg.message.file_header = null;

                // opened shape, so the ui reads it without a key of its own
                if (g_dpw_forward_frames == true)
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
            if (current_channel_keys != null)
            {
                let decrypted_value = decrypt_base64_contents_with_aes_keys(current_channel_keys, msg.message.value);

                msg.message.decrypted_value = decrypted_value;
                msg.message.value = null;

                global.postMessage({
                    type: "data_processing_worker__channel_chat_message",
                    value: msg
                });
            }
            else
            {
                custom_log("channel_chat_message decrypt failed");
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
                custom_log("! failed to decrypt direct chat message !");
                return;
            }

            let currect_direct_message_keys = JSON.parse(decryption_result.plaintext);
            let decrypted_text = decrypt_base64_contents_with_aes_keys(currect_direct_message_keys, message_data.message_value);

            let some_json = JSON.parse(decrypted_text);

            if (some_json.type == "direct_chat_message")
            {
                some_json.value = some_json.value; // sanitize here ?
            }

            msg.message.some_json = some_json;
            msg.message.value = null;

            // decrypted shape, so the ui reads it without a key of its own
            if (g_dpw_forward_frames == true)
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
                custom_log("! failed to decrypt offline chat message !");
                return;
            }

            let current_offline_message_keys = JSON.parse(decryption_result.plaintext);
            let decrypted_text = decrypt_base64_contents_with_aes_keys(current_offline_message_keys, message_data.message_value);

            msg.message.some_json = JSON.parse(decrypted_text);
            msg.message.value = null;

            // decrypted shape, so the ui reads it without a key of its own
            if (g_dpw_forward_frames == true)
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

// here are variables dedicated to g_websocket_worker thread
var g_websocket_instance = null;

// counts sockets ever created. events from an old (replaced) socket are ignored
var g_websocket_generation = 0;

// websocket worker message handler: creates the socket (relaying its events to the
// main thread), sends data when the socket is open, and closes it on request
function websocket_worker_onmessage(e)
{

    if (e.data.type == "create_websocket_object")
    {
        let generation = ++g_websocket_generation;

        // a replaced socket is closed, and its events below are ignored as stale
        if (g_websocket_instance != null)
        {
            try { g_websocket_instance.close(); } catch (close_error) { }
        }

        g_websocket_instance = new WebSocket(e.data.value.connection_string);

        g_websocket_instance.addEventListener('message', function (event)
        {
            if (generation != g_websocket_generation) { return; }

            global.postMessage({
                type: "websocket_worker_onmessage",
                value: event.data
            });
        });

        g_websocket_instance.onclose = function ()
        {
            if (generation != g_websocket_generation) { return; }

            console.log("websocket closed");
            global.postMessage({
                type: "websocket_worker_onclose"
            });
        }

        g_websocket_instance.onerror = function (error)
        {
            if (generation != g_websocket_generation) { return; }

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

        g_websocket_instance.onopen = function ()
        {
            if (generation != g_websocket_generation) { return; }

            g_websocket_instance.send(e.data.value.onopen_data);
        }
    }

    else if (e.data.type == "send")
    {
        // null before the first connect: the loopback ui can forward a request while
        // node still has no socket, and dereferencing it threw out of the handler
        if (g_websocket_instance == null)
        {
            console.warn("websocket send skipped: no socket yet");
        }
        else if (g_websocket_instance.readyState === 1)
        {
            g_websocket_instance.send(e.data.value);
        }
        else
        {
            console.warn("websocket send skipped: socket not open (readyState " + g_websocket_instance.readyState + ")");
        }
    }

    else if (e.data.type == "close")
    {
        // deliberate disconnect (identity switch): the onclose event that follows
        // drives the normal reset path on the main thread
        if (g_websocket_instance != null)
        {
            g_websocket_instance.close();
        }
    }
}


// -------------------------------------------------------------------------------------------
// readable console logging. this whole file IS the moduleFactory that every worker re-executes
// (see create_new_webworker_in_same_file), so this block installs ONCE in the source yet runs
// in every context - main thread and all five workers. it wraps console.* to prefix a coloured
// [context][fn:line][time] tag; the colour tells you which thread a line came from at a glance.
// tune it live from a context's devtools. the __LOG.xxx() METHODS are GLOBAL - they propagate to
// every context (main + all workers) over a BroadcastChannel; assigning a raw property is local
// to the context you type it in.
//    global.__LOG.disable()   /  .enable()      turn ALL logging off / on everywhere (global)
//    global.__LOG.setTime(false)                drop the [time] segment everywhere (global)
//    global.__LOG.set({ level: "warn" })        any field, globally (level/mute/raw/enabled/time)
//    global.__LOG.level = "warn"                this context only: hide debug/info
//    global.__LOG.mute.push("g_opus_decoder_worker")   this context only: g_silence a context
//    global.__LOG.raw = true                    this context only: bypass, restore native links
// context name comes from THREAD_NAME (already trimmed for workers above); do not introduce the
// literal placeholder assignment before this point or the worker-name injection would miscount.
// -------------------------------------------------------------------------------------------
(function ()
{
    var context_name = IS_CURRENT_THREAD_WORKER ? THREAD_NAME : "main";

    var COLORS = {
        "main":                   "#6ab0ff",
        "data_processing_worker": "#4caf50",
        "websocket_worker":       "#26c6da",
        "opus_encoder_worker":    "#ff9800",
        "opus_decoder_worker":    "#c586c0",
        "minimp3_worker":         "#d7ba7d"
    };
    var color = COLORS[context_name] || "#9aa0a6";

    var LEVELS = { debug: 10, info: 20, warn: 30, error: 40 };
    var cfg = global.__LOG || (global.__LOG = { enabled: true, level: "debug", mute: [], time: true, raw: false });

    // cross-context control: changes made through cfg.set / .disable / .enable / .setTime are
    // broadcast to every context (main + all workers) so "disable logging" is truly global. a
    // BroadcastChannel reaches a page and its dedicated workers on the same origin. the receive
    // handler only copies data fields; it never re-broadcasts, so there is no echo loop.
    var log_channel = null;
    try { log_channel = new BroadcastChannel("lemon_log_config"); } catch (e) { console.warn("BroadcastChannel unavailable; cross-context log control disabled:", e.message); log_channel = null; }
    if (log_channel !== null)
    {
        log_channel.onmessage = function (ev)
        {
            if (ev && ev.data) { for (var k in ev.data) { cfg[k] = ev.data[k]; } }
        };
    }
    cfg.set = function (patch)
    {
        for (var k in patch) { cfg[k] = patch[k]; }
        if (log_channel !== null) { try { log_channel.postMessage(patch); } catch (ee) { console.warn("failed to broadcast log config:", ee.message); } }
    };
    cfg.disable = function () { cfg.set({ enabled: false }); };
    cfg.enable  = function () { cfg.set({ enabled: true }); };
    cfg.setTime = function (on) { cfg.set({ time: on !== false }); };

    function pad(n, width)
    {
        var s = "" + n;
        while (s.length < width) { s = "0" + s; }
        return s;
    }

    function stamp()
    {
        var d = new Date();
        return pad(d.getHours(), 2) + ":" + pad(d.getMinutes(), 2) + ":" + pad(d.getSeconds(), 2) + "." + pad(d.getMilliseconds(), 3);
    }

    // pull the real call site (function + line) out of a fresh stack. the browser's own
    // file:line link would otherwise point here at the wrapper, so we reconstruct it in text.
    function origin()
    {
        var stack = "";
        try { throw new Error(); } catch (e) { stack = e.stack || ""; }

        var frames = [];
        var raw = stack.split("\n");
        for (var i = 0; i < raw.length; i++)
        {
            // drop chrome's leading "Error" header line; keep the actual frames
            if (raw[i].indexOf("Error") === 0 && raw[i].indexOf("@") === -1) { continue; }
            if (raw[i].length > 0) { frames.push(raw[i]); }
        }

        // frames[0] = origin(), frames[1] = the console wrapper, frames[2] = the real caller
        var caller = frames[2] || frames[frames.length - 1] || "";

        var fn = "anon";
        var m = caller.match(/at (?:async )?([^\s(]+)\s*\(/); // chrome: "    at fn (url:line:col)"
        if (m === null) { m = caller.match(/([^@\s]+)@/); }    // firefox: "fn@url:line:col"
        if (m !== null && m[1]) { fn = m[1]; }

        var line = "?";
        var lm = caller.match(/:(\d+):\d+\)?\s*$/);
        if (lm !== null) { line = lm[1]; }

        return fn + ":" + line;
    }

    function passes(level)
    {
        if (LEVELS[level] < LEVELS[cfg.level || "debug"]) { return false; }
        for (var i = 0; i < cfg.mute.length; i++)
        {
            if (cfg.mute[i] == context_name) { return false; }
        }
        return true;
    }

    function make(level, native_fn)
    {
        return function ()
        {
            if (cfg.enabled === false) { return; }                                 // global master switch, wins over everything
            if (cfg.raw) { return native_fn.apply(console, arguments); }
            if (passes(level) === false) { return; }

            // time is skipped (not even computed) when disabled
            var time_part = (cfg.time === false) ? "" : ("[" + stamp() + "]");
            var tag = "[" + context_name + "][" + origin() + "]" + time_part;
            var args = Array.prototype.slice.call(arguments);
            args.unshift("%c" + tag + "%c", "color:" + color + ";font-weight:bold", "color:inherit");
            return native_fn.apply(console, args);
        };
    }

    if (typeof console !== "undefined")
    {
        var native_log   = console.log   ? console.log.bind(console)   : function () {};
        var native_info  = console.info  ? console.info.bind(console)  : native_log;
        var native_warn  = console.warn  ? console.warn.bind(console)  : native_log;
        var native_error = console.error ? console.error.bind(console) : native_log;

        console.log   = make("info",  native_log);
        console.debug = make("debug", native_log);
        console.info  = make("info",  native_info);
        console.warn  = make("warn",  native_warn);
        console.error = make("error", native_error);
    }
})();

var g_are_server_details_predefined = false;
// for webpage to play sounds,  microphone user has to interact with it. A simple click on button (for example connect button) is enough
// in android WebView, this is not a problem, its not a problem in electron either i assume
// its also not a problem if chat is embedded in website, user already interacts with the website lots of ways before he loads some chat
// its only problem in testing when I re-launch client.html from desktop and it refuses use create AudioContext
// client.html:5164 The AudioContext was not allowed to start. It must be resumed (or created) after a user gesture on the page. https://goo.gl.qjz9zk/7K7WLu
// so in rare cases when it is a problem, just set the g_is_autoconnect_without_user_action_active to false
// i could explore this problem more but for now this solution is enough

var g_is_autoconnect_without_user_action_active = false;
var g_is_reconnect_active = true;

// whether the last connect attempt actually failed (so the login shows
// "connection failed, retrying in Xs" - not before)
var g_last_connect_attempt_failed = false;

// set once the udp-capability probe has run (it fires after the first failed
// datachannel attempt window; see probe_webrtc_udp_and_warn)
var g_webrtc_udp_probe_done = false;

// for any sound to play in html page, client must click something first
// if client just connects without clicking anything on website
// sound wont play. protection against popup websites playing music
// so make client click something for sound to play, like a button
// if you dont care about client hearing sound, set g_are_server_details_predefined and g_is_autoconnect_without_user_action_active to true

var g_autoconnect_details = {
    host: "192.168.1.106",
    port: 1111,
    keys: [
        "test",
        // "test2"
    ]
};

// the username this client asks for while connecting; the server uses it instead of the
// assigned user0/user1/... name, provided nobody connected is using it. leave "" for the
// assigned name. a registered identity's admin-set name still wins over this
var g_chosen_username = "";

// on unless the server policy says users may not rename themselves; admins always may
var g_client_renames_allowed = true;

// true once the server granted this session the admin tag
var g_is_local_client_admin = false;

// greys the local rename input when the server ignores user renames, because a rename that
// silently does nothing reads as a bug; an admin keeps the input editable
function apply_rename_policy_to_ui()
{
    let rename_input = document.getElementById("connected-local-client-input");

    if (rename_input == null)
    {
        return;
    }

    let may_rename = (g_client_renames_allowed == true) || (g_is_local_client_admin == true);

    rename_input.readOnly = (may_rename == false);
    rename_input.title = (may_rename == false) ? "renames are disabled on this server" : "";
}

var g_custom_key_count = 0; // no key field by default; the "add" button creates key 0, key 1, ... so connecting with zero keys matches a server configured with no extra metadata keys
var g_metadata_keys = [];

var g_dh_generator = null;
var g_dh_modulus = null;
var g_dh_secret_exponent = null;

if (typeof window != 'undefined')
{
    g_dh_generator = 2n;

    var dh_modulus_bits = 8192; // 2048, 4096, or 8192 - MUST match the server's DH_MODULUS_BITS in dh_primes.h

    // safe prime; the size is chosen by dh_modulus_bits above
    var dh_modulus_string = "32317006071311007300338913926423828248817941241140239112842009751400741706634354222619689417363569347117901737909704191754605873209195028853758986185622153212175412514901774520270235796078236248884246189477587641105928646099411723245426622522193230540919037680524235519125679715870117001058055877651038861847280257976054903569732561526167081339361799541336476559160368317896729073178384589680639671900977202194168647225871031411336429319536193471636533209717077448227988588565369208645296636077250268955505928362751121174096972998068410554359584866583291642136218231078990999448652468262416972035911852507045361090559";
    if (dh_modulus_bits == 4096) { dh_modulus_string = "769693417275193209984647063932271739387855846059952565355802298991172654607712104048642837327086393649061117273977479029847880929901816490502575106445708811728815104699538212859676255621694933541780065300216380119365448477045659714142752962409351060465337847941705392356465059912091910379610354725312649190019796723866880686790102505810145302961022375682955537024852712153016097337874982984026217981644194741246064934907045623310252540105014478602030042625050790892256552738501094150544017503366521405695110938208299693781667383898463231081051406284286841973557391837014717399840317430691522097547152517168661381236591027075489125945391080953462725086945640553263655450552529331021143283920078415126554651532264427032676910742519456229972244566183184460134372157613705601578581124341629241972089511142281008551119184446873409929566545851290281361166571221433352162292794386037934251491816926283501321267998170847037246436312385384549128374516008246102779333275418845691810684079267893733515705735729967088709311436111220883412133997678612136656076019025878523079903588858263406471350679442414909683284164773663260965983542100471510190345089294782440003918912451456992077790689703598681468439291465843547007915368517110174489101683183"; }
    if (dh_modulus_bits == 8192) { dh_modulus_string = "977457999394373613160803436413990067824664325329752783398860665650891758153546599425963979290921371512554424155404999662026528515231003619059250227581073193191817667145872254566150016152267378041043707227533090523130854162674719502912605093407840683480360272745764870153876137077404098306192221010043540729677343845507367074906936225679715947791388837030145881441948294883090889231339114529926218527174080089614380520453541140942641189135120655392817995558672348259240314664441211284094553472715483266674338226096876439738570920830431990068192898823624154982561509367679529622217472548774982858792731946532808614927934739139866206407985368007801168974233995065956999264060271473667589912911635282735847769997114239537087675283088650912126339354407150753898672557625721020468123121985217987055904104587919799098757089865856148151372159009557757252554523938389147793317088678845894492183206966490686288450161789777284440531583751707235355985011244929317700461604659491747569397015417605015844114189684757052804077400345793511780894375616367276781058142309055525279814951138171200246013005920763485637476273362810215876618783686490089700542042236756450450875748150296041125307287408385472961923570457489504445415040799421139916299426300456654408986639074321942096027448333024943556858265321864649169731360525833923693176347415312865652669889035367188555225154697353012485541899891405325448203641838378457181625265662866012641101851261909549715296062722220871699753970653198277244417978954211900339094323007747108915608099803996978341421956304178871205193962176652206358214977516703892527582137534788148271002406853630936928238671372686645287319965234542895792366832783193938302132219463877916744337606350077802640502896551808414614146119887726456058723288826440787605655349724793408478985959688481296784606909713086104258354166909655924759369179237403392373490546567686597579050582406536565868808879640217167277372359442106490085619712603267716148318470568791898988932303644832977117895522999512850187807810874398403568491329765776005361335497620431318887438033026603280081068652656086250926318691324234636743583950635209943952384403217947022081052893713058850302983931039183796265186758214153198152532323300955155280353467780525511888234737731346004632762960546857790075686663681335897303755575277977768637069626833995439307976899886428471454602498498262648098952772543021280017770216979020055000596511525631600903871686087096720903178326987700681798435674674029499323"; }
    g_dh_modulus = BigInt(dh_modulus_string);
}

var g_is_microphone_always_on = false;

// the ONE writer of g_is_microphone_always_on. it used to be flipped inside
// UI.activate_continous_audio_broadcast_onclick with `= !g_is_microphone_always_on`, which made
// the flag unreachable without a ui to click - and made any second writer invert it instead of
// setting it. the side effects (mic, android bridge, repaint) stay in ui.js, because voice
// belongs to the webview; a headless runtime owns the flag and must not touch the microphone
function set_microphone_always_on(is_active)
{
    g_is_microphone_always_on = (is_active == true);

    // the android settings radio maps onto the new tap-to-toggle mode; a choice made in
    // the local settings panel (localStorage) outranks it
    let has_local_choice = false;
    try { has_local_choice = (typeof localStorage !== "undefined" && localStorage.getItem("lemon_continuous_mic") != null); } catch (e) { }

    if (has_local_choice == false)
    {
        g_is_continuous_mic_mode = g_is_microphone_always_on;
    }
}

// first push is the app handing over saved settings, later ones mean a switch moved while running
var g_have_received_android_settings = false;
var g_upload_ack_timer = null;
var G_UPLOAD_ACK_TIMEOUT_MS = 300000;   // a slow 10mb upload legitimately takes minutes
var g_keys_init_status = false;
var g_is_identity_switch_in_progress = false; // deliberate disconnect->new-keypair->reconnect cycle (top bar "identity" button)
var base64_picture_string_to_send = "" ;
var g_verification_message = "welcome";
var g_is_websocket_connected = false;
var g_is_webrtc_datachannel_connected = false;
var g_is_webrtc_datachannel_check_running = false;
var host = "";
var port = 0;
var websocket = null;
var g_local_username = "";
var local_client_id = 0;
var g_alert_push_to_talk_key_shown_once = false;
var g_alert_streaming_music_shown_once = false;
var g_stop_song_stream_message_received = false;
var g_selected_server_chat_message_id = null;
var g_are_sound_effects_enabled = true;

// push-to-talk release hangover: capture keeps running this long after the key is let
// go, so word tails ("hello" not "hell") ship before the mic-off message stops the relay
var g_ptt_release_hangover_ms = 250;
var g_ptt_pending_stop_timer = null;
var g_is_authenticated = false;
var g_should_connection_check_be_running = false;

// deep idle state (android background mode): only the websocket + slow heartbeat stay alive
var g_is_deep_idle = false;
var g_is_deep_idle_pending = false;


// g_is_come_from_idle_in_flight and g_come_from_idle_in_flight_timer exist
// to solve some timing problems around leaving and going to idle that appeared
// it makes sure client doesnt go to idle when he should not
var g_is_come_from_idle_in_flight = false;
var g_come_from_idle_in_flight_timer = null;

// call-accept presence grace: gentle idle waits this long after an accept before re-deciding,
// so the accept transition's blink of invisibility cannot idle the user out of the call
var G_CALL_ACCEPT_PRESENCE_GRACE_MS = 5000;
var g_presence_grace_until_timestamp = 0;
var g_presence_grace_recheck_timer = null;

function mark_come_from_idle_in_flight()
{
    g_is_come_from_idle_in_flight = true;

    if (g_come_from_idle_in_flight_timer != null)
    {
        clearTimeout(g_come_from_idle_in_flight_timer);
    }

    // if the answer never comes, give up after a while so idle is not blocked forever
    g_come_from_idle_in_flight_timer = setTimeout(function()
    {
        g_come_from_idle_in_flight_timer = null;
        g_is_come_from_idle_in_flight = false;
        console.log("connect-path: come-from-idle was never confirmed, idle allowed again");
        apply_pending_deep_idle_if_any();
    }, 20000);
}

function clear_come_from_idle_in_flight()
{
    if (g_come_from_idle_in_flight_timer != null)
    {
        clearTimeout(g_come_from_idle_in_flight_timer);
        g_come_from_idle_in_flight_timer = null;
    }

    g_is_come_from_idle_in_flight = false;

    apply_pending_deep_idle_if_any();
}

// runs an idle entry that enter_deep_idle deferred while a come-from-idle was unconfirmed.
// exit_deep_idle cancels the pending flag, so a foregrounded app never re-enters idle here
function apply_pending_deep_idle_if_any()
{
    if (g_is_deep_idle_pending == true && g_is_deep_idle == false)
    {
        g_is_deep_idle_pending = false;
        console.log("connect-path: running deferred idle entry");
        enter_deep_idle();
    }
}

// accepting a call blinks the screen off or hands the call screen over to the activity, and both
// read as "nobody is looking" exactly while the user IS there - which idled people right back out
// of the call they just answered. state lives with the other idle globals above
function mark_call_accept_presence_grace()
{
    g_presence_grace_until_timestamp = new Date().valueOf() + G_CALL_ACCEPT_PRESENCE_GRACE_MS;
    console.log("connect-path: call accepted, holding presence for " + G_CALL_ACCEPT_PRESENCE_GRACE_MS + "ms");
}

// one shot at grace end. node re-reads whether a ui is attached; the webview retries the gentle
// entry (when the user actually came back, exit_deep_idle cancelled this and nothing runs)
function schedule_presence_grace_recheck()
{
    if (g_presence_grace_recheck_timer != null)
    {
        return;
    }

    let wait_ms = Math.max(250, (g_presence_grace_until_timestamp - new Date().valueOf()) + 250);

    g_presence_grace_recheck_timer = setTimeout(function()
    {
        g_presence_grace_recheck_timer = null;

        if (typeof process !== "undefined")
        {
            node_apply_idle_for_ui_state();
        }
        else
        {
            enter_deep_idle(false);
        }
    }, wait_ms);
}

function cancel_presence_grace_recheck()
{
    if (g_presence_grace_recheck_timer != null)
    {
        clearTimeout(g_presence_grace_recheck_timer);
        g_presence_grace_recheck_timer = null;
    }
}
var g_connection_check_interval_ms = 10 * 1000;
// three missed 10s heartbeats + slack. the old 120s left the ui sitting in a
// fake "connected" state for two minutes after the network died under it
var g_connection_check_lost_threshold_ms = 35 * 1000;
var g_connection_check_sleep_resolve = null;

// session statistics for the strip themes' session-info card. bytes are counted at
// the two choke points every message passes through (encrypted base64 lengths, so
// "wire-ish" numbers); ping is the heartbeat round trip.
var g_session_bytes_sent = 0;
var g_session_bytes_received = 0;
var g_session_connected_at = 0;
var g_session_ping_sent_at = 0;
var g_session_last_ping_ms = -1;

// server-side policy: admins may register aliases (display names) on identities
var g_is_alias_registration_allowed = false;

// typing indicator. only alive when the server allows it. g_typing_state maps a chat
// context id to { client_id: expiry_timestamp_ms } - an entry that is not refreshed just
// expires, so a sender that disappears mid-sentence never leaves "x is typing" hanging
var g_is_typing_indicator_allowed = false;
var g_typing_state = {};
var g_typing_last_sent_at = 0;
var g_typing_render_timer = null;
var G_TYPING_SEND_INTERVAL_MS = 3000;   // at most one message per 3s of continuous typing
var G_TYPING_EXPIRY_MS = 6000;          // twice the send interval, so one lost message does not flicker

var g_offline_client_list = []; // might be supported by the server, might not
var current_channel_id = 0;
var g_current_chat_context_id = "chat-context-channel-0";
var g_chat_message_receiver_type = "channel";
var chat_message_receiver_id = "main";
var g_textarea_log = null;
var g_my_rsa_key_object = null;
var g_rsa_public_key_string = "";
var g_chat_message_author_public_keys = {}; // server chat message id -> author's public key; used to decide whether to honour an incoming delete/edit
var identity_string = "";
var g_pending_identity_file_string = ""; // passphrase read from a picked .lmn file, waiting for its confirm click
var g_is_rsa_key_generated = false;
var g_is_client_list_retrieved = false;
var g_is_channel_list_retrieved = false;
var g_map_client_id_to_array_index = new Map();
var g_client_list = [];
var g_icons = [];
var g_tags = [];
var g_icon_upload_queue = [];             // base64 g_icons waiting to be uploaded one at a time
var g_icon_upload_in_flight_base64 = null; // the icon whose server reply we are currently waiting for

// uploads the next queued icon unless one is still in flight; the reply handler clears
// g_icon_upload_in_flight_base64 and calls this again for the rest of the queue.
// it touches no dom, so it lives here rather than on UI - behind the ui proxy a batch
// upload stopped after the first icon, because nothing re-armed the pump
function send_next_queued_icon_upload()
{
    if (g_icon_upload_in_flight_base64 != null) // still waiting for the previous upload's reply
    {
        return;
    }
    if (g_icon_upload_queue.length == 0)
    {
        return;
    }

    let base64_icon = g_icon_upload_queue.shift();
    g_icon_upload_in_flight_base64 = base64_icon;

    let message_object = {
        message: {
            type: "server_settings_icon_upload",
            base64_icon_value: base64_icon
        }
    };

    send_message_object(message_object);
}
var g_settings_delete_delegation_wired = false;
var g_tag_icon_picker_target_tag_id = null; // tag whose icon the currently open picker will change
var g_channel_icon_picker_target_channel_id = null; // channel whose icon the currently open picker will change (the icon picker is shared between g_tags and channels)
var g_channel_properties_edit_channel_id = null; // the channel currently open in the edit form (null while creating); the form's icon box targets it
var g_channel_list = [];
// internal sentinel meaning "the root level" while walking the channel tree.
// root channels are identified by their is_root_channel flag, never by this value
// (server ids are uint64 and can no longer carry -1).
const g_ROOT_LEVEL_PARENT_SENTINEL = -1;
var g_is_chat_hidden = false;
// optional per-theme extras: flat channel list + a live right-pane member list
var g_is_channel_list_flattened = false;
var g_member_list_observer = null;
var g_member_list_sync_scheduled = false;
// avatars (server opt-in via __SERVER_CONFIG__.allow_avatars). cache maps client_id -> base64
// data-url; the queue drives chunked lazy loading; g_profile_avatar_client_id is the client whose
// avatar belongs in the big right-pane #current-client-avatar.
var g_avatars_allowed = false;
var g_avatar_max_upload_bytes = 51200;
var g_profile_avatar_client_id = -1;
// true only when the discord-style member list (avatar grid) is shown: gates painting avatars
// into the small round circles (in other themes that circle is the mic-state icon). the big
// right-pane #current-client-avatar shown on click is NOT gated on this - it works in all themes.
var g_avatar_grid_visible = false;
var g_avatar_load_queue = [];
var g_avatar_load_scheduled = false;

// AVATAR PREFETCH (option). on by default: after joining, quietly ask for every connected
// person's avatar one at a time so faces are there before anybody is clicked.
var g_android_app_mode = "";

// the android wrapper's simple/advanced mode drives the theme, both ways. simple locks the
// messenger look; advanced restores the user's saved theme (or the device default). called at startup
function apply_theme_for_app_mode()
{
    // simple = the bluebell messenger look, locked
    if (g_android_app_mode == "simple")
    {
        UI.apply_theme("bluebell", false);
        return;
    }

    // advanced = the full mobile interface. honor a theme the user picked manually (so
    // their choice persists), otherwise fall back to default-mobile
    if (g_android_app_mode == "advanced")
    {
        let saved_theme = null;
        try { saved_theme = localStorage.getItem("lemon_theme"); } catch (e) {}

        if (saved_theme != null && saved_theme != "bluebell" && UI.is_theme_allowed_on_this_device(saved_theme))
        {
            UI.apply_theme(saved_theme, false);
        }
        else
        {
            UI.apply_theme("default-mobile", false);
        }
    }
}

// pending AudioContext.suspend() from idle entry; exit chains its resume on it so a
// late-settling suspend can never land after the resume and g_silence the graph
var g_audio_suspend_promise = null;

var g_prefetch_avatars_automatically = true;
var g_avatar_prefetch_interval_ms = 300;
var g_avatar_prefetch_queue = [];
var g_avatar_prefetch_timer = null;
var local_message_id = 0;
var g_selected_font = "custom-font-usage-default";
var g_selected_font_color = "#ffffff";
var g_selected_font_size = 12;
var file_send_intent = "";
var file_send_intent_extra_data = {};
var g_is_file_being_uploaded = false;
var g_picture_delivery_pending = false; // between the server's upload ack and image_sent_status (relay to the receivers finished)
var g_picture_delivery_hide_timer = null;
var g_is_client_running_under_touch_device = false;  // for touch devices
var g_client_volume_by_id = {};  // local per-client playback volume (client_id -> gain, 1.0 = default); worklet mode only, never sent to the server
var g_is_running_in_android_webview = false;
var g_is_microphone_enabled_on_touch_device = false;  // for touch devices
var g_is_pressing = null;  // for touch devices
var g_is_long_press = null;

var g_received_files = []; // in chunks

const AUDIO_STATE = {
    PUSH_TO_TALK_ACTIVE: 1,
    PUSH_TO_TALK_ENABLED: 2,
    PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS: 3,
    AUDIO_COMPLETELY_DISABLED: 4
};

// save timestamp of last received connection check response (loss of connectivity is detected this way, websocket api cannot report loss of connection accurately. This value is set first time when client connects to server
var g_connection_check_message_response_received_timestamp = 0;

var g_loading_gif = "data:image/gif;base64,R0lGODlhIAAgALMAAP///7Ozs/v7+9bW1uHh4fLy8rq6uoGBgTQ0NAEBARsbG8TExJeXl/39/VRUVAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQFBQAAACwAAAAAIAAgAAAE5xDISSlLrOrNp0pKNRCdFhxVolJLEJQUoSgOpSYT4RowNSsvyW1icA16k8MMMRkCBjskBTFDAZyuAEkqCfxIQ2hgQRFvAQEEIjNxVDW6XNE4YagRjuBCwe60smQUDnd4Rz1ZAQZnFAGDd0hihh12CEE9kjAEVlycXIg7BAsMB6SlnJ87paqbSKiKoqusnbMdmDC2tXQlkUhziYtyWTxIfy6BE8WJt5YEvpJivxNaGmLHT0VnOgGYf0dZXS7APdpB309RnHOG5gDqXGLDaC457D1zZ/V/nmOM82XiHQjYKhKP1oZmADdEAAAh+QQFBQAAACwAAAAAGAAXAAAEchDISasKNeuJFKoHs4mUYlJIkmjIV54Soypsa0wmLSnqoTEtBw52mG0AjhYpBxioEqRNy8V0qFzNw+GGwlJki4lBqx1IBgjMkRIghwjrzcDti2/Gh7D9qN774wQGAYOEfwCChIV/gYmDho+QkZKTR3p7EQAh+QQFBQAAACwBAAAAHQAOAAAEchDISWdANesNHHJZwE2DUSEo5SjKKB2HOKGYFLD1CB/DnEoIlkti2PlyuKGEATMBaAACSyGbEDYD4zN1YIEmh0SCQQgYehNmTNNaKsQJXmBuuEYPi9ECAU/UFnNzeUp9VBQEBoFOLmFxWHNoQw6RWEocEQAh+QQFBQAAACwHAAAAGQARAAAEaRDICdZZNOvNDsvfBhBDdpwZgohBgE3nQaki0AYEjEqOGmqDlkEnAzBUjhrA0CoBYhLVSkm4SaAAWkahCFAWTU0A4RxzFWJnzXFWJJWb9pTihRu5dvghl+/7NQmBggo/fYKHCX8AiAmEEQAh+QQFBQAAACwOAAAAEgAYAAAEZXCwAaq9ODAMDOUAI17McYDhWA3mCYpb1RooXBktmsbt944BU6zCQCBQiwPB4jAihiCK86irTB20qvWp7Xq/FYV4TNWNz4oqWoEIgL0HX/eQSLi69boCikTkE2VVDAp5d1p0CW4RACH5BAUFAAAALA4AAAASAB4AAASAkBgCqr3YBIMXvkEIMsxXhcFFpiZqBaTXisBClibgAnd+ijYGq2I4HAamwXBgNHJ8BEbzgPNNjz7LwpnFDLvgLGJMdnw/5DRCrHaE3xbKm6FQwOt1xDnpwCvcJgcJMgEIeCYOCQlrF4YmBIoJVV2CCXZvCooHbwGRcAiKcmFUJhEAIfkEBQUAAAAsDwABABEAHwAABHsQyAkGoRivELInnOFlBjeM1BCiFBdcbMUtKQdTN0CUJru5NJQrYMh5VIFTTKJcOj2HqJQRhEqvqGuU+uw6AwgEwxkOO55lxIihoDjKY8pBoThPxmpAYi+hKzoeewkTdHkZghMIdCOIhIuHfBMOjxiNLR4KCW1ODAlxSxEAIfkEBQUAAAAsCAAOABgAEgAABGwQyEkrCDgbYvvMoOF5ILaNaIoGKroch9hacD3MFMHUBzMHiBtgwJMBFolDB4GoGGBCACKRcAAUWAmzOWJQExysQsJgWj0KqvKalTiYPhp1LBFTtp10Is6mT5gdVFx1bRN8FTsVCAqDOB9+KhEAIfkEBQUAAAAsAgASAB0ADgAABHgQyEmrBePS4bQdQZBdR5IcHmWEgUFQgWKaKbWwwSIhc4LonsXhBSCsQoOSScGQDJiWwOHQnAxWBIYJNXEoFCiEWDI9jCzESey7GwMM5doEwW4jJoypQQ743u1WcTV0CgFzbhJ5XClfHYd/EwZnHoYVDgiOfHKQNREAIfkEBQUAAAAsAAAPABkAEQAABGeQqUQruDjrW3vaYCZ5X2ie6EkcKaooTAsi7ytnTq046BBsNcTvItz4AotMwKZBIC6H6CVAJaCcT0CUBTgaTg5nTCu9GKiDEMPJg5YBBOpwlnVzLwtqyKnZagZWahoMB2M3GgsHSRsRACH5BAUFAAAALAEACAARABgAAARcMKR0gL34npkUyyCAcAmyhBijkGi2UW02VHFt33iu7yiDIDaD4/erEYGDlu/nuBAOJ9Dvc2EcDgFAYIuaXS3bbOh6MIC5IAP5Eh5fk2exC4tpgwZyiyFgvhEMBBEAIfkEBQUAAAAsAAACAA4AHQAABHMQyAnYoViSlFDGXBJ808Ep5KRwV8qEg+pRCOeoioKMwJK0Ekcu54h9AoghKgXIMZgAApQZcCCu2Ax2O6NUud2pmJcyHA4L0uDM/ljYDCnGfGakJQE5YH0wUBYBAUYfBIFkHwaBgxkDgX5lgXpHAXcpBIsRADs=";
var selected_channel_id;
var selected_client_id;
var current_channel_keys = null;

// channel encryption keys are changed when somebody new joins the channel with clients already in it.
// that creates the situation where somebody send the channel chat message encrypted with old channel keys but receiver expects the new ones
// g_historic_keys_of_current_channel allows clients to try to decrypt incoming image with the old ones
// g_historic_keys_of_current_channel gets reset if local client switches channel
// its the data processing worker that does the work with historic channel keys, main UI thread does not need it

var g_historic_keys_of_current_channel = [];

var g_chat_context_array = [
    {
        type: "channel",
        chat_context_id: "chat-context-channel-0",
        last_known_message_sender_username: ""
    }
];

var g_opus_encoder_worker = null;
var g_opus_decoder_worker = null;
var g_data_processing_worker = null;
var g_minimp3_worker = null;

var g_websocket_worker = null;

// for voice chat

var g_peer_connection_with_server = null;

var g_iceconfig = null;

var g_is_voice_chat_allowed_by_server = false;   // audio subsystem active (datachannel kept up); true when client voice OR music-bot audio is on
var g_is_client_microphone_allowed_by_server = false;   // may this client transmit its own mic; true only when client voice is on
var g_local_audio_stream = null;
var g_is_microphone_enabled = false;
var g_is_microphone_active = false;
var g_last_sent_value_microphone_usage = false;

var g_is_server_settings_tab_visible = false;

var g_audio_config = {
    codec: {
        bufferSize: 16384 / 2
    }
};

var g_selected_song_name = null;

var g_datachannel = null;

var g_opus_decoding_sampler = null;
var g_opus_encoding_sampler = null;

var g_decoder = null;
var g_silence = null;
var audio_context = null;
var g_encoder = null;
var g_microphone_recorder = null;
var player = null;
var g_audio_player_worklet_node = null;
var g_is_audio_worklet_player_active = false;
var g_audio_worklet_module_promise = null; // set while/after the worklet module loads; null = no worklet support
var g_is_microphone_worklet_active = false;
var g_PREMIXED_AUDIO_LANE_ID = 9999999; // worklet lane id for already-mixed audio (matches PREMIXED_LANE_ID in the processor)

var g_audio_player_gain_node = null;
var g_audio_recorder_gain_node = null;
var g_audio_input = null;

// =============================================================================
// channel maintainer reset - last-resort recovery for a channel whose announced
// maintainer never delivers usable channel keys (faulty/modified client or a
// half-dead connection). whenever this client ends up WAITING for keys (own
// channel join, maintainer succession, somebody joining the channel), a timer is
// armed; if no valid keys arrive within the timeout, a "reset_channel_maintainer"
// vote is sent to the server and re-sent every timeout period while still keyless.
// the request carries NO payload - the server does all vote bookkeeping internally
// (one vote per client, votes die on maintainer change / channel switch) and once
// more than half of the channel's clients have voted, it deposes the maintainer,
// announces a new one, and key distribution restarts normally.
// =============================================================================
var g_MAINTAINER_KEYS_WAIT_TIMEOUT_MS = 5000;
var g_maintainer_keys_wait_timer = null;
var g_maintainer_keys_wait_channel_id = -1;

// arms (re-arms) the keys-wait timer for the current channel. no-op when there is
// nothing to wait for: no maintainer announced, or the local user IS the maintainer
function arm_maintainer_keys_wait_timer()
{
    // node owns every reactive protocol decision, this one included. the webview voting
    // too churned the maintainer in a loop and invalidated the keys it had just applied
    if (is_ui_only_runtime())
    {
        return;
    }

    let channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, current_channel_id);

    cancel_maintainer_keys_wait_timer();

    if (channel_index == -1)
    {
        return;
    }

    if (!g_channel_list[channel_index].has_maintainer)
    {
        return;
    }

    if (g_channel_list[channel_index].maintainer_id == local_client_id)
    {
        return;
    }

    g_maintainer_keys_wait_channel_id = current_channel_id;
    g_maintainer_keys_wait_timer = setTimeout(maintainer_keys_wait_timer_fired, g_MAINTAINER_KEYS_WAIT_TIMEOUT_MS);
}

// stops the pending keys-wait timer, if any, and clears g_maintainer_keys_wait_timer
function cancel_maintainer_keys_wait_timer()
{
    if (g_maintainer_keys_wait_timer != null)
    {
        clearTimeout(g_maintainer_keys_wait_timer);
        g_maintainer_keys_wait_timer = null;
    }
}

// fires when the maintainer stayed silent for the whole timeout. every condition is
// re-checked because the world may have moved on during those seconds: channel switched,
// keys arrived (timer normally cancelled, this is belt and suspenders), local user became
// the maintainer, or a new maintainer generation was announced (which armed a fresh timer)
function maintainer_keys_wait_timer_fired()
{
    g_maintainer_keys_wait_timer = null;

    if (g_maintainer_keys_wait_channel_id != current_channel_id)
    {
        return;
    }

    if (current_channel_keys != null)
    {
        return;
    }

    let channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, current_channel_id);

    if (channel_index == -1)
    {
        return;
    }

    if (!g_channel_list[channel_index].has_maintainer)
    {
        return;
    }

    if (g_channel_list[channel_index].maintainer_id == local_client_id)
    {
        return;
    }

    console.log("%c no channel keys from maintainer (id " + g_channel_list[channel_index].maintainer_id + ") after " + g_MAINTAINER_KEYS_WAIT_TIMEOUT_MS + " ms, sending reset_channel_maintainer vote", "color: red");

    // payload-free: the client just asks for its channel's maintainer to be reset,
    // the server does all vote bookkeeping (one vote per client, dies on maintainer change)
    let message_object = {
        message: {
            type: "reset_channel_maintainer"
        }
    };

    send_message_object(message_object);

    // keep voting while still keyless: the server counts at most one vote per client,
    // so repeats are harmless and cover a lost request on the way
    g_maintainer_keys_wait_timer = setTimeout(maintainer_keys_wait_timer_fired, g_MAINTAINER_KEYS_WAIT_TIMEOUT_MS);
}

var g_audio_queue = {
    buffer: new Float32Array(0),

    // AUDIO TUNABLE (fallback path only): this queue feeds the ScriptProcessor used when the
    // AudioWorklet is unavailable. On the normal worklet path the jitter buffer in
    // LemonPlayerProcessor ("AUDIO WORKLET TUNABLES") governs playback instead.
    // hard cap on buffered playback (~300 ms of 48 kHz INTERLEAVED STEREO, two floats per frame).
    // anything beyond this is accumulated delay the listener never catches up with (decode output
    // outpacing playback after gc pauses, tab throttling, or a sample-rate mismatch). when the
    // backlog exceeds the cap the oldest samples are dropped, so playback snaps back to
    // near-real-time instead of lagging forever
    max_buffered_samples: 28800,

    write: function (newAudio)
    {
        var currentQLength = this.buffer.length;
        var newBuffer = new Float32Array(currentQLength + newAudio.length);
        newBuffer.set(this.buffer, 0);
        newBuffer.set(newAudio, currentQLength);

        if (newBuffer.length > this.max_buffered_samples)
        {
            newBuffer = newBuffer.subarray(newBuffer.length - this.max_buffered_samples);
        }

        this.buffer = newBuffer;
    },

    clear: function ()
    {
        this.buffer = g_silence;
    },

    read: function (nSamples)
    {
        var samplesToPlay = this.buffer.subarray(0, nSamples);
        this.buffer = this.buffer.subarray(nSamples, this.buffer.length);
        return samplesToPlay;
    },

    length: function ()
    {
        return this.buffer.length;
    }
};

// global mousedown: closes the chat-message context menu, and every other context menu
// unless the click landed on a menu item (so the item's own click handler still runs)
function document_onmousedown(event)
{
    UI.delete_chat_message_contextmenu();

    let is_channel_list_contextmenu_delete_needed = !event.target.classList.contains("context-menu-item");

    if (is_channel_list_contextmenu_delete_needed)
    {
        UI.delete_contextmenus(true);
    }
}

// --- avatars: apply/cache/request helpers (server opt-in via g_avatars_allowed) ---
function apply_avatar_to_ui(client_id, base64)
{
    if (client_id === undefined || client_id === null) { return; }

    let has_avatar = (typeof base64 === "string" && base64.length > 0);

    // keep the avatar ON the client object in g_client_list, so it travels with the client and is
    // re-applied by avatar_inline_style_for_client on every tree render (channel switch etc.)
    let client_object = get_client_by_client_id(client_id);
    if (client_object != null) { client_object.base64_avatar = has_avatar ? base64 : null; }

    // paint it straight onto the tree row's circle as well, so a new or cleared avatar shows
    // up without waiting for the next full tree render
    // offer it to the tree row the same way the renderer does: a variable + marker, never a
    // painted background (that circle is the mic-state icon in most themes)
    let tree_circle = document.getElementById("client-audio-state-" + client_id);
    if (tree_circle != null)
    {
        if (has_avatar == true && g_avatars_allowed == true)
        {
            tree_circle.style.setProperty("--client-avatar", "url(" + base64 + ")");
            tree_circle.setAttribute("data-has-avatar", "1");
        }
        else
        {
            tree_circle.style.removeProperty("--client-avatar");
            tree_circle.removeAttribute("data-has-avatar");
        }
    }

    // the member list is a clone of the tree, so repaint it too
    if (g_avatar_grid_visible == true) { UI.schedule_member_list_sync(); }

    // the big avatar in the right-pane profile, only if this is the client shown there
    if (parseInt(g_profile_avatar_client_id) === parseInt(client_id))
    {
        let big = document.getElementById("current-client-avatar");
        if (big != null)
        {
            if (has_avatar)
            {
                big.style.backgroundImage = "url(" + base64 + ")";
                big.style.backgroundSize = "100% 100%";

                // a picture replaces the letter that was standing in for it
                big.textContent = "";
                big.classList.remove("avatar-empty");
            }
            else
            {
                big.style.backgroundImage = "";
            }
        }
    }
}

// saved server configurations, kept on this device only: [{ name, host, port }]
function read_server_bookmarks()
{
    try
    {
        let stored = JSON.parse(localStorage.getItem("lemon_server_bookmarks"));
        return Array.isArray(stored) ? stored : [];
    }
    catch (e) { return []; }
}

function write_server_bookmarks(bookmarks)
{
    try { localStorage.setItem("lemon_server_bookmarks", JSON.stringify(bookmarks)); } catch (e) { }
}

// repaints the dropdown; the option value is the index into the stored array
function render_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null) { return; }

    let bookmarks = read_server_bookmarks();
    select_element.innerHTML = "";

    let placeholder = document.createElement("option");
    placeholder.value = "";
    placeholder.textContent = (bookmarks.length > 0) ? "saved servers" : "no saved servers";
    select_element.appendChild(placeholder);

    for (let x = 0; x < bookmarks.length; x++)
    {
        let option = document.createElement("option");
        option.value = "" + x;
        option.textContent = bookmarks[x].name + " (" + bookmarks[x].host + ":" + bookmarks[x].port + ")";
        select_element.appendChild(option);
    }
}

function wire_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null) { return; }

    render_server_bookmarks();

    // picking one loads it into the address and port fields, ready to connect
    select_element.onchange = function()
    {
        if (this.value === "") { return; }

        let bookmark = read_server_bookmarks()[parseInt(this.value)];

        if (bookmark == null) { return; }

        document.getElementById("input-ip-address").value = bookmark.host;
        document.getElementById("input-port-number").value = bookmark.port;
        document.getElementById("server-bookmark-name").value = bookmark.name;
    };

    // saving under a name that already exists overwrites it, so this doubles as "update"
    document.getElementById("server-bookmark-save").onclick = function()
    {
        let name_element = document.getElementById("server-bookmark-name");
        let name = name_element.value.trim();
        let host = document.getElementById("input-ip-address").value.trim();
        let port = document.getElementById("input-port-number").value.trim();

        if (name.length === 0) { custom_alert("give this server a name first"); return; }
        if (host.length === 0 || port.length === 0) { custom_alert("fill in the address and port first"); return; }

        let bookmarks = read_server_bookmarks();
        let existing = -1;

        for (let x = 0; x < bookmarks.length; x++)
        {
            if (bookmarks[x].name.toLowerCase() === name.toLowerCase()) { existing = x; }
        }

        if (existing === -1) { bookmarks.push({ name: name, host: host, port: port }); }
        else { bookmarks[existing] = { name: name, host: host, port: port }; }

        write_server_bookmarks(bookmarks);
        render_server_bookmarks();
    };

    document.getElementById("server-bookmark-delete").onclick = function()
    {
        let selected = document.getElementById("server-bookmark-select").value;

        if (selected === "") { custom_alert("pick a saved server to delete"); return; }

        let bookmarks = read_server_bookmarks();
        bookmarks.splice(parseInt(selected), 1);

        write_server_bookmarks(bookmarks);
        render_server_bookmarks();
        document.getElementById("server-bookmark-name").value = "";
    };
}

// the first letter, shown in the avatar circle while there is no picture in it
function set_profile_avatar_monogram(username)
{
    let big = document.getElementById("current-client-avatar");

    if (big == null) { return; }

    big.textContent = (typeof username === "string" && username.length > 0) ? username.charAt(0) : "?";
    big.classList.add("avatar-empty");
}

// the profile line under the name. it replaced a hardcoded "time active: 0 minutes" - the
// server sends no connect timestamp, so this says only things the client list actually knows
function describe_client_for_profile(client)
{
    let parts = [];

    if (client.client_id === local_client_id) { parts.push("you"); }
    if (client.is_music_bot == true) { parts.push("music bot"); }

    let channel = get_channel_by_id(g_channel_list, client.channel_id);

    if (channel != null) { parts.push("in " + channel.name); }

    if (client.is_idle == true) { parts.push("idle"); }
    if (client.alias != null && client.alias.length > 0) { parts.push("known as " + client.alias); }
    if (client.country_iso_code != null && client.country_iso_code.length > 0) { parts.push(client.country_iso_code.toUpperCase()); }
    if (client.is_muted_by_local_client == true) { parts.push("muted by you"); }
    if (client.is_ignored_by_local_client == true) { parts.push("ignored by you"); }

    return parts.join(" · ");
}

// asks the server for one client's avatar; no-op when avatars are disabled
function request_single_avatar(client_id)
{
    if (g_avatars_allowed == false || client_id === undefined || client_id === null) { return; }
    let message_object = { message: { type: "request_avatar_for_client", client_id: parseInt(client_id) } };
    send_message_object(message_object);
}

// chunked lazy load: enqueue every connected client id, then pull avatars in growing chunks
// (50, then 100, then 150) so joining a busy server doesn't request everything at once.
function enqueue_all_avatars_for_loading()
{
    // only bulk-load everyone's avatar for the discord-style member list (shown all at once).
    // other themes only ever fetch the clicked client's avatar for the big right-pane.
    if (g_avatars_allowed == false || g_avatar_grid_visible == false) { return; }
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null) { g_avatar_load_queue.push(g_client_list[i].client_id); }
    }
    if (g_avatar_load_scheduled == false) { pump_avatar_load_queue(50); }
}

// queues every connected person whose avatar we do not have yet. safe to call repeatedly -
// already-queued and already-known ids are skipped, and the drain timer is started once.
function start_avatar_prefetch()
{
    if (g_prefetch_avatars_automatically == false || g_avatars_allowed == false) { return; }

    // the strip themes (simpledark/bluebell) show everybody at once and already bulk-load
    // through enqueue_all_avatars_for_loading; prefetching on top would double the requests
    if (g_avatar_grid_visible == true) { return; }

    for (let i = 0; i < g_client_list.length; i++)
    {
        let client_object = g_client_list[i];

        if (client_object == null) { continue; }
        if (client_object.client_id == local_client_id) { continue; }
        if (client_object.is_music_bot == true) { continue; }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0) { continue; }
        if (g_avatar_prefetch_queue.indexOf(client_object.client_id) != -1) { continue; }

        g_avatar_prefetch_queue.push(client_object.client_id);
    }

    if (g_avatar_prefetch_timer != null || g_avatar_prefetch_queue.length == 0) { return; }

    // one request per tick, so a room full of people never arrives as a burst
    g_avatar_prefetch_timer = setInterval(function()
    {
        if (g_avatars_allowed == false || g_avatar_prefetch_queue.length == 0)
        {
            stop_avatar_prefetch();
            return;
        }

        let client_id = g_avatar_prefetch_queue.shift();
        let client_object = get_client_by_client_id(client_id);

        // they left, or their avatar arrived some other way while waiting in the queue
        if (client_object == null) { return; }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0) { return; }

        request_single_avatar(client_id);
    }, g_avatar_prefetch_interval_ms);
}

// stops the prefetch drain timer and empties g_avatar_prefetch_queue
function stop_avatar_prefetch()
{
    if (g_avatar_prefetch_timer != null)
    {
        clearInterval(g_avatar_prefetch_timer);
        g_avatar_prefetch_timer = null;
    }

    g_avatar_prefetch_queue.length = 0;
}

// drains g_avatar_load_queue: requests one chunk of avatars now, then re-schedules itself
// every 400ms with a growing chunk size (max 150) until the queue is empty
function pump_avatar_load_queue(chunk_size)
{
    if (g_avatars_allowed == false || g_avatar_load_queue.length === 0)
    {
        g_avatar_load_scheduled = false;
        return;
    }

    let ids = g_avatar_load_queue.splice(0, chunk_size);
    let message_object = { message: { type: "request_avatars", client_ids: ids } };
    send_message_object(message_object);

    if (g_avatar_load_queue.length > 0)
    {
        g_avatar_load_scheduled = true;
        let next_chunk = Math.min(chunk_size + 50, 150);
        setTimeout(function() { pump_avatar_load_queue(next_chunk); }, 400);
    }
    else
    {
        g_avatar_load_scheduled = false;
    }
}

// push-to-talk press: enables the mic track, connects the recorder chain and reports
// microphone_usage=1 to the server; no-op when the mic is off, forbidden or already sending
function process_start_sending_audio()
{
    // a re-press inside the release hangover cancels the pending stop: the mic never
    // went down, so the early-outs below just keep the running capture untouched
    if (g_ptt_pending_stop_timer != null)
    {
        clearTimeout(g_ptt_pending_stop_timer);
        g_ptt_pending_stop_timer = null;
    }

    if (g_is_microphone_enabled == false)
    {
        return;
    }
    if (g_is_client_microphone_allowed_by_server == false)
    {
        return;
    }

    if (g_last_sent_value_microphone_usage == true)
    {
        return;
    }
    if (g_local_audio_stream == null)
    {
        return;
    }

    if (g_local_audio_stream.getTracks() == null)
    {
        return;
    }

    // spurt-start marker: receivers scrub this sender's decoder on the jump, pairing with
    // the encoder reset that the capture-start clear below performs
    g_voice_send_sequence_number = (g_voice_send_sequence_number + OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP) & 0xffff;

    set_microphone_capture_active(true);
    set_mic_transmitting_visual(true);

    g_local_audio_stream.getTracks()[0].enabled = true;

    try {
        g_audio_recorder_gain_node.connect(g_microphone_recorder);
    } catch (e) {
        console.log("audio_recorder_gain_node.connect" + e.message);
    }
    // g_microphone_recorder.connect(audio_context.destination);

    g_is_microphone_active = true;
    let message_object = {
        message: {
            type: "microphone_usage",
            value: 1,
        }
    };

    send_message_object(message_object);
    g_last_sent_value_microphone_usage = 1;
}

// push-to-talk release: keeps capturing for a short hangover so the tail of the last
// word ships (and the mic-off message cannot outrun the final audio frames), then stops
// glow on the mic controls while actually transmitting, so the user knows the mic is hot
function set_mic_transmitting_visual(is_transmitting)
{
    let button_ids = ["microphone-push-to-talk-button-touch-device", "microphone-always-broadcasting-audio-button", "toggle-microphone-label"];

    for (let i = 0; i < button_ids.length; i++)
    {
        let button = document.getElementById(button_ids[i]);

        if (button != null)
        {
            if (is_transmitting == true) { button.classList.add("mic-transmitting"); }
            else { button.classList.remove("mic-transmitting"); }
        }
    }
}

function process_stop_sending_audio()
{
    if (g_ptt_pending_stop_timer != null)
    {
        clearTimeout(g_ptt_pending_stop_timer);
    }

    g_ptt_pending_stop_timer = setTimeout(function()
    {
        g_ptt_pending_stop_timer = null;
        process_stop_sending_audio_now();
    }, g_ptt_release_hangover_ms);
}

// the actual stop: disables the mic track (capture graph stays wired) and
// reports microphone_usage=2 to the server
function process_stop_sending_audio_now()
{
    // audio capture only exists in the webview; node crashed here on a missing symbol
    if (typeof set_microphone_capture_active !== "function")
    {
        return;
    }

    set_microphone_capture_active(false);
    set_mic_transmitting_visual(false);

    if (g_is_microphone_enabled == false)
    {
        return;
    }
    if (g_is_client_microphone_allowed_by_server == false)
    {
        return;
    }

    if (g_local_audio_stream == null)
    {
        return;
    }

    if (g_local_audio_stream.getTracks() == null)
    {
        return;
    }

    // activate recorder by assigning function to onaudioprocess

    g_local_audio_stream.getTracks()[0].enabled = false;

    // no edge of the capture graph may ever be disconnected: an unpulled mic source freezes stale
    // speech in chromium's stream fifo, and the next press replays it. the worklet gate mutes alone

    g_is_microphone_active = false;
    let message_object = {
        message: {
            type: "microphone_usage",
            value: 2,
        }
    };

    send_message_object(message_object);
    g_last_sent_value_microphone_usage = 2;

    // drop the encoder's half-filled frame, otherwise its stale samples lead the NEXT
    // transmission (a piece of the previous sentence replayed on the new press)
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
}

// push-to-talk key (Q unless changed in local settings): held down = speaking.
// in continuous mode the mic is toggled by the button instead, so the key does nothing
function document_onkeydown(event)
{
    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    if (event.which == g_push_to_talk_key_code)
    {
        process_start_sending_audio();
    }
}

// releasing it stops sending audio and clears the opus encoder's buffered frames
function document_onkeyup(event)
{
    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    if (event.which == g_push_to_talk_key_code)
    {
        // the encoder clear moved into process_stop_sending_audio_now: clearing here
        // would wipe the release-hangover tail that is still being captured
        process_stop_sending_audio();
    }
}

// promise-based delay, for awaiting inside async loops
function sleep(ms)
{
    return new Promise(resolve => window.setTimeout(resolve, ms));
}

// appends a timestamped line to the on-page log textarea; workers (no textarea) post the
// text to the main thread instead, and touch devices skip logging entirely
function custom_log(text)
{
    // g_textarea_log only exists in main thread
    // in case its null, use global log

    if (g_textarea_log == null)
    {
        global.postMessage({
            type: "log",
            value: text
        });
        return;
    }

    // keep the log bounded (phones keep the page alive for days, and every
    // append copies the whole string): past ~50 KB keep only the newest half
    if (g_textarea_log.value.length > 50000)
    {
        g_textarea_log.value = g_textarea_log.value.slice(-25000);
    }

    // append via .value, never textContent: textContent is only the textarea's
    // DEFAULT value - the first manual edit detaches the display from it forever
    let aa = new Date();
    let a = aa.toLocaleTimeString();
    g_textarea_log.value += a + " | " + text + "\n";
}

// sleep used by the heartbeat loop; exit_deep_idle resolves it early so the first
// post-idle connection check is sent immediately instead of after a leftover long sleep
function connection_check_sleep(ms)
{
    return new Promise(function(resolve)
    {
        let timer_id = setTimeout(function()
        {
            g_connection_check_sleep_resolve = null;
            resolve();
        }, ms);

        // early-wake hook used by exit_deep_idle: cancel this sleep's timer so a stale
        // (long) timeout can never fire later and clobber a newer sleep's resolver
        g_connection_check_sleep_resolve = function()
        {
            clearTimeout(timer_id);
            g_connection_check_sleep_resolve = null;
            resolve();
        };
    });
}

// deep idle: entered when the android app goes to background. keeps the websocket alive on a slow
// heartbeat and shuts down everything else (audio graph, opus g_decoder tick, webrtc datachannel)
function enter_deep_idle(is_forced)
{
    // every refusal below says so. a silent one is indistinguishable from the request
    // never arriving, which is what made this so hard to pin down
    if (g_is_deep_idle == true)
    {
        console.log("connect-path: idle requested, already idle");
        return;
    }

    // an unconfirmed come-from-idle blocks idle entry, but the request must not be thrown
    // away: dropping it left the page awake while the server considered the client idle,
    // and every datachannel handshake attempted from that split state was doomed
    if (g_is_come_from_idle_in_flight == true)
    {
        console.log("connect-path: idle deferred, a come-from-idle is still unconfirmed");
        g_is_deep_idle_pending = true;
        return;
    }

    // app backgrounded before authentication finished - remember it, the auth success handler
    // re-enters (fixes the "went to background while still connecting" race)
    if (g_is_authenticated == false)
    {
        console.log("connect-path: idle requested while not authenticated, deferred to login");
        g_is_deep_idle_pending = true;
        return;
    }

    // inside the call-accept presence grace: hold, then re-decide from the real state.
    // a swipe-away (is_forced) stays deliberate and is honoured immediately
    if (is_forced != true && new Date().valueOf() < g_presence_grace_until_timestamp)
    {
        console.log("connect-path: idle deferred, inside the call-accept presence grace");
        schedule_presence_grace_recheck();
        return;
    }

    // backgrounding (home) keeps an in-channel session alive - music and calls continue.
    // only a swipe-away forces idle from any channel (is_forced, from onTaskRemoved)
    if (is_forced != true)
    {
        let local_client = get_client_by_client_id(local_client_id);

        if (local_client != null && local_client.channel_id != 0)
        {
            console.log("connect-path: idle refused, in channel " + local_client.channel_id + " not root");
            return;
        }
    }

    console.log("connect-path: going idle" + (is_forced == true ? " (forced)" : ""));

    g_is_deep_idle = true;

    client_msg.send_go_to_idle_mode_request();

    // heartbeat 10s -> 120s, loss detector 120s -> 360s (three missed checks)
    g_connection_check_interval_ms = 120 * 1000;
    g_connection_check_lost_threshold_ms = 360 * 1000;

    if (audio_context != null && audio_context.state === "running")
    {
        g_audio_suspend_promise = audio_context.suspend();
    }

    // a push-to-talk held down at background time must not keep capturing all idle long;
    // capture stays off until the next press. guarded: node has no audio at all, and
    // an unguarded call threw out of here, so headless node never actually went idle
    if (typeof set_microphone_capture_active === "function")
    {
        set_microphone_capture_active(false);
    }

    if (typeof set_mic_transmitting_visual === "function")
    {
        set_mic_transmitting_visual(false);
    }

    if (g_opus_decoder_worker != null)
    {
        g_opus_decoder_worker.postMessage({ type: "deep_idle_stop" });
    }

    // the datachannel's ICE consent checks ping the server every few seconds even when nothing
    // streams - close it, exit_deep_idle re-establishes it
    if (g_peer_connection_with_server != null)
    {
        try
        {
            g_peer_connection_with_server.close();
        }
        catch (e)
        {
            console.log("deep idle: peer connection close failed ", e.message);
        }
        g_peer_connection_with_server = null;
        g_datachannel = null;
        g_is_webrtc_datachannel_connected = false;
    }
}

// leaves deep idle: restores heartbeat cadence, audio graph, opus tick and the webrtc datachannel
function exit_deep_idle()
{
    // the user is demonstrably here: a queued presence-grace re-check must not fire behind them.
    // before the was-not-idle return, because the webview arms that timer without ever being idle
    cancel_presence_grace_recheck();

    g_is_deep_idle_pending = false;

    if (g_is_deep_idle == false)
    {
        console.log("connect-path: leave-idle requested, was not idle");
        return;
    }

    // we really were idle, so an answer from the server is coming. armed here rather
    // than where the request is sent, because a request sent while not idle gets none
    mark_come_from_idle_in_flight();

    console.log("connect-path: leaving idle");

    g_is_deep_idle = false;

    g_connection_check_interval_ms = 10 * 1000;
    g_connection_check_lost_threshold_ms = 35 * 1000; // keep in sync with the startup value
    g_connection_check_message_response_received_timestamp = new Date().valueOf();

    // wake the heartbeat loop out of its long idle sleep so a fresh check goes out now
    if (g_connection_check_sleep_resolve != null)
    {
        g_connection_check_sleep_resolve();
        g_connection_check_sleep_resolve = null;
    }

    if (audio_context != null)
    {
        // wait for idle entry's suspend to settle before acting, so the two can never race
        let suspend_settled = (g_audio_suspend_promise != null) ? g_audio_suspend_promise : Promise.resolve();
        g_audio_suspend_promise = null;

        suspend_settled.catch(function() {}).then(function()
        {
            // android resets the audio HAL under a backgrounded webview; resume() then
            // "succeeds" into a broken graph (gurgling both ways), so rebuild instead
            if (g_android_app_mode != "")
            {
                rebuild_audio_graph_after_idle();
                return;
            }

            // browsers: resume, but verify it took - rebuild when the context stays stuck
            audio_context.resume().catch(function(e) { console.log("audio resume rejected: " + e.message); });
            setTimeout(function()
            {
                if (audio_context.state !== "running")
                {
                    console.log("audio context stuck in '" + audio_context.state + "' after resume, rebuilding");
                    rebuild_audio_graph_after_idle();
                }
            }, 1000);
        });
    }

    if (g_opus_decoder_worker != null)
    {
        g_opus_decoder_worker.postMessage({ type: "deep_idle_resume" });
    }

    // re-establish the webrtc datachannel that idle entry closed
    if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_webrtc_datachannel_check_running == false)
    {
        g_is_webrtc_datachannel_check_running = true;
        webrtc_datachannel_connection_check(true);
    }
}

// ping loop: sends client_connection_check messages on an interval; if no response
// arrives within the lost threshold, alerts the user and resets the app (identity kept)
async function websocket_connection_check(websocket)
{
    while (g_should_connection_check_be_running == true)
    {
        // one throw in here must not kill the loop: it carries BOTH the heartbeat and
        // the loss detector, and a dead loop is an unnoticed dead connection
        try
        {
            let timestamp_now = new Date().valueOf();

            if (g_connection_check_message_response_received_timestamp == 0)
            {
                g_connection_check_message_response_received_timestamp = timestamp_now;
            }

            if ((g_connection_check_message_response_received_timestamp + g_connection_check_lost_threshold_ms) < timestamp_now)
            {
                custom_alert("connection with server lost");
                console.error("loss detector: no heartbeat response for " + (timestamp_now - g_connection_check_message_response_received_timestamp) + "ms, resetting");
                reset_chat_app_keep_identity();
            }

            g_session_ping_sent_at = timestamp_now; // the response handler turns this into the round-trip time

            let message_object = {
                message: {
                    type: "client_connection_check",
                }
            };

            send_message_object(message_object);
        }
        catch (checker_error)
        {
            console.error("connection check iteration failed: " + (checker_error != null && checker_error.stack ? checker_error.stack : checker_error));
        }

        await connection_check_sleep(g_connection_check_interval_ms);
    }
}

// this function is started when client detects datachannel connection gets cancelled
// this function tried to re-establish rtc connection
// the "disable non-proxied UDP" webrtc policy (chrome flag / WebRTC Network Limiter
// extension / enterprise policy) cannot be read from a page, but it is reliably
// inferable: under it, ice gathering yields NO udp candidates at all, while any
// normal browser produces at least a local host candidate without even needing a
// stun server. runs once, and only after a datachannel attempt already failed
function probe_webrtc_udp_and_warn()
{
    if (g_webrtc_udp_probe_done == true)
    {
        return;
    }
    g_webrtc_udp_probe_done = true;

    try
    {
        let probe_pc = new RTCPeerConnection();
        let got_udp_candidate = false;

        probe_pc.onicecandidate = function(event)
        {
            if (event.candidate != null && event.candidate.candidate.toLowerCase().indexOf(" udp ") != -1)
            {
                got_udp_candidate = true;
            }
        };

        probe_pc.createDataChannel("udp-probe");
        probe_pc.createOffer().then(function(offer) { return probe_pc.setLocalDescription(offer); });

        window.setTimeout(function()
        {
            probe_pc.close();

            if (got_udp_candidate == false)
            {
                let reason = "audio data blocked: the browser refuses direct UDP - possibly a chrome://flags setting like 'disable non-proxied UDP'. this is just a UDP websocket to your server, never a connection to other users";
                console.warn(reason);
                custom_log(reason);
                custom_alert(reason);
            }
        }, 3000);
    }
    catch (probe_error)
    {
        console.warn("webrtc udp probe could not run: " + probe_error.message);
    }
}

async function webrtc_datachannel_connection_check(is_this_reconnect = false)
{
    // voice lives in the webview only; node has no webrtc and must never enter this.
    // the caller set the running flag, so it must be dropped here too
    if (typeof RTCPeerConnection === "undefined")
    {
        g_is_webrtc_datachannel_check_running = false;
        return;
    }

    // unordered, unreliable datachannel with maxretransmits set to 0
    // basically UDP websocket, where one peer is server and the other peer is client
    // its just between server and client, client doesnt connect to other clients
    // so it doesnt have to be considered as peer to peer

    while (true)
    {
        console.log("webrtc_datachannel_connection_check ran");

        if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
        {
            // one throw here must not kill the loop: a dead loop leaves the running flag
            // stranded true, which blocks every future re-create until the app restarts
            try
            {
                create_new_peer_connection_object_for_use(is_this_reconnect);

                let message_object = {
                    message: {
                        type: "create_new_webrtc_datachannel_connection",
                    }
                };

                send_message_object(message_object);
            }
            catch (datachannel_attempt_error)
            {
                console.error("webrtc datachannel attempt failed: "
                    + (datachannel_attempt_error != null && datachannel_attempt_error.stack ? datachannel_attempt_error.stack : datachannel_attempt_error));
            }

            await sleep(10000);

            // a full attempt window passed and the channel is still down: find out
            // whether the browser is even capable of direct udp before retrying forever
            if (g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
            {
                probe_webrtc_udp_and_warn();
            }
        }
        else
        {
            console.log("webrtc_datachannel_connection_check break");
            g_is_webrtc_datachannel_check_running = false;
            break;
        }
    }
}

// the size this server last asked for, so the offer is not repeated on every redial. the
// connection driver retries forever, and the rejection repeats on every attempt
var g_rsa_key_too_weak_prompted_for_bits = 0;

// smallest size we are allowed to create that satisfies the server. a modulus may come out one
// bit short, so a server asking for exactly N is satisfied by our N; anything between the
// offered sizes rounds up to the next one we can actually generate
function pick_rsa_key_bits_for_minimum(minimum_bits)
{
    for (let i = 0; i < G_ALLOWED_RSA_KEY_BITS.length; i++)
    {
        if (G_ALLOWED_RSA_KEY_BITS[i] >= minimum_bits) { return G_ALLOWED_RSA_KEY_BITS[i]; }
    }
    return 0;
}

// the server rejected our identity key for being too small and told us what it wants. offers to
// switch this device to a big enough key and reconnect. the announcement is optional on the
// server: without it we are simply dropped and this never runs
function handle_rsa_key_too_weak_notice(announced_minimum_bits)
{
    let minimum_bits = parseInt(announced_minimum_bits);

    // the notice arrives before the handshake, so it is unauthenticated: treat the number as
    // untrusted input. nothing outside the range the server itself enforces is believable
    if (isNaN(minimum_bits) || minimum_bits < 2048 || minimum_bits > 8192)
    {
        console.warn("ignoring an implausible key size requirement from the server: " + announced_minimum_bits);
        return;
    }

    // we already generate a key this big, so the rejection was not really about size - say
    // nothing rather than offer a switch that would change nothing
    if (g_rsa_key_bits >= minimum_bits)
    {
        return;
    }

    let target_bits = pick_rsa_key_bits_for_minimum(minimum_bits);

    if (target_bits == 0)
    {
        custom_alert("this server requires an RSA key of " + minimum_bits + " bits, which this client cannot create");
        return;
    }

    // the driver redials on its own and the server rejects every attempt, so without this the
    // dialog would reopen in a loop
    if (g_rsa_key_too_weak_prompted_for_bits == minimum_bits)
    {
        return;
    }
    g_rsa_key_too_weak_prompted_for_bits = minimum_bits;

    document.getElementById("rsa-key-too-weak-text").textContent =
        "This server requires an identity key of at least " + minimum_bits + " bits. Your key is "
        + g_rsa_key_bits + " bits, so the server refused the connection. Switch this device to "
        + target_bits + "-bit keys and reconnect? Creating the key may take a while, and it gives "
        + "you a NEW identity here - the same passphrase produces a different key at a different size.";

    document.getElementById("rsa-key-too-weak-yes-button").setAttribute("data-target-bits", target_bits);
    document.getElementById("rsa-key-too-weak-container").style.display = "block";
    document.getElementById("background-container").style.display = "block";
}

// hides the "stronger key required" dialog
function hide_rsa_key_too_weak_dialog()
{
    document.getElementById("rsa-key-too-weak-container").style.display = "none";
    document.getElementById("background-container").style.display = "none";
}

// shows the a-toast element with the given message
function custom_alert(message = "")
{
    const el = document.querySelector("a-toast")

    el.style.visibility = 'visible';
    el.innerHTML = message
}

// hides the a-toast element and replaces its text
function hide_custom_alert(message = "")
{
    const el = document.querySelector("a-toast")

    el.style.visibility = 'hidden';
    el.innerHTML = message
}

// index of a chat context in g_chat_context_array, -1 when not found
function get_chat_context_index_by_chat_context_id(id)
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

// the STATE half of promoting an offline conversation into the live one, split out of
// UI.promote_offline_chat_context. every decision it makes - is there anything to promote,
// does a live thread already exist, were we reading it - used to be asked of the dom, which
// tied the merge to having been painted. now it is asked of g_chat_context_array.
//
// returns what it did, so the render half can move the matching markup without asking the
// dom the same questions a second time and possibly getting a different answer.
function promote_offline_chat_context_state(client)
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
        chat_message_receiver_id = client.client_id;
        g_offline_chat_recipient_alias = "";
    }

    return promotion;
}

// unread private-message count per client. it used to live ONLY in the badge's innerHTML -
// read out with parseInt, incremented, written back - so with no dom it parsed "" into NaN,
// and nothing outside the member list could ask how many unread messages somebody had.
// that question is exactly what a background service needs in order to raise a notification
// unread message count per CHANNEL, the same idea the member list has per client.
// keyed by channel id; a channel you are looking at is always zero
var g_channel_unread_counts = {};

function get_channel_unread_count(channel_id)
{
    let stored = g_channel_unread_counts[channel_id];
    return (typeof stored === "number") ? stored : 0;
}

function increment_channel_unread_count(channel_id)
{
    // the channel being read right now never accumulates a badge - but "being read"
    // means somebody is LOOKING at it. under node with no ui attached nobody is, so
    // everything counts; otherwise a headless phone would badge nothing for its own
    // channel, which is the one most messages arrive in
    if (is_someone_watching_the_ui() == true
        && g_current_chat_context_id === ("chat-context-channel-" + channel_id))
    {
        return;
    }

    g_channel_unread_counts[channel_id] = get_channel_unread_count(channel_id) + 1;
    render_channel_unread_badge(channel_id);
    update_app_unread_badge();
}

function clear_channel_unread_count(channel_id)
{
    g_channel_unread_counts[channel_id] = 0;
    render_channel_unread_badge(channel_id);
    update_app_unread_badge();
}

function render_channel_unread_badge(channel_id)
{
    let badge = document.querySelector('[data-channel-unread-for="' + channel_id + '"]');

    if (badge == null)
    {
        return;
    }

    let count = get_channel_unread_count(channel_id);
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

// the launcher icon badge: every unread channel plus every unread private conversation.
// android shows it through the notification count; a browser has no icon to badge
function update_app_unread_badge()
{
    let total = 0;

    for (let channel_id in g_channel_unread_counts)
    {
        total += get_channel_unread_count(channel_id);
    }

    for (let i = 0; i < g_client_list.length; i++)
    {
        if (typeof g_client_list[i].unread_count === "number")
        {
            total += g_client_list[i].unread_count;
        }
    }

    // the webview reports directly through the java bridge; node has no Android object,
    // so it hands the number to its host, which sends it over the bridge instead.
    // node is the one that is always alive, so it is what makes the badge work while
    // the app is idle, closed, or was started headless at boot
    if (typeof Android !== "undefined" && typeof Android.JavaExportSetUnreadBadge === "function")
    {
        Android.JavaExportSetUnreadBadge(total);
    }
    else if (g_node_unread_listener != null)
    {
        g_node_unread_listener(total);
    }
}

function get_unread_count(client_id)
{
    let client_object = get_client_by_client_id(client_id);

    if (client_object == null || typeof client_object.unread_count !== "number")
    {
        return 0;
    }

    return client_object.unread_count;
}

function increment_unread_count(client_id)
{
    let client_object = get_client_by_client_id(client_id);

    if (client_object != null)
    {
        client_object.unread_count = get_unread_count(client_id) + 1;
    update_app_unread_badge();
    }
}

function clear_unread_count(client_id)
{
    // opening it is reading it, so anything that arrived while it was closed is
    // receipted now
    send_pending_seen_receipts(client_id);
    render_seen_indicator();

    let client_object = get_client_by_client_id(client_id);

    if (client_object != null)
    {
        client_object.unread_count = 0;
    update_app_unread_badge();
    }
}

// paints the badge from the state above. visibility is a parameter rather than derived from
// the count, because one caller hides the badge WITHOUT clearing it (a message grouped under
// the same sender) - that is odd, but it is the existing behaviour and this is not the place
// to change it
function render_unread_badge(client_id, is_visible)
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
    badge.innerHTML = get_unread_count(client_id);
}

// icon object from g_icons by id, null when not found
function get_icon_by_icon_id(icon_id)
{
    for (var i = 0; i < g_icons.length; i++)
    {
        if (g_icons[i].id == icon_id)
        {
            return g_icons[i];
        }
    }
    return null;
}

// tag object from g_tags by id, null when not found
function get_tag_by_tag_id(tag_id)
{
    for (var i = 0; i < g_tags.length; i++)
    {
        if (g_tags[i].tag_id == tag_id)
        {
            return g_tags[i];
        }
    }
    return null;
}

// direct children of a parent channel; pass g_ROOT_LEVEL_PARENT_SENTINEL to get the
// root channels (those are matched by their is_root_channel flag, not by parent id)
function get_channels_by_channel_parent_id(channels, parent_channel_id)
{
    let result = [];
    for (var i = 0; i < channels.length; i++)
    {
        if (parent_channel_id == g_ROOT_LEVEL_PARENT_SENTINEL)
        {
            // root channels no longer carry a usable parent id (uint64 has no -1),
            // so match them by the explicit is_root_channel flag sent by the server
            if (channels[i].is_root_channel == true)
            {
                result.push(channels[i]);
            }
        }
        else if (channels[i].parent_channel_id == parent_channel_id)
        {
            result.push(channels[i]);
        }
    }
    return result;
}

// channel object from the given channel list by id, null when not found
function get_channel_by_id(channel_list_a, channel_id)
{
    let result = null;

    for (let x = 0; x < channel_list_a.length; x++)
    {
        if (channel_list_a[x].channel_id == channel_id)
        {
            result = channel_list_a[x];
            break;
        }
    }

    return result;
}

// index of a channel in the given channel list by id, -1 when not found
function get_channel_index_in_array_by_channel_id(channel_list_a, channel_id)
{
    let result = -1;

    for (let x = 0; x < channel_list_a.length; x++)
    {
        if (channel_list_a[x].channel_id == channel_id)
        {
            result = x;
            break;
        }
    }

    return result;
}

// AES-CTR encrypts a byte buffer with every key layer in order, returns a Uint8Array
function encrypt_data_with_aes_keys(keys, data)
{
    let encrypted_data_in_current_iteration = new Uint8Array(data);

    for (let i = 0; i < keys.length; i++)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        encrypted_data_in_current_iteration = aesCtrCustom.encrypt(encrypted_data_in_current_iteration);
    }
    return encrypted_data_in_current_iteration;
}

// reverse of encrypt_data_with_aes_keys: applies the key layers in reverse order
function decrypt_data_with_aes_keys(keys, data)
{
    let current_data = new Uint8Array(data);

    for (let i = (keys.length - 1); i >= 0; i--)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        current_data = aesCtrCustom.decrypt(current_data);
    }

    return current_data;
}

// utf8 string -> zero-padded buffer (1024 bytes minimum) -> AES-CTR key layers -> base64
function encrypt_with_aes_keys_and_convert_to_base64(keys, string_to_encrypt)
{
    let byteArrayToSend = new Uint8Array(1024);
    if (string_to_encrypt.length > 1024)
    {
        byteArrayToSend = new Uint8Array(string_to_encrypt.length);
    }

    let textBytes = aesjs.utils.utf8.toBytes(string_to_encrypt);

    for (i = 0; i < textBytes.length; i++)
    {
        byteArrayToSend[i] = textBytes[i];
    }

    let base64EncryptedData = "";
    let encrypted_data_in_current_iteration = byteArrayToSend;

    for (let i = 0; i < keys.length; i++)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        encrypted_data_in_current_iteration = aesCtrCustom.encrypt(encrypted_data_in_current_iteration);

        if (i + 1 == keys.length) // if end has been reached, convert encrypted bytes to base64string
        {
            base64EncryptedData = bytesToBase64String(encrypted_data_in_current_iteration);
        }
    }
    return base64EncryptedData;
}

// base64 -> AES-CTR key layers in reverse -> utf8 string cut at the first null
// terminator (the encrypt side pads short messages up to 1024 bytes)
function decrypt_base64_contents_with_aes_keys(keys, string_to_decrypt)
{
    let encrypted_data_in_current_iteration = base64StringToBytes(string_to_decrypt);

    // decrypt by applying keys in reverse order
    for (let i = (keys.length - 1); i >= 0; i--)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        encrypted_data_in_current_iteration = aesCtrCustom.decrypt(encrypted_data_in_current_iteration);
    }

    decryptedBytes = encrypted_data_in_current_iteration;

    // coerce to Uint8Array: when no cipher layer is applied (zero metadata keys) the value is still the
    // plain array from base64StringToBytes, and TextDecoder.decode rejects a plain array
    let str1 = new TextDecoder().decode(new Uint8Array(decryptedBytes));
    let decrypted_metadata = substringByNullTerminator(str1);
    return decrypted_metadata;
}

// --- HKDF-SHA256 + HMAC-SHA256 for the DH metadata layer. MUST match the server (mbedtls hkdf/md).
//    js-sha256's built-in .hmac is broken in this build, so HMAC is hand-rolled on the plain hash.
function _sha256_bytes(bytes)
{
    let h = _sha256.create();
    h.update(bytes);
    return h.array();
}

// standard HMAC-SHA256 built on the plain hash; accepts plain or typed byte arrays
function _hmac_sha256(key_bytes, msg_bytes)
{
    // normalize to plain arrays: aesjs.utils.utf8.toBytes returns a Uint8Array, which has no .push,
    // and Array.concat(typedArray) appends it as ONE element instead of spreading its bytes.
    let k = Array.from(key_bytes);
    let msg = Array.from(msg_bytes);
    if (k.length > 64) { k = _sha256_bytes(k); }
    while (k.length < 64) { k.push(0); }
    let ipad = k.map(b => b ^ 0x36);
    let opad = k.map(b => b ^ 0x5c);
    let inner = _sha256_bytes(ipad.concat(msg));
    return _sha256_bytes(opad.concat(inner));
}

// HKDF-SHA256 extract + expand, returns `length` derived bytes as a plain array
function _hkdf_sha256(ikm_bytes, salt_bytes, info_bytes, length)
{
    let info = Array.from(info_bytes); // plain array so t.concat(info) spreads the bytes
    let prk = _hmac_sha256(salt_bytes, ikm_bytes);
    let okm = [];
    let t = [];
    let counter = 1;
    while (okm.length < length)
    {
        t = _hmac_sha256(prk, t.concat(info).concat([counter & 0xff]));
        okm = okm.concat(t);
        counter++;
    }
    return okm.slice(0, length);
}

// derive the DH layer's AES enc key (32) + HMAC mac key (32) from the shared-secret decimal string.
// salt/info MUST match base.c _base_internal__derive_dh_keys on the server.
function dh_derive_keys(shared_secret_string)
{
    let ikm = aesjs.utils.utf8.toBytes(shared_secret_string);
    let salt = aesjs.utils.utf8.toBytes("lemonchat-hkdf-salt-v1");
    let info = aesjs.utils.utf8.toBytes("lemonchat-dh-keys-v1");
    let okm = _hkdf_sha256(ikm, salt, info, 64);
    return { enc_key: okm.slice(0, 32), mac_key: okm.slice(32, 64) };
}

// wraps an outgoing message: pads, AES-CTR encrypts with every metadata key under a fresh
// random IV, base64s, and returns the { iv, data [, tag] } JSON envelope string
function encrypt_all_message_data_and_convert_to_base64(string_to_encrypt)
{
    // ONE line that says who encrypted what. a message must be wrapped exactly once:
    // the webview leaves it plain and node wraps it. if a message is ever dropped
    // silently by the server, this says whether it went out double-wrapped (webview
    // wrapped it too) or bare (node did not wrap it)
    if (typeof custom_log === "function" && string_to_encrypt.indexOf("direct_chat_message") !== -1)
    {
        custom_log("[crypto] direct message, ui_only=" + is_ui_only_runtime()
            + " loopback_port=" + g_loopback_port + " -> " + (is_ui_only_runtime() ? "left plain" : "wrapped"));
    }

    // loopback stays plaintext on-device; node re-encrypts toward the real server
    if (is_ui_only_runtime())
    {
        return string_to_encrypt;
    }

    let byteArrayToSend = new Uint8Array(1024);
    if (string_to_encrypt.length > 1024)
    {
        byteArrayToSend = new Uint8Array(string_to_encrypt.length);
    }

    let textBytes = aesjs.utils.utf8.toBytes(string_to_encrypt);

    for (i = 0; i < textBytes.length; i++)
    {
        byteArrayToSend[i] = textBytes[i];
    }

    // fresh random IV per message (CSPRNG). the same IV seeds every metadata layer's AES-CTR counter;
    // it is not secret, so it travels as a separate "iv" field in the JSON envelope next to the ciphertext.
    let message_iv = new Uint8Array(16);
    crypto.getRandomValues(message_iv);

    let encrypted_data_in_current_iteration = byteArrayToSend;

    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        let key_bytes = g_metadata_keys[i].key_bytes;

        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(key_bytes, new aesjs.Counter(Array.from(message_iv)));
        encrypted_data_in_current_iteration = aesCtrCustom.encrypt(encrypted_data_in_current_iteration);
    }

    // base64 the result after all layers. with 0 metadata keys this is just the plaintext bytes,
    // matching the server which base64-decodes and applies no cipher when keys_count is 0
    let base64EncryptedData = bytesToBase64String(encrypted_data_in_current_iteration);

    let iv_base64 = bytesToBase64String(message_iv);
    let envelope = { iv: iv_base64, data: base64EncryptedData };

    // encrypt-then-MAC: if a metadata key carries an HMAC key (the DH layer), authenticate iv || data
    let dh_mac_key = null;
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes) { dh_mac_key = g_metadata_keys[i].mac_bytes; break; }
    }
    if (dh_mac_key)
    {
        let tag_bytes = _hmac_sha256(dh_mac_key, aesjs.utils.utf8.toBytes(iv_base64 + base64EncryptedData));
        envelope.tag = bytesToBase64String(tag_bytes);
    }

    return JSON.stringify(envelope);
}

// reverse of encrypt_all_message_data_and_convert_to_base64: verifies the HMAC tag when
// the DH layer is active, then peels the metadata key layers; returns "" on any failure
function decrypt_message_metadata(envelope_json)
{
    // the wire payload is a JSON envelope { "iv": <base64>, "data": <base64 ciphertext> }.
    // the per-message IV is public and seeds every metadata layer's AES-CTR counter.
    let envelope = null;
    try
    {
        envelope = JSON.parse(envelope_json);
    }
    catch (exception)
    {
        return "";
    }

    if (envelope == null || typeof envelope.iv != "string" || typeof envelope.data != "string")
    {
        return "";
    }

    // encrypt-then-MAC: if a metadata key carries an HMAC key (the DH layer), the message MUST carry a
    // valid tag. verify it over iv || data BEFORE decrypting; reject (return "") on a missing/bad tag.
    let dh_mac_key = null;
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes) { dh_mac_key = g_metadata_keys[i].mac_bytes; break; }
    }
    if (dh_mac_key)
    {
        if (typeof envelope.tag != "string") { return ""; }
        let expected_tag = bytesToBase64String(_hmac_sha256(dh_mac_key, aesjs.utils.utf8.toBytes(envelope.iv + envelope.data)));
        if (expected_tag !== envelope.tag) { return ""; }
    }

    let message_iv = base64StringToBytes(envelope.iv);
    let encrypted_data_in_current_iteration = base64StringToBytes(envelope.data);

    for (let i = (g_metadata_keys.length - 1); i >= 0; i--)
    {
        let key_bytes = g_metadata_keys[i].key_bytes;

        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(key_bytes, new aesjs.Counter(Array.from(message_iv)));
        encrypted_data_in_current_iteration = aesCtrCustom.decrypt(encrypted_data_in_current_iteration);
    }

    decryptedBytes = encrypted_data_in_current_iteration;

    // coerce to Uint8Array: when no cipher layer is applied (zero metadata keys) the value is still the
    // plain array from base64StringToBytes, and TextDecoder.decode rejects a plain array
    let str1 = new TextDecoder().decode(new Uint8Array(decryptedBytes));
    let decrypted_metadata = substringByNullTerminator(str1);
    return decrypted_metadata;
}

// HTML-entity-escape for the two contexts this app inserts untrusted strings into: element text
// ("<p>NAME</p>") and quoted attribute values (class='...', style='...', value="..."). escaping both
// quote characters plus < > & covers those. it is NOT sufficient for unquoted attributes, javascript:
// URLs, inline event handlers, or <script>/<style> bodies - do not use it there. it is not idempotent,
// so escape a value exactly once on the way into the DOM. for an <img> src use sanitize_image_data_url.
function sanitize_string(str)
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

// validate a decrypted picture before it is used as an <img> src. only a base64 image data URL is
// allowed; anything else (an external http(s) URL that would beacon the viewer's ip/online status on
// load, a javascript:/data:text-html scheme, or malformed data) is replaced with a blank placeholder.
// assigning to .src does not decode HTML entities, so sanitize_string is the wrong tool for this sink;
// this scheme+charset allowlist is.
function sanitize_image_data_url(str)
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

// strips the "data:...;base64," prefix; returns the input unchanged when there is no comma
function remove_data_url_prefix_from_base64_string(base64string)
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

// splits a string into up to required_parts_count roughly equal slices
function split_string_into_smaller_parts(string_to_split, required_parts_count)
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

// ships the local webrtc sdp answer to the server (also logs the message object)
function send_sdp_answer_to_server(value)
{
    let message_object = {
        message: {
            type: "sdp_answer",
            value: value
        }
    };

    console.log(message_object);

    send_message_object(message_object);
}

// large files such as .mp3 files for music bots or direct and channel chat pictures, had to be split into smaller parts
// sending them at once lags the application
// clears the upload lock and the overlay. the parts loop must not do this itself: it only queues,
// so releasing there let a second upload start and wipe the server buffer of the first
function release_file_upload_lock()
{
    if (g_upload_ack_timer != null)
    {
        clearTimeout(g_upload_ack_timer);
        g_upload_ack_timer = null;
    }

    document.getElementById("upload-progress-overlay").style.display = "none";
    document.getElementById("upload-progress-bar").style.width = '0%';
    document.getElementById("upload-progress-text").innerHTML = '0%';
    g_is_file_being_uploaded = false;
}

// the upload ack arrived but the relay to the receivers is still running; say so instead of
// leaving the grayed thumbnail unexplained. image_sent_status ends it, the timer is a fallback
function show_picture_delivery_status()
{
    g_picture_delivery_pending = true;

    document.getElementById("upload-progress-overlay").style.display = "block";
    document.getElementById("upload-progress-bar").style.width = "100%";
    document.getElementById("upload-progress-text").innerHTML = "image received by server, being received by users ...";

    if (g_picture_delivery_hide_timer != null)
    {
        clearTimeout(g_picture_delivery_hide_timer);
    }

    g_picture_delivery_hide_timer = setTimeout(hide_picture_delivery_status, 120000);
}

function hide_picture_delivery_status()
{
    if (g_picture_delivery_hide_timer != null)
    {
        clearTimeout(g_picture_delivery_hide_timer);
        g_picture_delivery_hide_timer = null;
    }

    if (g_picture_delivery_pending == false)
    {
        return;
    }

    g_picture_delivery_pending = false;

    // a new upload may already own the overlay; leave it to that one then
    if (g_is_file_being_uploaded == false)
    {
        document.getElementById("upload-progress-overlay").style.display = "none";
        document.getElementById("upload-progress-bar").style.width = "0%";
        document.getElementById("upload-progress-text").innerHTML = "0%";
    }
}

// the intent globals are written before send_file_by_parts runs its guard, so a refused upload
// relabelled the one already in flight. call sites ask here first and touch nothing on refusal
function can_start_file_upload()
{
    if (g_is_file_being_uploaded == true)
    {
        custom_alert("cant upload more than 1 file at a time");
        return false;
    }

    return true;
}

function send_file_by_parts(parts, total_bytes_length, delay_ms) {

    if (g_is_file_being_uploaded == true)
    {
        custom_alert("cant upload more than 1 file at a time");
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
            g_upload_ack_timer = setTimeout(release_file_upload_lock, G_UPLOAD_ACK_TIMEOUT_MS);
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

// replaces g_peer_connection_with_server with a fresh RTCPeerConnection wired to the
// webrtc handlers; makes sure g_iceconfig carries the TURN server exactly once
function create_new_peer_connection_object_for_use(is_this_reconnect = false)
{
    // close the replaced pc: an abandoned-but-open pc kept its ice agent and its
    // 'datachannel' listener alive, hijacking state with zombie events and slowly
    // eating into the browser's peer connection cap
    if (g_peer_connection_with_server != null)
    {
        try
        {
            g_peer_connection_with_server.removeEventListener('datachannel', webrtc.peerconnection_on_datachannel_receive);
            g_peer_connection_with_server.close();
        }
        catch (peer_connection_close_error)
        {
            console.log("closing replaced peer connection failed: " + peer_connection_close_error.message);
        }
    }

    g_datachannel = null;
    g_peer_connection_with_server = null;

    // added when missing rather than keyed on is_this_reconnect: the audio_enabled handler
    // rebuilds g_iceconfig without it, and retries used to push a duplicate every 10s
    let is_turn_server_present = false;

    for (let i = 0; i < g_iceconfig.iceServers.length; i++)
    {
        let ice_server_urls = g_iceconfig.iceServers[i].urls;

        if ((typeof ice_server_urls === "string" && ice_server_urls.indexOf("turn:") == 0)
            || (Array.isArray(ice_server_urls) && ice_server_urls.length > 0 && String(ice_server_urls[0]).indexOf("turn:") == 0))
        {
            is_turn_server_present = true;
            break;
        }
    }

    if (is_turn_server_present == false)
    {
        let turn_server = {
            urls: [
                "turn:"+host+":3478?transport=udp",
                "turn:"+host+":3478?transport=tcp"
            ],
            username: "usweger123",
            credential: "pw1wegweg23Q"
        };

        g_iceconfig.iceServers.push(turn_server);
    }

    g_peer_connection_with_server = new RTCPeerConnection(g_iceconfig);
    g_peer_connection_with_server.onicecandidate = webrtc.peerconnection_handle_ice_candidate_event;
    g_peer_connection_with_server.onsignalingstatechange = webrtc.peerconnection_handle_signaling_state_change_event;
    g_peer_connection_with_server.onicegatheringstatechange = webrtc.peerconnection_handle_ice_gathering_state_change_event;
    g_peer_connection_with_server.oniceconnectionstatechange = webrtc.peerconnection_handle_ice_connection_state_change_event;
    g_peer_connection_with_server.onnegotiationneeded = webrtc.peerconnection_handle_negotiation_needed_event;
    g_peer_connection_with_server.onconnectionstatechange = webrtc.peer_connection_handle_onconnection_state_change_event;
    g_peer_connection_with_server.addEventListener('datachannel', webrtc.peerconnection_on_datachannel_receive);
}

// depth of a channel in the tree (0 for a root), walked via parent ids; bails out
// with an error when the parent chain is broken or cyclic
function get_indentation_level(channel_id, channels)
{
    let result = 0;
    let is_root_channel_found = false;
    let channel_id_currently_looking_for = channel_id;

    let safety_counter = 0;

    while (is_root_channel_found == false)
    {
        // a valid parent chain reaches a root in at most channels.length steps. if it does not,
        // the data is malformed (missing root or a cycle) - bail out instead of looping forever
        if (safety_counter > channels.length)
        {
            console.error("get_indentation_level: could not reach a root channel for channel_id " + channel_id + " - channel data looks invalid");
            return result;
        }
        safety_counter++;

        let was_channel_found = false;
        for (let i = 0; i < channels.length; i++)
        {
            if (channels[i].channel_id == channel_id_currently_looking_for)
            {
                was_channel_found = true;
                if (channels[i].is_root_channel == true)
                {
                    is_root_channel_found = true;
                }
                else
                {
                    result++;
                    channel_id_currently_looking_for = channels[i].parent_channel_id;
                }
                break;
            }
        }

        // the channel we are looking for is not in the list - the chain is broken, stop
        if (was_channel_found == false)
        {
            console.error("get_indentation_level: channel_id " + channel_id_currently_looking_for + " not found while walking parents - channel data looks invalid");
            return result;
        }
    }
    return result;
}

// tells whoever is on the other side of the OPEN conversation that we are writing. called on
// every keystroke but only actually sends every few seconds, so a long message is a handful
// of tiny messages, not one per character. carries no text - just where it belongs
function send_typing_indicator()
{
    if (g_is_typing_indicator_allowed == false || g_is_authenticated == false)
    {
        return;
    }

    // an offline conversation has nobody to tell
    if (g_offline_chat_recipient_alias != null && g_offline_chat_recipient_alias.length > 0)
    {
        return;
    }

    let now = new Date().valueOf();
    if (now - g_typing_last_sent_at < G_TYPING_SEND_INTERVAL_MS)
    {
        return;
    }
    g_typing_last_sent_at = now;

    let receiver_id = (g_chat_message_receiver_type == "channel") ? current_channel_id : parseInt(chat_message_receiver_id);

    if (isNaN(receiver_id))
    {
        return;
    }

    send_message_object({
        message: {
            type: "typing_indicator_request",
            receiver_type: g_chat_message_receiver_type,
            receiver_id: receiver_id
        }
    });
}

// chat context id a typing notice belongs to, matching the ids the chat contexts are built
// with, so the notice can be shown only while that conversation is the one on screen
function get_chat_context_id_for_typing(receiver_type, receiver_id, sender_client_id)
{
    if (receiver_type == "channel")
    {
        return "chat-context-channel-" + receiver_id;
    }

    // he is writing to US, so the conversation is the one with HIM
    return "chat-context-pm-" + sender_client_id;
}

// remembers that somebody is typing in a conversation and (re)starts the ticker that
// repaints and expires the notice
function note_typing_from_client(sender_client_id, receiver_type, receiver_id)
{
    let context_id = get_chat_context_id_for_typing(receiver_type, receiver_id, sender_client_id);

    if (g_typing_state[context_id] == null)
    {
        g_typing_state[context_id] = {};
    }

    g_typing_state[context_id][sender_client_id] = new Date().valueOf() + G_TYPING_EXPIRY_MS;

    render_typing_indicator();

    if (g_typing_render_timer == null)
    {
        g_typing_render_timer = window.setInterval(render_typing_indicator, 1000);
    }
}

// somebody's message arrived (or he left): whatever he was typing is over
function clear_typing_from_client(sender_client_id)
{
    for (let context_id in g_typing_state)
    {
        if (g_typing_state[context_id] != null)
        {
            delete g_typing_state[context_id][sender_client_id];
        }
    }

    render_typing_indicator();
}

// one typing line: the text, then three dots that animate on their own. the dots are
// separate spans so css can fade them in one after another - the alternative (rewriting
// the text on a timer) would repaint the whole line several times a second
function build_typing_line(text_without_dots)
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

// paints the "x is typing ..." line for the conversation currently on screen, drops entries
// that were not refreshed, and stops its own ticker once nothing is left to show
function render_typing_indicator()
{
    let element = document.getElementById("typing-indicator-container");
    if (element == null)
    {
        return;
    }

    let now = new Date().valueOf();
    let names = [];
    let anything_left = false;

    for (let context_id in g_typing_state)
    {
        let typers = g_typing_state[context_id];
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
                names.push(get_display_name_by_client_id(parseInt(client_id), ""));
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
            element.appendChild(build_typing_line(names[i] + " is typing"));
        }

        if (names.length > lines_to_show)
        {
            element.appendChild(build_typing_line("and " + (names.length - lines_to_show) + " others are typing"));
        }

        // "block", never "": clearing the inline style falls back to the stylesheet,
        // where this element is display:none - so it would never actually appear
        element.style.display = "block";
    }

    if (anything_left == false && g_typing_render_timer != null)
    {
        window.clearInterval(g_typing_render_timer);
        g_typing_render_timer = null;
    }
}

// the attached file goes out as a message of its own; typed text stays in the box for the next
// send. every way this can fail tells the user and keeps the file attached. true = handed off
function send_pending_chat_file(receiver_type)
{
    if (g_pending_chat_file == null)
    {
        return false;
    }

    if (g_is_file_being_uploaded == true)
    {
        custom_alert("cant upload more than 1 file at a time");
        return false;
    }

    let chat_context_index = get_chat_context_index_by_chat_context_id(g_current_chat_context_id);

    if (chat_context_index == -1)
    {
        custom_log("[send-file] STOP: no chat context for " + g_current_chat_context_id);
        return false;
    }

    let receiver_public_key = "";

    if (receiver_type == "user")
    {
        let receiver = get_client_by_client_id(chat_message_receiver_id);

        if (receiver == null)
        {
            custom_alert("files can only be sent to people who are online");
            return false;
        }

        if (receiver.is_idle == true)
        {
            custom_alert(sanitize_string(get_display_name_by_client_id(chat_message_receiver_id, receiver.username)) + " is idle, files can not be delivered to idle clients");
            return false;
        }

        receiver_public_key = get_public_key_by_client_id(chat_message_receiver_id);

        if (receiver_public_key == "" || receiver_public_key == null)
        {
            custom_alert("no public key for this user yet, try again in a moment");
            return false;
        }
    }
    else if (current_channel_keys == null)
    {
        custom_alert("no channel keys from channel maintainer, cant send the file");
        return false;
    }

    let file = g_pending_chat_file;
    let key = "local-" + local_message_id;

    clear_pending_chat_file();

    let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer').length;

    for (let i = 0; i < element_count; i++)
    {
        document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer')[0].remove();
    }

    let html_to_append = "<div class=\"single-chat-message\">\n\
                            <div class=\"single-message-content\">\n\
                                <div class=\"single-chat-message-sender-local-username-container\">\n\
                                    " + generate_message_sender_html(local_client_id, g_local_username) + "\n\
                                </div>\n\
                                <div class=\"single-chat-message-sender-time\">\n\
                                    <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                </div>\n\
                                <div class=\"single-chat-message-content\">\n\
                                    " + generate_chat_file_card_html({ key: key, name: file.name, size: file.size, is_local: true, local_message_id: local_message_id }) + "\n\
                                </div>\n\
                            </div>\n\
                        </div>";

    document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
    document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
    document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;

    g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;

    // our own copy is downloadable right away
    g_chat_files_by_message_id[key] = file;

    let card = get_chat_file_card_by_key(key);

    if (card != null)
    {
        wire_chat_file_download_button(card, key);
    }

    // lock now, not when the worker answers: encrypting 20 MB takes a moment and a second
    // send in that window must be refused instead of relabelling this one
    g_is_file_being_uploaded = true;
    document.getElementById("upload-progress-overlay").style.display = "block";
    document.getElementById("upload-progress-text").innerHTML = "encrypting ...";

    if (receiver_type == "user")
    {
        clear_seen_indicator_for_client(chat_message_receiver_id);

        g_data_processing_worker.postMessage({
            type: "mainthread__create_direct_chat_file",
            file: file,
            receiver_public_key: receiver_public_key,
            chat_message_receiver_id: chat_message_receiver_id,
            local_message_id: local_message_id
        });
    }
    else
    {
        g_data_processing_worker.postMessage({
            type: "mainthread__create_channel_chat_file",
            file: file,
            current_channel_keys: current_channel_keys,
            current_channel_id: current_channel_id,
            local_message_id: local_message_id
        });
    }

    local_message_id++;
    return true;
}

// sends the chat input (or the pending picture) to the current channel: renders the local
// echo into the chat DOM, hands encryption to the worker, and bumps local_message_id
function send_channel_chat_message()
{
    // a pending file goes first, on its own
    if (g_pending_chat_file != null)
    {
        send_pending_chat_file("channel");
        return;
    }

    let chat_message_to_send_value = document.getElementById('chat-input-container-text-input').value;
    document.getElementById('chat-input-container-text-input').value = "";

    if (base64_picture_string_to_send.length == 0 && chat_message_to_send_value.trim().length == 0)
    {
        return;
    }

    let chat_context_index = get_chat_context_index_by_chat_context_id(g_current_chat_context_id);
    if (chat_context_index == -1)
    {
        custom_log('could not find ' + g_current_chat_context_id.toString());
        return;
    }

    if (base64_picture_string_to_send.length > 0)
    {
        if (can_start_file_upload() == false)
        {
            return;
        }

        let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer').length;

        for (let i = 0; i < element_count; i++)
        {
            document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer')[0].remove();
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                <div class=\"single-message-content\">\n\
                                    <div class=\"single-chat-message-sender-local-username-container\">\n\
                                        " + generate_message_sender_html(local_client_id, g_local_username) + "\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <img class='imgnotsentyet local-client-chat-picture-img chat-picture-img' data-single-chat-message-local-message-id='"+ local_message_id + "' data-single-chat-message-server-message-id='unspecified' src=" + base64_picture_string_to_send + "></img>\n\
                                    </div>\n\
                                </div>\n\
                            </div>";

        document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", '<div class=\"chat-spacer\"></div>');
        document.getElementById('chat-context-container').scrollTop = document.getElementById('chat-context-container').scrollHeight;

        document.getElementById("image-upload-preview").style.backgroundImage = "";
        document.getElementById("image-upload-preview").style.display = "none";
        g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;

        // lock now, like the file path: encrypting a big picture takes a while and a second
        // send in that window must be refused, not relabel this one
        g_is_file_being_uploaded = true;
        document.getElementById("upload-progress-overlay").style.display = "block";
        document.getElementById("upload-progress-text").innerHTML = "processing image for send ...";

        g_data_processing_worker.postMessage({
            type: "mainthread__create_channel_chat_picture",
            base64_picture_string_to_send: base64_picture_string_to_send,
            current_channel_keys: current_channel_keys,
            current_channel_id: current_channel_id,
            local_message_id: local_message_id
        });

        local_message_id++;

        base64_picture_string_to_send = "";
    }
    else if (chat_message_to_send_value.length > 0)
    {
        actual_data_to_send = {
            font : g_selected_font,
            font_size: g_selected_font_size,
            font_color: g_selected_font_color,
            value : chat_message_to_send_value,
        };

        let actual_chat_message_to_send_value = JSON.stringify(actual_data_to_send);

        if (g_chat_context_array[chat_context_index].last_known_message_sender_username == g_local_username)
        {
            let chat_message = "<p class='single-chat-message-content-p local-single-chat-message-content-p "+g_selected_font+"' data-single-chat-message-local-message-id='" + local_message_id + "' data-single-chat-message-server-message-id='unspecified' style='color: "+g_selected_font_color+"; font-size: "+g_selected_font_size+"px;'>" + sanitize_string(chat_message_to_send_value) + "</p>";
            let last_child_index = document.getElementById(g_current_chat_context_id).getElementsByClassName("single-chat-message").length - 1;
            document.getElementById(g_current_chat_context_id).getElementsByClassName("single-chat-message")[last_child_index].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", chat_message);
        }
        else
        {
            let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer').length;

            for (let i = 0; i < element_count; i++)
            {
                document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer')[0].remove();
            }

            let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-local-username-container\">\n\
                                            " + generate_message_sender_html(local_client_id, g_local_username) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\">\n\
                                            <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            <p class='single-chat-message-content-p local-single-chat-message-content-p "+g_selected_font+"' data-single-chat-message-local-message-id='"+ local_message_id + "' data-single-chat-message-server-message-id='unspecified' style='color: "+g_selected_font_color+"; font-size: "+g_selected_font_size+"px;'>" + sanitize_string(chat_message_to_send_value) + "</p>\n\
                                        </div>\n\
                                    </div>\n\
                                </div>";

            document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
            document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", '<div class=\"chat-spacer\"></div>');
        }

        document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;

        g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;

        let object_to_send = {
            type: "mainthread__create_channel_chat_message",
            chat_message_to_send_value: actual_chat_message_to_send_value,
            current_channel_keys: current_channel_keys,
            current_channel_id: current_channel_id,
            local_message_id: local_message_id
        }

        g_data_processing_worker.postMessage(object_to_send);

        local_message_id++;
    }
}

// which offline person the chat input is currently addressing (set when their circle is
// tapped). empty whenever the open conversation is a channel or a connected client.
var g_offline_chat_recipient_alias = "";

// a CONNECTED client carrying this alias, or null. aliases are unique per identity, so
// this is how an offline conversation finds its owner once they come back
function get_client_by_alias(alias)
{
    if (typeof alias !== "string" || alias.length == 0) { return null; }

    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null && typeof g_client_list[i].alias === "string" && g_client_list[i].alias.toLowerCase() == alias.toLowerCase())
        {
            return g_client_list[i];
        }
    }

    return null;
}

// offline contact from g_offline_client_list by alias (case-insensitive), or null
function get_stored_client_by_alias(alias)
{
    if (typeof alias !== "string" || alias.length == 0) { return null; }

    for (let i = 0; i < g_offline_client_list.length; i++)
    {
        if (g_offline_client_list[i].alias.toLowerCase() == alias.toLowerCase())
        {
            return g_offline_client_list[i];
        }
    }

    return null;
}

// text to somebody who is not connected: same encryption as a direct message (their
// public key came with the offline roster), addressed by alias. the server parks it and
// hands it over when they return.
function send_offline_chat_message(offline_contact)
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
                                        " + generate_message_sender_html(local_client_id, g_local_username) + "\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <p class='single-chat-message-content-p " + g_selected_font + "' style='color: " + g_selected_font_color + "; font-size: " + g_selected_font_size + "px;'>" + sanitize_string(chat_message_to_send_value) + "</p>\n\
                                    </div>\n\
                                </div>\n\
                            </div>";

        document.getElementById(context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__create_offline_chat_message",
        chat_message_value: JSON.stringify(actual_data_to_send),
        receiver_public_key: offline_contact.public_key,
        recipient_alias: offline_contact.alias
    });
}

// sends the chat input (or the pending picture) to the selected client: renders the local
// echo, then lets the worker RSA+AES encrypt it for the receiver's public key
function send_direct_chat_message()
{
    // a pending file goes first, on its own
    if (g_pending_chat_file != null)
    {
        send_pending_chat_file("user");
        return;
    }

    let chat_message_to_send_value = document.getElementById('chat-input-container-text-input').value;
    document.getElementById('chat-input-container-text-input').value = "";

    custom_log("[send-direct] entered, text=" + chat_message_to_send_value.trim().length
        + " chars, picture=" + base64_picture_string_to_send.length + " chars");

    if (base64_picture_string_to_send.length == 0 && chat_message_to_send_value.trim().length == 0)
    {
        custom_log("[send-direct] STOP: nothing to send (input was empty)");
        return;
    }

    let chat_context_index = get_chat_context_index_by_chat_context_id(g_current_chat_context_id);

    if (chat_context_index == -1)
    {
        custom_log("[send-direct] STOP: no chat context for " + g_current_chat_context_id);
        return;
    }

    let receiver_public_key = get_public_key_by_client_id(chat_message_receiver_id);

    if (receiver_public_key == "" || receiver_public_key == null)
    {
        custom_log("[send-direct] STOP: no public key for receiver " + chat_message_receiver_id);
        return;
    }

    custom_log("[send-direct] key ok, handing to the worker for receiver " + chat_message_receiver_id);

    // a new message of ours is unread by definition, so the eye goes until they read it
    clear_seen_indicator_for_client(chat_message_receiver_id);

    if (base64_picture_string_to_send.length > 0)
    {
        if (can_start_file_upload() == false)
        {
            return;
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                <div class=\"single-message-content\">\n\
                                    <div class=\"single-chat-message-sender-local-username-container\">\n\
                                        " + generate_message_sender_html(local_client_id, g_local_username) + "\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <img class='imgnotsentyet local-client-chat-picture-img chat-picture-img' data-single-chat-message-local-message-id='"+ local_message_id + "' data-single-chat-message-server-message-id='unspecified' src=" + base64_picture_string_to_send + "></img>\n\
                                    </div>\n\
                                </div>\n\
                            </div>";

        document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
        document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;

        let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer').length;

        for (let i = 0; i < element_count; i++)
        {
            let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer')[0].remove();
        }

        document.getElementById('image-upload-preview').style.backgroundImage = "";
        document.getElementById('image-upload-preview').style.display = "none";
        g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;

        g_is_file_being_uploaded = true;
        document.getElementById("upload-progress-overlay").style.display = "block";
        document.getElementById("upload-progress-text").innerHTML = "processing image for send ...";

        g_data_processing_worker.postMessage({
            type: "mainthread__create_direct_chat_picture",
            base64_picture_string_to_send: base64_picture_string_to_send,
            receiver_public_key: receiver_public_key,
            chat_message_receiver_id: chat_message_receiver_id,
            local_message_id: local_message_id
        });

        base64_picture_string_to_send = "";
        local_message_id++;
    }
    else if (chat_message_to_send_value.length > 0)
    {
        // server does not have access to color used or fontsize, this is all part of encrypted data of the message

        actual_data_to_send = {
            font : g_selected_font,
            font_size: g_selected_font_size, // not used right now but could be
            font_color: g_selected_font_color, // not used right now but oculd be
            value : chat_message_to_send_value,
        };

        let actual_chat_message_to_send_value = JSON.stringify(actual_data_to_send);

        // process html here, message content will be processed in g_data_processing_worker

        if (g_chat_context_array[chat_context_index].last_known_message_sender_username == g_local_username)
        {
            let chat_message = "<p class='single-chat-message-content-p local-single-chat-message-content-p "+g_selected_font+"' data-single-chat-message-local-message-id='" + local_message_id + "' data-single-chat-message-server-message-id='unspecified' style='color: "+g_selected_font_color+"; font-size: "+g_selected_font_size+"px;'>" + sanitize_string(chat_message_to_send_value) + "</p>";
            let last_child_index = document.getElementById(g_current_chat_context_id).getElementsByClassName("single-chat-message").length - 1;
            document.getElementById(g_current_chat_context_id).getElementsByClassName("single-chat-message")[last_child_index].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", chat_message);
        }
        else
        {
            let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-local-username-container\">\n\
                                            " + generate_message_sender_html(local_client_id, g_local_username) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\">\n\
                                            <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            <p class='single-chat-message-content-p local-single-chat-message-content-p "+g_selected_font+"' data-single-chat-message-local-message-id='"+ local_message_id + "' data-single-chat-message-server-message-id='unspecified' style='color: "+g_selected_font_color+"; font-size: "+g_selected_font_size+"px;'>" + sanitize_string(chat_message_to_send_value) + "</p>\n\
                                        </div>\n\
                                    </div>\n\
                                </div>";

            document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
            let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer').length;

            for (let i = 0; i < element_count; i++)
            {
                document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer')[0].remove();
            }

            html_to_append = "<div class=\"chat-spacer\"></div>";
            document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        }

        document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;

        g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;

        g_data_processing_worker.postMessage({
            type: "mainthread__create_direct_chat_message",
            chat_message_value: actual_chat_message_to_send_value,
            receiver_public_key: receiver_public_key,
            chat_message_receiver_id: chat_message_receiver_id,
            local_message_id: local_message_id
        });

        local_message_id++;
    }
}

// send-button entry point: routes to channel/direct/offline sending based on the current
// receiver type, with fallbacks when the direct receiver went offline or came back
function send_chat_message()
{
    // EVERY exit from this function is logged. a message that quietly goes nowhere is
    // the hardest kind of bug to chase, and this path has several silent returns
    custom_log("[send] type=" + g_chat_message_receiver_type
        + " receiver_id=" + chat_message_receiver_id + " (" + (typeof chat_message_receiver_id) + ")"
        + " context=" + g_current_chat_context_id);

    if (g_chat_message_receiver_type == "channel")
    {
        if (current_channel_keys == null)
        {
            custom_log("[send] STOP: no channel keys");
            custom_alert("no channel keys from channel maintainer, cant send the message");
            return;
        }
        send_channel_chat_message();
    }
    else if (g_chat_message_receiver_type == "user")
    {
        if (get_client_by_client_id(chat_message_receiver_id) == null)
        {
            // nobody is there to decrypt a file, and the server keeps nothing this big
            if (g_pending_chat_file != null)
            {
                custom_log("[send] STOP: file attached but the receiver is offline");
                custom_alert("files can only be sent to people who are online");
                return;
            }

            custom_log("[send] receiver not in client list, trying the offline paths"
                + " (alias='" + g_offline_chat_recipient_alias + "')");
            // they left (or were never here this session). if the server keeps messages
            // for registered people we can still write - otherwise say so plainly
            // they may have come online since this context was opened: send it live
            // instead of queueing (the server refuses to queue for a connected client)
            let live_client = get_client_by_alias(g_offline_chat_recipient_alias);

            if (live_client != null)
            {
                // state first, markup second. routing this through UI.* alone would lose the
                // whole promotion in a runtime with no ui to paint
                let promotion = promote_offline_chat_context_state(live_client);
                UI.promote_offline_chat_context_render(promotion, live_client);

                custom_log("[send] promoted offline context to live, sending direct");
                send_direct_chat_message();
                return;
            }

            let offline_contact = get_stored_client_by_alias(g_offline_chat_recipient_alias);

            if (offline_contact != null && typeof offline_contact.public_key === "string" && offline_contact.public_key.length > 0)
            {
                custom_log("[send] sending as OFFLINE message");
                send_offline_chat_message(offline_contact);
                return;
            }

            if (offline_contact != null)
            {
                // we know who they are but were never given their public key, so nothing
                // can be encrypted for them - the server does not hold messages here
                custom_log("[send] STOP: known contact but no public key");
                custom_alert("this server does not keep messages for people who are offline");
                return;
            }

            custom_log("[send] STOP: receiver unknown and no offline contact");
            custom_alert("this user is offline, the message can not be delivered");
            return;
        }

        custom_log("[send] receiver found, going direct");
        send_direct_chat_message();
    }
}

// true when the local client carries the admin tag (tag id 0)
function is_local_client_admin()
{
    let result = false;
    let client = get_client_by_client_id(local_client_id);
    if (client.tag_ids.includes(0))
    {
        result = true;
    }
    return result;
}

// client object from g_client_list via the id->index map; accepts string or int id
function get_client_by_client_id(client_id)
{
    let actual_client_id = null;
    if (typeof client_id === "string")
    {
        actual_client_id = parseInt(client_id);
    }
    else
    {
        actual_client_id = client_id;
    }

    let result = null;

    let client_index = g_map_client_id_to_array_index.get(actual_client_id);
    result = g_client_list[client_index];

    return result;
}

// client_id must be int
function get_client_index_in_array_by_client_id(client_id)
{
    let index = g_map_client_id_to_array_index.get(client_id);

    // callers all check for -1, and a map hands back undefined instead. undefined == -1
    // is false, so the check passed and the array lookup threw one line later
    if (index === undefined)
    {
        // the map and the client list disagree. the list length says which way:
        // a non-empty list here means the two were cleared at different moments
        console.warn("client lookup miss: no entry for client_id " + client_id
            + ", client list holds " + g_client_list.length);

        return -1;
    }

    return index;
}

// username for a client id; undefined when the client is not in g_client_list
function get_username_by_client_id(client_id)
{
    let result = "";
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i].client_id == client_id)
        {
            result = g_client_list[i].username;
            return result;
        }
    }
}

// the name a person should be LABELLED with: their alias when they have one, their
// username otherwise. an alias is admin-granted and stable, a username is whatever they
// typed this session, so showing the username next to someone known as "fred" only
// confuses. use this for anything the user reads; use get_username_by_client_id when
// the actual username is what matters (protocol, rename bookkeeping)
function get_display_name_by_client_id(client_id, fallback_username)
{
    let client_object = get_client_by_client_id(client_id);

    if (client_object != null && typeof client_object.alias === "string" && client_object.alias.length > 0)
    {
        return client_object.alias;
    }

    if (client_object != null && typeof client_object.username === "string" && client_object.username.length > 0)
    {
        return client_object.username;
    }

    return (typeof fallback_username === "string") ? fallback_username : "";
}

// public key string for a client id; undefined when the client is not in g_client_list
function get_public_key_by_client_id(client_id)
{
    let result = "";
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i].client_id == client_id)
        {
            result = g_client_list[i].public_key;
            return result;
        }
    }
}

// --- channel/client tree DOM ordering helpers ---
// the channel-list container is a flat pre-order (DFS) list: each .single-channel header is followed by
// that channel's own .connected-client rows and then by its subchannels' subtrees. new items are inserted
// relative to these anchors so they land at the correct (bottom) position instead of being prepended
// right after a header (which reversed sibling order until a full rebuild on rejoin).

// true if ancestor_channel_id is child_channel_id itself or any of its ancestors
function is_channel_in_subtree_of(child_channel_id, ancestor_channel_id)
{
    let current = get_channel_by_id(g_channel_list, child_channel_id);
    let guard = 0;

    while (current != null && guard < 10000)
    {
        if (current.channel_id == ancestor_channel_id)
        {
            return true;
        }
        current = get_channel_by_id(g_channel_list, current.parent_channel_id);
        guard++;
    }

    return false;
}

// last DOM element belonging to a channel's whole subtree (its clients, its subchannels and their
// content), or the channel header itself when the subtree is empty; insert a new subchannel after this
function get_channel_subtree_last_element(channel_id)
{
    let header = document.querySelector('[data-channel-id="' + channel_id + '"]');
    let anchor = null;
    let node = null;

    if (header == null)
    {
        return null;
    }

    anchor = header;
    node = header.nextElementSibling;

    while (node != null)
    {
        let belongs = false;

        if (node.classList.contains("connected-client"))
        {
            let client = get_client_by_client_id(parseInt(node.getAttribute("data-connected-client-id")));
            if (client != null && is_channel_in_subtree_of(client.channel_id, channel_id))
            {
                belongs = true;
            }
        }
        else if (node.classList.contains("single-channel"))
        {
            if (is_channel_in_subtree_of(parseInt(node.getAttribute("data-channel-id")), channel_id))
            {
                belongs = true;
            }
        }

        if (belongs == false)
        {
            break;
        }

        anchor = node;
        node = node.nextElementSibling;
    }

    return anchor;
}

// last of a channel's own .connected-client rows (the contiguous run right after its header, before any
// subchannel), or the header itself when the channel has no clients; insert a joining client after this
function get_channel_own_clients_last_element(channel_id)
{
    let header = document.querySelector('[data-channel-id="' + channel_id + '"]');
    let anchor = null;
    let node = null;

    if (header == null)
    {
        return null;
    }

    anchor = header;
    node = header.nextElementSibling;

    while (node != null && node.classList.contains("connected-client"))
    {
        anchor = node;
        node = node.nextElementSibling;
    }

    return anchor;
}

// builds the channel-tree row HTML for one client (mic-state circle, name - an editable
// input for the local client - alias, flag, song marquee, tags), indented by channel depth
function generate_html_for_single_client(client, is_local_client)
{
    let result = "";
    let client_id = client.client_id;
    let channel_id_of_client = client.channel_id;
    let username = sanitize_string(client.username);
    let alias_text = sanitize_string((client.alias != null) ? client.alias : "");
    let has_alias_html_class = (alias_text.length > 0) ? " has-alias" : "";
    let audio_state = client.audio_state;
    let country_iso_code = client.country_iso_code;
    let is_music_bot = client.is_music_bot;
    // every theme hides this badge by default, so it needs the inline display to show at all.
    // painting it from state means a re-rendered row (channel join, idle) keeps its count
    // instead of silently resetting to 0, which is what the hardcoded markup used to do
    let unread_count = (typeof client.unread_count === "number") ? client.unread_count : 0;

    let client_audio_state_html_class = "client-audio-state-completely-disabled";
    switch (audio_state)
    {
        case 1:
            client_audio_state_html_class = "client-audio-state-microphone-active";
            break;

        case 2:
            client_audio_state_html_class = "client-audio-state-microphone-enabled";
            break;

        case 3:
            client_audio_state_html_class = "client-audio-state-microphone-disabled";
            break;

        case 4:
            client_audio_state_html_class = "client-audio-state-completely-disabled";
            break;
    }

    let additional_client_type_html_class = "";
    if (is_music_bot == true)
    {
        additional_client_type_html_class = "connected-client-p-music-bot"
    }

    let indentation_level = 1;

    if (channel_id_of_client != -2) // if channel is idle channel, (ID -2), do not run this function, program will enter infinite loop
    {
        indentation_level = get_indentation_level(channel_id_of_client, g_channel_list) + 1;
    }

    let is_channel_collapsed = "";
    if (is_channel_or_his_parent_collapsed(channel_id_of_client) == true)
    {
        is_channel_collapsed = "collapsed";
    }

    // the avatar is OFFERED to the tree row as a css variable plus a marker attribute - it
    // is deliberately not painted here. in most themes that circle IS the microphone-state
    // icon (a background-image), so an inline avatar would wipe the mic indicator out. a
    // theme that wants faces in its channel list (termix) opts in with one rule reading
    // var(--client-avatar); everybody else is untouched.
    let avatar_inline_style = "";
    if (g_avatars_allowed == true && typeof client.base64_avatar === "string" && client.base64_avatar.length > 0)
    {
        avatar_inline_style = " data-has-avatar=\"1\" style=\"--client-avatar: url(" + client.base64_avatar + ");\"";
    }

    if (is_local_client)
    {
        result = "<div class=\"connected-local-client connected-client " + is_channel_collapsed + has_alias_html_class + "\" data-connected-client-id=\"" + client_id + "\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                    </div>\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    </div>\n\
                    <input maxlength=\"40\" id=\"connected-local-client-input\" value=\"" + username + "\">\n\
                    <p class=\"client-alias\">" + alias_text + "</p>\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <div class='client-tags' id =\"client-tags-" + client_id + "\">\n\
                    </div>\n\
                </div>";
    }
    else
    {

        let client_mute_state_display = client.is_muted_by_local_client == true ? "block" : "none";
        let client_ignore_state_display = client.is_ignored_by_local_client == true ? "block" : "none";
        let is_idle_client_html = client.is_idle == true ? " idle-client" : "";
        let music_bot_client_html = client.is_music_bot == true ? " music-bot-client" : "";

        result = "<div class=\"connected-client " + is_channel_collapsed + " " + music_bot_client_html + " " + is_idle_client_html + has_alias_html_class + "\" data-connected-client-id=\"" + client_id + "\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                        <div class=\"client-mute-state\" style=\"display: "+client_mute_state_display+";\"></div>\n\
                    </div>\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    </div>\n\
                    <p class=\"connected-client-p "+additional_client_type_html_class+"\">" + username + "</p>\n\
                    <p class=\"client-alias\">" + alias_text + "</p>\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <p class=\"connected-client-p-received-messages-number\" style=\"display: "+((unread_count > 0) ? "inline-block" : "none")+";\">"+unread_count+"</p>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <div class='client-tags' id=\"client-tags-" + client_id + "\">\n\
                    </div>\n\
                    <svg class=\"client-ignore-state\" style=\"margin-left: "+indentation_level * 20+"px; display: "+client_ignore_state_display+";\"  viewBox=\"0 0 100 20\">\n\
                        <line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"20\"></line>\n\
                        <line x1=\"0\" y1=\"20\" x2=\"100\" y2=\"0\"></line>\n\
                    </svg>\n\
                </div>";
    }

    return result;
}

// true when the channel itself or any of its ancestors is collapsed (recursive walk up)
function is_channel_or_his_parent_collapsed(channel_id)
{
    let result = false;
    let channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, channel_id);

    if (channel_index == -1)
    {
        return false;
    }

    if (g_channel_list[channel_index].is_channel_directly_collapsed == false)
    {
        result = is_channel_or_his_parent_collapsed(g_channel_list[channel_index].parent_channel_id);
    }
    else
    {
        result = true;
    }
    return result;
}

// tears the session down to the connect screen while keeping the rsa identity: closes the
// socket, clears the client/channel DOM and resets nearly all connection/chat/voice state
function reset_chat_app_keep_identity()
{
    // no connection, nobody is typing - drop the notices and the ticker with them
    g_typing_state = {};
    render_typing_indicator();

    // whatever triggered this reset (watchdog timeout, socket error, clean close):
    // make sure the socket is REALLY gone. the connect screen showing while an old
    // authenticated socket lived on made every reconnect stall against the zombie
    // session until an admin kicked it. closing an already-closed socket is a no-op,
    // and the onclose this fires re-enters the reset harmlessly (g_is_authenticated is
    // already false by then, so no second alert and nothing left to tear down).
    g_websocket_worker.postMessage({ type: "close" });

    UI.clear_chat_button_onclick();
    UI.clear_chat_button_onclick();

    // the list goes with the map. they used to be emptied 55 lines apart, and a frame
    // arriving in between found an empty map next to a full list
    g_map_client_id_to_array_index.clear();
    g_client_list.length = 0;

    // the session is over, so the stored login frame is a lie. the loopback replays it
    // to any ui that attaches, which showed "connected" and played the sound on every
    // reconnect attempt against a server that was not there
    g_node_cached_auth_frame = null;

    document.getElementById("another-buttons-sub-container").style.display = "";
    document.getElementById("another-buttons-sub-loading-container").style.display = "none";
    document.getElementById("another-buttons-loading-container-p").innerHTML = "loading...";
    set_connect_button_pending(false);
    g_is_microphone_available = false;

    document.getElementById('verification-system').style.display = "block";
    document.getElementById('communication-system-container').style.display = "none";

    // after the page switch, otherwise the mic is repainted while the old screen is still up
    update_microphone_button();

    // back on the connect screen: the in-flow "server settings" button owns this
    // state, a second gear in the bar would only confuse - it returns on connect
    if (g_is_running_in_android_webview)
    {
        document.getElementById("android-settings-button").style.display = "none";
    }

    let elements_count = document.getElementsByClassName("connected-client").length;
    for (let i = 0; i < elements_count; i++)
    {
        document.getElementsByClassName("connected-client")[0].remove();
    }

    g_data_processing_worker.postMessage({
        type: "mainthread_reset_data"
    });

    base64_picture_string_to_send = "" ;
    g_is_websocket_connected = false;
    g_local_username = "";
    local_client_id = 0;
    g_alert_push_to_talk_key_shown_once = false;
    g_alert_streaming_music_shown_once = false;
    g_stop_song_stream_message_received = false;
    g_selected_server_chat_message_id = null;
    g_is_authenticated = false;
    g_should_connection_check_be_running = false;

    // the avatar prefetch timer must not outlive the session - its client ids are meaningless
    // after a reconnect (they are slot indices), and it would keep asking a dead socket
    stop_avatar_prefetch();

    // if the connection died while backgrounded-idle, re-enter idle automatically after reconnect
    g_is_deep_idle_pending = (g_is_deep_idle == true) || g_is_deep_idle_pending;
    g_is_deep_idle = false;
    g_connection_check_interval_ms = 10 * 1000;
    g_connection_check_lost_threshold_ms = 35 * 1000; // keep in sync with the startup value
    current_channel_id = 0;
    g_current_chat_context_id = "chat-context-channel-0";
    g_chat_message_receiver_type = "channel";
    chat_message_receiver_id = "main";
    g_is_client_list_retrieved = false;
    g_is_channel_list_retrieved = false;
    g_is_webrtc_datachannel_connected = false;

    // emptied in place, not reassigned: a fresh array would leave anything still
    // holding the old one reading stale clients. the client list is already cleared above
    g_icons.length = 0;
    g_tags.length = 0;
    g_channel_list.length = 0;

    g_offline_client_list = [];

    g_is_chat_hidden = false;
    layout_apply(); // re-sync the grid (panel visibility) with the reset flag; no-op on touch
    local_message_id = 0;
    g_selected_font = "custom-font-usage-default";
    g_selected_font_color = "#ffffff";
    g_selected_font_size = 12;
    g_is_microphone_enabled_on_touch_device = false;  // for touch devices

    g_connection_check_message_response_received_timestamp = 0;

    selected_channel_id = null;
    selected_client_id = null;
    current_channel_keys = null;
    g_chat_context_array = [
        {
            type: "channel",
            chat_context_id: "chat-context-channel-0",
            last_known_message_sender_username: ""
        }
    ];

    // for voice chat

    g_peer_connection_with_server = null;

    g_iceconfig = null;

    g_is_voice_chat_allowed_by_server = false;
    g_is_client_microphone_allowed_by_server = false;
    g_is_microphone_enabled = false;
    g_is_microphone_active = false;
    g_last_sent_value_microphone_usage = false;

    if (g_local_audio_stream != null)
    {
        if (g_local_audio_stream.getTracks() != null)
        {
            g_local_audio_stream.getTracks()[0].enabled = false;
            // g_local_audio_stream = null; careful
        }
    }

    // activate recorder by assigning function to onaudioprocess

    // if (g_is_client_running_under_touch_device == false)
    // {
    //     window.alert("connection lost, window will reload");
    //     window.location.reload();
    // }

    // completely reloading window fixed microphone issues
}

// applies a client's new audio state: swaps the mic-state css class on their tree row,
// toggles the local push-to-talk controls, and shows the song marquee when streaming
function process_audio_state_of_single_client(client)
{

    let target_element = document.getElementById("client-audio-state-" + client.client_id);

    // an audio state can arrive for a client this runtime has not listed yet; the lookup
    // returns -1 and g_client_list[-1] is undefined, which threw out of the handler
    let client_index = get_client_index_in_array_by_client_id(client.client_id);

    if (client_index == -1 || target_element == null)
    {
        return;
    }

    g_client_list[client_index].audio_state = client.audio_state;

    if (client.audio_state == AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
    {

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-active");

    }
    else if (client.audio_state == AUDIO_STATE.PUSH_TO_TALK_ENABLED)
    {

        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-enabled");

        if (client.client_id == local_client_id)
        {
            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_available = true;
                update_microphone_button();
            }
            else
            {
                document.getElementById("toggle-microphone-label").style.display = "block";
            }
        }
    }
    else if (client.audio_state == AUDIO_STATE.PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS)
    {
        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-disabled");

        if (client.client_id == local_client_id)
        {
            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_available = true;
                update_microphone_button();
            }
            else
            {
                document.getElementById("toggle-microphone-label").style.display = "block";
            }
        }

    }

    else if (client.audio_state == AUDIO_STATE.AUDIO_COMPLETELY_DISABLED)
    {
        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-completely-disabled");

        if (client.client_id == local_client_id)
        {
            g_is_microphone_available = false;
            update_microphone_button();
            document.getElementById("toggle-microphone-label").style.display = "none";

            // audio is enabled by the server and server just disconnected our audio, attempt datachannel reconnect
            if (g_is_voice_chat_allowed_by_server == true)
            {
                g_is_webrtc_datachannel_connected = false;
                if (g_is_webrtc_datachannel_check_running == false)
                {
                    // claim the flag like every other starter - without this the loop ran
                    // unregistered and later triggers stacked concurrent loops on top of it
                    g_is_webrtc_datachannel_check_running = true;
                    webrtc_datachannel_connection_check(true);
                }
            }
        }
    }

    if (client.is_streaming_song)
    {
        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + client.client_id + '"]');
        if (element != null)
        {
            element.style.display = "inline-block";
            document.getElementById("marquee-song-name-client-id-" + client.client_id).innerHTML = sanitize_string(client.song_name);
        }
        else
        {
            console.log("could not find element");
        }
    }
}

// asks the data processing worker to generate fresh channel keys and build the per-client
// encrypted key messages (the rsa keypair only exists in that worker, not on the main thread)
// tells the sender of a private message that it has been read. every id is sent once,
// and only to the person who wrote it
var g_seen_receipts_already_sent = {};

// which conversations have had our latest message read. one eye in the corner of the chat,
// not one on every bubble
var g_seen_state_by_context = {};

function render_seen_indicator()
{
    let indicator = document.getElementById("chat-seen-indicator");

    if (indicator == null)
    {
        return;
    }

    let is_seen = (g_current_chat_context_id != null)
        && (g_seen_state_by_context[g_current_chat_context_id] == true)
        && (g_show_seen_indicator == true);

    indicator.classList.toggle("chat-seen-visible", is_seen);
}

// we just wrote to this person, so our newest message is unread again
function clear_seen_indicator_for_client(client_id)
{
    g_seen_state_by_context["chat-context-pm-" + client_id] = false;
    render_seen_indicator();
}

function mark_message_as_seen(sender_client_id)
{
    g_seen_state_by_context["chat-context-pm-" + sender_client_id] = true;
    render_seen_indicator();
}

// private messages that arrived while their conversation was closed. a receipt is owed
// for each, and is sent when the user actually opens it
var g_unreceipted_messages_by_sender = {};

function remember_message_awaiting_receipt(sender_client_id, server_chat_message_id)
{
    if (server_chat_message_id == null || sender_client_id == local_client_id)
    {
        return;
    }

    if (g_unreceipted_messages_by_sender[sender_client_id] == null)
    {
        g_unreceipted_messages_by_sender[sender_client_id] = [];
    }

    g_unreceipted_messages_by_sender[sender_client_id].push(server_chat_message_id);
}

function send_pending_seen_receipts(sender_client_id)
{
    let pending = g_unreceipted_messages_by_sender[sender_client_id];

    if (pending == null)
    {
        return;
    }

    g_unreceipted_messages_by_sender[sender_client_id] = [];

    for (let x = 0; x < pending.length; x++)
    {
        send_seen_receipt_for_message(sender_client_id, pending[x]);
    }
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

        send_pending_seen_receipts(parseInt(g_current_chat_context_id.split("chat-context-pm-")[1]));
    });
}

// the screen is on and this page is in front of it. headless node always answers no:
// nobody is looking at it, and if it answered yes every read would be receipted twice
function is_the_user_actually_looking()
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

// whether audio can actually be sent right now. false means the datachannel is gone or
// the server disabled audio for us
var g_is_microphone_available = false;

// the one place that decides how the mic button looks. it used to vanish when audio was
// unavailable, which left no way to tell that from "this build has no mic button"
// launching the webview onto a node that is already logged in used to show the connect
// page for a moment before the burst arrived. the page STARTS held back (the spinner is
// the first page); it is revealed only when connecting turns out to fail or stall
var g_is_holding_back_connect_page = true;

// the spinner that covers the held-back connect page, so the user never stares at a blank screen
function set_connect_holdback_loader_visible(is_visible)
{
    let loader = document.getElementById("connect-holdback-loader");

    if (loader != null)
    {
        // opaque, in the theme's color when one paints body/html; some themes (bluebell)
        // paint only inner containers, then a dark fallback keeps page one solid
        if (is_visible == true && typeof getComputedStyle === "function")
        {
            let page_background = getComputedStyle(document.body).backgroundColor;

            if (page_background === "rgba(0, 0, 0, 0)" || page_background === "transparent")
            {
                page_background = getComputedStyle(document.documentElement).backgroundColor;
            }

            if (page_background === "rgba(0, 0, 0, 0)" || page_background === "transparent")
            {
                page_background = "#12151c";
            }

            loader.style.backgroundColor = page_background;
        }

        loader.style.display = (is_visible == true) ? "flex" : "none";
    }
}

// the spinner's reveal deadline; node's "still connecting" reports push it out
var g_connect_holdback_deadline = 0;
var g_connect_holdback_started = 0;

function hold_back_connect_page()
{
    if (g_loopback_port <= 0 || g_is_authenticated == true)
    {
        return;
    }

    if (g_is_holding_back_connect_page == false)
    {
        g_is_holding_back_connect_page = true;
        g_connect_holdback_started = new Date().valueOf();
        document.getElementById("verification-system").style.visibility = "hidden";
        set_connect_holdback_loader_visible(true);

        setTimeout(connect_holdback_check, 250);
    }

    // nothing arrives at all if node is wedged, so the page cannot stay blank forever
    g_connect_holdback_deadline = new Date().valueOf() + 2500;
}

// node reporting "still working on it" keeps the spinner up a little longer, capped
// so a looping connect attempt can never hide the page forever
function extend_connect_page_holdback()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    let now = new Date().valueOf();

    if (now - g_connect_holdback_started > 12000)
    {
        return;
    }

    g_connect_holdback_deadline = now + 4000;
}

function connect_holdback_check()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    if (new Date().valueOf() >= g_connect_holdback_deadline)
    {
        reveal_connect_page();
        return;
    }

    setTimeout(connect_holdback_check, 250);
}

function reveal_connect_page()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    g_is_holding_back_connect_page = false;
    document.getElementById("verification-system").style.visibility = "";
    set_connect_holdback_loader_visible(false);
}

function update_microphone_button()
{
    let button = document.getElementById("microphone-push-to-talk-button-touch-device");

    if (button == null)
    {
        return;
    }

    if (g_is_client_running_under_touch_device == false || g_hide_microphone_button == true)
    {
        button.style.display = "none";
        return;
    }

    // the connect screen has nobody to talk to, so the mic has no business floating over it
    let connect_page = document.getElementById("verification-system");

    if (connect_page != null && connect_page.style.display != "none")
    {
        button.style.display = "none";
        return;
    }

    button.style.display = "block";
    button.classList.toggle("mic-unavailable", g_is_microphone_available == false);
}

function send_seen_receipt_for_message(sender_client_id, server_chat_message_id)
{
    if (is_the_user_actually_looking() == false)
    {
        return;
    }

    if (g_send_seen_receipts == false)
    {
        return;
    }

    if (server_chat_message_id == null || sender_client_id == local_client_id)
    {
        return;
    }

    if (g_seen_receipts_already_sent[server_chat_message_id] == true)
    {
        return;
    }

    let public_key = get_public_key_by_client_id(sender_client_id);

    if (public_key == null || public_key == "")
    {
        return;
    }

    g_seen_receipts_already_sent[server_chat_message_id] = true;

    g_data_processing_worker.postMessage({
        type: "mainthread__create_seen_receipt_message",
        receiver_id: sender_client_id,
        public_key: public_key,
        server_chat_message_id: server_chat_message_id
    });
}

function create_and_send_new_channel_keys()
{
    // reactive protocol runs in ONE runtime: node. the webview reacting too sent a
    // second key set on the same connection and the server kicked it
    if (is_ui_only_runtime())
    {
        return;
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__create_websocket_channel_keys_message",
        clients: g_client_list,
        local_client_id: local_client_id,
        current_channel_id: current_channel_id
    });

    // rsa encryption stays in the data processing worker: the private key lives only there, and
    // the wasm module lemon_crypto uses compiles synchronously, which the main thread disallows.
    // so each user's public key is sent to g_data_processing_worker, which posts the encryption
    // results back for the main thread to route (possibly on to the websocket worker)
}

// rebuilds g_metadata_keys by sha256-hashing the predefined (or form-entered) key strings,
// then hands the key list to the data processing worker
function init_keys_object()
{
    g_metadata_keys.length = 0; // clear it in case client is reconnecting

    if (g_are_server_details_predefined == true)
    {
        for (let i = 0; i < g_autoconnect_details.keys.length; i++)
        {
            let key_string_value = g_autoconnect_details.keys[i];
            // removed: never log key material
            let hash = _sha256.create();

            hash.update(key_string_value);
            hash.array();
            key_bytes = hash.array();

            let single_key = {
                info: "aes-ctr",
                key_string: key_string_value,
                key_bytes: key_bytes
            };

            g_metadata_keys[i] = single_key;
        }
    }
    else
    {
        // iterate the key inputs that actually exist in the dom (in order), not a 0..count
        // range - keys can be removed, which leaves gaps in the input-key-N ids. this also
        // lets the user drop an accidental (even empty) key back to zero without a restart.
        let key_input_elements = document.querySelectorAll('#connect-form-sub-container-2 input[id^="input-key-"]');
        for (let i = 0; i < key_input_elements.length; i++)
        {
            let key_string_value = key_input_elements[i].value;
            // removed: never log key material

            let hash = _sha256.create();

            hash.update(key_string_value);
            hash.array();
            key_bytes = hash.array();

            let single_key = {
                info: "aes-ctr",
                key_string: key_string_value,
                key_bytes: key_bytes
            };

            g_metadata_keys[i] = single_key;
        }
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__metadata_keys",
        value: g_metadata_keys
    });

    // removed: never log key material (g_metadata_keys holds the session key)
}

// finishing an inline chat-message edit: locks the element again and sends the
// edit_chat_message_request with the new content to the server
function local_chat_message_onblur(event)
{
    event.target.setAttribute("contenteditable", "false");
    event.target.removeEventListener('blur', local_chat_message_onblur);

    let message_object = {
        message: {
            type: "edit_chat_message_request",
            message_id: g_selected_server_chat_message_id,
            new_message_value: event.target.innerHTML,
            receiver_type: g_chat_message_receiver_type,
            receiver_id: parseInt(chat_message_receiver_id)
        }
    };

    send_message_object(message_object);
}

// diffie-hellman first step: draws a fresh CSPRNG secret exponent into
// g_dh_secret_exponent and returns the public mix g^x mod p as a bigInt
function get_public_mix()
{
    // secret exponent size: 256 bits for the 2048-bit modulus, otherwise 512. A short exponent of N
    // bits gives N / 2 bits of security (Pollard's lambda), which only has to match the modulus's
    // number-field-sieve strength (~112 / ~145 / ~192 bits for 2048 / 4096 / 8192), so 256 / 512 / 512
    // is already ample; a bigger exponent (modulus / 8 would be 1024 at 8192) just slows modPow.
    let dh_exponent_bits = (dh_modulus_bits == 2048) ? 256 : 512;

    // draw the exponent from a cryptographically secure RNG. Math.random is a non-cryptographic PRNG
    // whose internal state is recoverable from a handful of outputs, so it must never make key material.
    let random_exponent_bytes = new Uint8Array(dh_exponent_bits / 8);
    crypto.getRandomValues(random_exponent_bytes);

    let random_exponent_binary_string = "0b";

    // dh_exponent_bits bits, first bit always 1 (full length), the rest from the secure RNG above
    for (let a = 0; a < dh_exponent_bits; a++)
    {
        if (a == 0)
        {
            random_exponent_binary_string += "1";
        }
        else
        {
            let single_random_bit_stringpart = ((random_exponent_bytes[a >> 3] >> (7 - (a & 7))) & 1) ? "1" : "0";
            random_exponent_binary_string += single_random_bit_stringpart;
        }
    }

    g_dh_secret_exponent = BigInt(random_exponent_binary_string);

    // let A = product.mod(g_dh_modulus);
    let A = lemon_crypto.modpow(g_dh_generator, g_dh_secret_exponent, g_dh_modulus);

    return A;
}

// one dial, driven only by connection_driver(): builds the ws/wss connection string
// and has the websocket worker open the socket with the right opening frame
// fades the connect button while an attempt runs; hiding rows caused a layout flicker
function set_connect_button_pending(is_pending)
{
    let button = document.getElementById("connect-button");

    if (button != null && button.classList != null)
    {
        button.classList.toggle("connect-attempt-pending", is_pending == true);
    }
}

async function attempt_connection(target, identity)
{
    if (is_ui_only_runtime())
    {
        // the page stays exactly as it is; only the button signals the running attempt
        set_connect_button_pending(true);
    }
    else
    {
        document.getElementById("another-buttons-sub-container").style.display = "none";
        document.getElementById("another-buttons-sub-loading-container").style.display = "";
        document.getElementById("another-buttons-loading-container-p").innerHTML = "connecting to server...";
    }

    // a new attempt is in flight - the failure text only shows after this one resolves
    g_last_connect_attempt_failed = false;

    // loopback: connect to node on this device, token as the opening frame, no dh.
    // stun/turn still point at the real server, so keep the real host for webrtc
    if (target.kind === "loopback")
    {
        host = g_autoconnect_details.host;
        port = g_autoconnect_details.port;

        let loopback_connection_string = "ws://127.0.0.1:" + target.port + "/";
        console.log("connection_string -> " + loopback_connection_string + " (loopback)");

        // name the SERVER being joined, not the local hop to node - and never print an
        // unknown one ("connecting to :0" when the settings have not landed yet)
        document.getElementById("another-buttons-loading-container-p").innerHTML =
            (typeof host === "string" && host.length > 0)
                ? "connecting to " + sanitize_string(host) + ":" + sanitize_string("" + port)
                : "connecting...";

        g_websocket_worker.postMessage({
            type: "create_websocket_object",
            value: {
                connection_string: loopback_connection_string,
                onopen_data: target.token
            }
        });

        return;
    }

    host = target.host;
    port = target.port;

    init_keys_object();

    while (g_keys_init_status == false)
    {
        await sleep(100);
    }

    let protocol_part_of_connection_string = "ws://";
    let connection_port = port;

    if (document.location.protocol == "https:")
    {
        protocol_part_of_connection_string = "wss://";
        // an https page must use wss, on the separate stunnel wss port (not the plain ws port)
        if (target.wss_port)
        {
            connection_port = target.wss_port;
        }
    }

    let connection_string = protocol_part_of_connection_string + '' + host + ':' + connection_port + '/';
    console.log("connection_string -> " + connection_string);

    // say WHERE we are connecting: only now are the real host/port known
    document.getElementById("another-buttons-loading-container-p").innerHTML =
        "connecting to " + sanitize_string(host) + ":" + sanitize_string("" + connection_port)
        + "<br>using " + ((protocol_part_of_connection_string == "wss://") ? "secure websocket (wss)" : "websocket (ws)");

    let dh_public_mix = get_public_mix().toString();

    let message_object = {
        message: {
            type: "public_key_info",
            value: identity.public_key_string,
            verification_string: g_verification_message,
            dh_public_mix: dh_public_mix
        }
    };

    let message_json_string = process_message_before_sending(message_object);
    let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

    g_websocket_worker.postMessage({
        type: "create_websocket_object",
        value: {
            connection_string: connection_string,
            onopen_data: data
        }
    });
}

// per-sender frame sequence for outgoing voice/song audio; 16 bits, wraps at 65536
var g_voice_send_sequence_number = 0;

// the server's client-facing policy, applied from authentication_status on join and from the
// "server_policy" broadcast an admin's settings save triggers. absent fields (older server)
// leave the current values untouched, except the file fields, where absent means "off"
function apply_server_policy_fields(data)
{
    // whether admins may register aliases (gates the context menu item)
    g_is_alias_registration_allowed = (data.is_alias_registration_allowed == true);
    g_is_typing_indicator_allowed = (data.allow_typing_indicator == true);

    // rename policy: on unless the server says otherwise (an older server says nothing)
    if (data.allow_client_renames !== undefined)
    {
        g_client_renames_allowed = (data.allow_client_renames == true);
    }
    apply_rename_policy_to_ui();

    // avatars policy arrives in-protocol: the android app loads client.html from its assets,
    // so the serve-time config injection never reaches it
    if (data.allow_avatars !== undefined)
    {
        g_avatars_allowed = (data.allow_avatars == true);
    }
    if (typeof data.avatar_max_size === "number" && data.avatar_max_size > 0)
    {
        g_avatar_max_upload_bytes = data.avatar_max_size;
    }

    g_file_uploads_allowed = (data.allow_file_uploads == true);

    if (typeof data.file_upload_max_size === "number" && data.file_upload_max_size > 0)
    {
        g_file_upload_max_bytes = data.file_upload_max_size;
    }

    // inline pictures: on unless the server says otherwise (an older server says nothing)
    if (data.allow_chat_pictures !== undefined)
    {
        g_chat_pictures_allowed = (data.allow_chat_pictures == true);
    }
    if (typeof data.chat_picture_max_size === "number" && data.chat_picture_max_size > 0)
    {
        g_chat_picture_max_bytes = data.chat_picture_max_size;
    }

    apply_file_upload_policy_to_ui();
    apply_chat_picture_policy_to_ui();
}

// * Callback when main thread receives a message
function mainthread_onmessage(e)
{
    if (DBG_WORKER_BOOT_LOG) { console.log("[m<-w] " + (e.data && e.data.type)); }

    if (e.data.type == "log")
    {
        custom_log(e.data.value);
    }
    else if (e.data.type == "opus_encoder_worker__encode_result")
    {
        // chunks already inside the encoder when push-to-talk ended arrive here after
        // mic-off; dropping them keeps the stale tail out of receivers' jitter lanes
        if (g_is_microphone_active == false && (typeof is_playing_music == "undefined" || is_playing_music != true))
        {
            return;
        }

        if (current_channel_keys != null)
        {
            let opus_data_chunks = e.data.value;

            for (var i = 0; i < opus_data_chunks.length; i++)
            {
                // prepend a 2-byte little-endian sequence number INSIDE the encrypted payload;
                // receivers use it to reorder frames the unordered datachannel scrambled and to
                // detect losses. the server relay never sees it (it sits under the encryption)
                let opus_chunk_bytes = new Uint8Array(opus_data_chunks[i]);
                let sequenced_chunk = new Uint8Array(2 + opus_chunk_bytes.length);

                sequenced_chunk[0] = g_voice_send_sequence_number & 0xff;
                sequenced_chunk[1] = (g_voice_send_sequence_number >> 8) & 0xff;
                sequenced_chunk.set(opus_chunk_bytes, 2);
                g_voice_send_sequence_number = (g_voice_send_sequence_number + 1) & 0xffff;

                let data = encrypt_data_with_aes_keys(current_channel_keys, sequenced_chunk);
                if (data != null && data.byteLength > 0) { g_session_bytes_sent += data.byteLength; }
                g_datachannel.send(data);
            }
        }
        else
        {
            custom_log("dont have channel keys, cant encrypt audio for sending");
        }
    }
    else if (e.data.type == "opus_decoder_worker__decode_result")
    {
        audio_player_write_chunk(e.data.value);
    }
    else if (e.data.type == "minimp3_worker__decode_result")
    {
        let data_chunks = chunk_buffers(e.data.value, 4096);

        let message_object1 = {
            message:
            {
                type: "start_song_stream",
                song_name: g_selected_song_name
            }
        };

        send_message_object(message_object1);

        stream_local_mp3_file_to_other_clients(data_chunks, e.data.mp3_sample_rate);

        if (g_alert_streaming_music_shown_once == false)
        {
            custom_alert("when streaming music from file, detach browser tab where chat is to separate window (if using multiple tabs) or stay focused on tab where chat is otherwise music is not sent reliably and it lags");
            g_alert_streaming_music_shown_once = true;
        }
    }
    else if (e.data.type == "websocket_worker_onmessage")
    {
        if (e.data.value != null && e.data.value.length > 0) { g_session_bytes_received += e.data.value.length; }

        // data from g_websocket_worker are passed to main thread and then to g_data_processing_worker
        g_data_processing_worker.postMessage({
            type: "mainthread__process_received_websocket_message",
            value: e.data.value,
            // loopback frames arrive as plain json, the worker skips decryption
            is_plaintext: is_ui_only_runtime()
        });
    }
    else if (e.data.type == "data_processing_worker__rsa_key_too_weak")
    {
        handle_rsa_key_too_weak_notice(e.data.minimum_rsa_key_bits);
    }
    else if (e.data.type == "websocket_worker_onclose")
    {
        // the driver awaits this signal; it decides whether and when to redial
        g_last_connect_attempt_failed = true;

        // the server never says why - these are inferences from WHERE the socket died.
        // in the webview the socket is the loopback to node, so its close says nothing
        // about the real connection: node owns that story and reports it separately
        if (is_ui_only_runtime() == false)
        {
            if (g_is_authenticated == true)
            {
                g_connection_status.last_connected_at = new Date().valueOf();

                if (g_last_disconnect_reason === "")
                {
                    g_last_disconnect_reason = "the server closed the connection (it probably kicked this client, or shut down)";
                }
            }
            else if (g_last_disconnect_reason === "")
            {
                g_last_disconnect_reason = (g_device_has_network === false)
                    ? "no network connection (wifi and mobile data are off)"
                    : "the server probably rejected the keys (it closed the connection during login)";
            }
        }

        signal_connection_closed();
        console.warn("server connection lost (identity switch in progress: " + g_is_identity_switch_in_progress + ")");
        // a deliberate close (identity switch) is not a lost connection - no scary alert
        if (g_is_authenticated && g_is_identity_switch_in_progress == false)
        {
            if (g_is_running_in_android_webview == false)
            {
                custom_alert("connection with server was lost");
            }
            // window.location.reload();
        }
        g_is_authenticated = false;

        reset_chat_app_keep_identity();
    }
    else if (e.data.type == "websocket_worker_onerror")
    {
        g_last_connect_attempt_failed = true;

        if (g_last_disconnect_reason === "")
        {
            g_last_disconnect_reason = describe_socket_error(e.data.error_code);
        }

        if (g_is_authenticated == true)
        {
            g_connection_status.last_connected_at = new Date().valueOf();
        }

        signal_connection_closed();
        if (g_is_running_in_android_webview == false)
        {
            custom_alert("connecting to server failed");

            // window.alert("connection with server lost"); //this will be window.alert, this is serious problem
        }
        reset_chat_app_keep_identity();

        // window.location.reload();
        g_is_authenticated = false;
    }
    else if (e.data.type == "data_processing_worker__generate_rsa_keypair_result")
    {
        g_rsa_public_key_string = e.data.value;
        console.log("public key string -> " + g_rsa_public_key_string);
        g_is_rsa_key_generated = true;

        // the keygen phase is over: stop claiming it. when nothing will dial on its own the
        // status goes idle (empty), otherwise the driver's "connecting" replaces it right away
        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            report_connection_status("idle", "");
        }
        document.getElementById("another-buttons-sub-loading-container").style.display = "none";
        document.getElementById("another-buttons-sub-container").style.display = "";
        identity_string = e.data.identity_string;

        // wakes the driver if it is waiting to dial with this identity
        g_identity_slot.set({ public_key_string: e.data.value, identity_string: e.data.identity_string });
        g_is_identity_switch_in_progress = false;

        // persist the identity (the passphrase) so the next launch reconstructs this same keypair
        // instead of a fresh random one. covers first launch (random) and identity switches alike:
        // the last identity used is the one remembered.
        if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true)
        {
            try { localStorage.setItem("lemon_identity_string", identity_string); }
            catch (e) { console.warn("failed to persist identity:", e.message); }
        }
    }
    else if (e.data.type == "data_processing_worker__loopback_status")
    {
        // node still working keeps the spinner; a failed or idle state shows the page
        if (e.data.value.state == "connecting" || e.data.value.state == "connected")
        {
            extend_connect_page_holdback();
        }
        else
        {
            reveal_connect_page();
        }

        // node owns the real connection; its report replaces the local guesswork
        g_connection_status.state = e.data.value.state;
        g_connection_status.reason = e.data.value.reason;
        g_connection_status.next_retry_at = e.data.value.next_retry_at;
        g_connection_status.last_connected_at = e.data.value.last_connected_at;
        render_connection_status();
    }
    else if (e.data.type == "data_processing_worker__authentication_status")
    {
        if (e.data.value == "success")
        {
            g_connection_status.last_connected_at = new Date().valueOf();
            g_connection_status.next_retry_at = 0;
            report_connection_status("connected");

            // node that logs in with nobody watching belongs in idle straight away,
            // otherwise it stands in the root channel looking present
            if (typeof node_apply_idle_for_ui_state === "function")
            {
                node_apply_idle_for_ui_state();
            }

            hide_custom_alert(); // if there was any alert

            // the same policy set arrives again as "server_policy" whenever an admin saves,
            // so both paths share one application function
            apply_server_policy_fields(e.data);

            // fresh session: the info card counts from this moment
            g_session_connected_at = new Date().valueOf();
            g_session_bytes_sent = 0;
            g_session_bytes_received = 0;
            g_session_last_ping_ms = -1;

            // ask for the offline-people roster on every connect, from every device and theme.
            // the server decides: it answers only if it offers the list AND this user is
            // registered on it. a refusal is simply g_silence, so nothing here needs to know.
            client_msg.send_request_stored_clients();

            if (e.data.is_idle_mode_allowed)
            {
                document.getElementById('channel-list-container').style.height = "calc(70% - 30px)";
                document.getElementById("idle-channel-collapse-button").addEventListener("mousedown", UI.collapse_expand_channel);
                document.getElementById('idle-clients-container').style.display = "block";
            }

            g_is_authenticated = true;
            console.log("client authenticated");

            g_client_list = g_client_list;
            g_channel_list = g_channel_list;

            if (g_is_running_in_android_webview)
            {
                // connected is when the bar gear appears: the connect screen has its own
                // in-flow "server settings" button, a second one there only confused.
                // tapping it while connected asks first (the warning dialog) - these are
                // the details of the very connection the user is on
                document.getElementById("android-settings-button").style.display = "block";
            }

            // the connected ui is up, so the hold has served its purpose
            g_is_holding_back_connect_page = false;
            document.getElementById('verification-system').style.visibility = "";
            set_connect_holdback_loader_visible(false);
            set_connect_button_pending(false);

            document.getElementById('verification-system').style.display = "none";
            document.getElementById('communication-system-container').style.display = (g_layout_grid_active == true) ? "grid" : "block";

            // the connect screen is gone, so the mic may come back out
            update_microphone_button();

            // people on touch device dont need to see chat by default, current UI isnt suitable for that
            // most likely they will just want to call with voice
            if (g_is_client_running_under_touch_device)
            {
                UI.hide_chat_container();
            }

            if (!g_should_connection_check_be_running)
            {
                g_should_connection_check_be_running = true;
                websocket_connection_check(websocket);
            }

            // the connected sound effect is delayed for 1 second...
            // because at this moment its not yet known if the sound effect should be played or not. why?
            // because, at the moment client.html receives g_client_list message (later, shortly after this)
            // client.html invokes JavaExport onconnected (if client.html is running in android)
            // the invoke causes java to send user settings to androids WebView (client.html)
            // it happens in short time frame
            // so thats why this timeout waits 1 second
            // of course this is a bit useless in web browser context, but web browser can live with the sound effect delayed at 1 second
            // plus minus 1 second doesnt matter in this case
            // and the way its made now, android needs it
            setTimeout( () => {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.connected.play();
                }
            }, 1000);
        }
    }
    else if (e.data.type == "data_processing_worker__audio_enabled")
    {
        // default stun port is 3478 for this chat. Stun port is given to client with websocket connection, stun port can be changed if needed, there are two places that need to be edited in code of server to set different stun port

        g_iceconfig = {
            iceServers: [{
                urls: "stun:" + host + ":" + e.data.stun_port,
            }],
        };

        // the datachannel/playback below is set up whenever audio is on for clients or music bots,
        // so this flag (which keeps the datachannel established/reconnected) is always true here.
        // whether this client may transmit its own microphone is a separate flag, gated on client voice
        g_is_voice_chat_allowed_by_server = true;
        g_is_client_microphone_allowed_by_server = (e.data.client_voice_allowed == true);
        // everything from here down is playback and webrtc, and it all runs in this one
        // handler (see the note further below). a runtime with no webaudio - the headless
        // service - has no opus decoder worker and no RTCPeerConnection either, so there is
        // nothing after this point for it to do. bail before the constructor rather than
        // throwing out of the handler. in a browser this is never null
        if (typeof window.AudioContext !== "function" && typeof window.webkitAudioContext !== "function")
        {
            console.log("no webaudio in this runtime, skipping playback and datachannel setup");
            return;
        }

        // decoded audio is 48 kHz PCM and is pushed into the context untouched (the SpeexResampler below
        // is not wired into the playback path), so ask for a 48 kHz context and let the browser do the
        // device-rate conversion itself. without this, a 44.1 kHz device plays 48 kHz samples ~9% slow
        // (pitch drop) and the playback queue grows ~8% per second - the endlessly accumulating delay
        try
        {
            audio_context = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
        }
        catch (sample_rate_hint_not_supported)
        {
            audio_context = new (window.AudioContext || window.webkitAudioContext)();
        }
        console.log("audio_context.sampleRate" + audio_context.sampleRate);

        // with autoconnect there may have been no user gesture before this point, then the browser
        // creates the context suspended and every played frame is silently discarded. try to resume
        // right away (succeeds when a gesture already happened); if it stays suspended, the
        // first-gesture unlock handler in main.js resumes it on the next click/tap/keypress
        if (audio_context.state === "suspended")
        {
            console.log("audio_context created suspended (autoplay policy, no user gesture yet); audio stays silent until a gesture resumes it");
            audio_context.resume();
        }

        let opus_decoding_sampler_channels = 1;
        let opus_decoding_sampler_input_rate = 48000;
        let opus_decoding_sampler_output_rate = audio_context.sampleRate;

        g_opus_decoding_sampler = new SpeexResampler(opus_decoding_sampler_channels,
            opus_decoding_sampler_input_rate,
            opus_decoding_sampler_output_rate);

        g_opus_decoder_worker.postMessage({
            type: "init",
            sampleRate: opus_decoding_sampler_output_rate
        });

        g_silence = new Float32Array(g_audio_config.codec.bufferSize);
        g_audio_player_gain_node = audio_context.createGain();
        g_audio_player_gain_node.connect(audio_context.destination);

        // AudioWorklet playback when the context supports it, ScriptProcessorNode otherwise.
        // guarded on purpose: everything below runs in this one message handler, so an
        // exception thrown while setting up audio OUTPUT used to abort the handler before it
        // reached the webrtc datachannel check - leaving the client with no datachannel at all
        try
        {
            create_audio_player_output();
        }
        catch (audio_output_setup_error)
        {
            console.log("audio player output setup failed (" + audio_output_setup_error + "); continuing so the datachannel still gets created");
        }

        try
        {
            console.log("create_new_peer_connection_object_for_use");

            // a re-login burst can arrive while a previous check loop is still sleeping;
            // that loop will pick the fresh config up itself, a second one just fights it
            if (g_is_webrtc_datachannel_check_running == false)
            {
                g_is_webrtc_datachannel_check_running = true;
                webrtc_datachannel_connection_check(false);
            }
        }
        catch (Exception)
        {
            custom_log(Exception.toString());
            custom_alert('audio connection failed');
            return;
        }

        // android app went to background while the connect was still in progress - enter idle now
        if (g_is_deep_idle_pending == true)
        {
            g_is_deep_idle_pending = false;
            enter_deep_idle();
        }
    }
    else if (e.data.type == "data_processing_worker__metadata_keys_accepted")
    {
        g_keys_init_status = true;
    }
    else if (e.data.type == "data_processing_worker__client_list_from_server")
    {
        server_msg.process_client_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_avatar_from_server")
    {
        server_msg.process_client_avatar_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__avatar_changed_from_server")
    {
        server_msg.process_avatar_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__connection_check_response")
    {
        g_connection_check_message_response_received_timestamp = new Date().valueOf();

        if (g_session_ping_sent_at > 0)
        {
            g_session_last_ping_ms = g_connection_check_message_response_received_timestamp - g_session_ping_sent_at;
        }
    }
    else if (e.data.type == "data_processing_worker__channel_list_from_server")
    {
        server_msg.process_channel_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_list_from_server")
    {
        server_msg.process_tag_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__identity_list_from_server")
    {
        server_msg.process_identity_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_list_from_server")
    {
        server_msg.process_icon_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_add_from_server")
    {
        server_msg.process_icon_add_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_add_to_client_from_server")
    {
        server_msg.process_add_tag_to_client_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_alias_changed_from_server")
    {
        server_msg.process_client_alias_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__typing_indicator_from_server")
    {
        note_typing_from_client(e.data.value.message.client_id, e.data.value.message.receiver_type, e.data.value.message.receiver_id);
    }
    else if (e.data.type == "data_processing_worker__stored_clients_list_from_server")
    {
        server_msg.process_stored_clients_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__remove_tag_from_client_from_server")
    {
        server_msg.process_remove_tag_from_client_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_add_from_server")
    {
        server_msg.process_tag_add_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_delete_from_server")
    {
        server_msg.process_tag_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_delete_from_server")
    {
        server_msg.process_icon_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_icon_changed_from_server")
    {
        server_msg.process_tag_icon_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_icon_changed_from_server")
    {
        server_msg.process_channel_icon_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__start_song_stream_from_server")
    {
        server_msg.process_start_song_stream_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__stop_song_stream_from_server")
    {
        server_msg.process_stop_song_stream_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_message_delete_from_server")
    {
        server_msg.process_chat_message_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__public_key_challenge_from_server")
    {
        let msg = e.data.value;
        let dh_public_mix_string = msg.message.dh_public_mix;

        let dh_public_mix = BigInt(dh_public_mix_string);

        // reject a degenerate server public mix (B <= 1 or B >= p-1) before using it; such values
        // force a known/tiny shared secret. for a safe prime, requiring 2 <= B <= p-2 is sufficient.
        if (dh_public_mix <= 1n || dh_public_mix >= g_dh_modulus - 1n)
        {
            console.error("rejected degenerate DH public mix from server; aborting handshake");
            return;
        }

        let shared_secret = lemon_crypto.modpow(dh_public_mix, g_dh_secret_exponent, g_dh_modulus);

        // add shared secret to

        // because there is also "secret key" used ,used to further secure connection between server and client, and is different for every connected client, (obtained with steps that wants to look like diffie-hellman key exchange)
        // this secret key needs to be added to metadata keys
        // and then copy of g_metadata_keys object sent to data processing webworker aswell (there are two copies of same object)

        let shared_secret_string = shared_secret.toString();

        // derive the AES enc key + HMAC mac key from the shared secret via HKDF (matches the server)
        let dh_keys = dh_derive_keys(shared_secret_string);
        key_bytes = dh_keys.enc_key;

        let single_key = {
            info: "aes-ctr",
            key_string: shared_secret_string,
            key_bytes: key_bytes,
            mac_bytes: dh_keys.mac_key
        };

        g_metadata_keys.unshift(single_key);

        // removed: never log key material (g_metadata_keys holds the session key)

        // here the g_metadata_keys object within data_processing web worker is updated aswell

        console.log("public_key_challenge_response result -> ", msg.message.decryption_result);

        g_data_processing_worker.postMessage({
            type: "mainthread__metadata_keys",
            value: g_metadata_keys
        });

        let message_object = {
            message: {
                type: "public_key_challenge_response",
                value: msg.message.decryption_result
            }
        };

        // the chosen username rides along on the last login message, because the server
        // assigns the final name right after accepting this response
        if (typeof g_chosen_username === "string" && g_chosen_username.length > 0)
        {
            message_object.message.chosen_username = g_chosen_username;
        }

        send_message_object(message_object);
    }
    else if (e.data.type == "data_processing_worker__chat_message_edit_from_server")
    {
        server_msg.process_chat_message_edit_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__poke_from_server")
    {
        server_msg.process_poke_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__access_denied_from_server")
    {
        server_msg.process_access_denied_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__force_admin_password_change")
    {
        // the admin password set during server setup was typed in cleartext; prompt once for a new one
        let new_admin_password = prompt("The admin password set at server setup was typed in cleartext in the console. Please set a new admin password now.");
        if (new_admin_password != null && new_admin_password != "")
        {
            let message_object = { message: { type: "change_admin_password", value: new_admin_password } };
            send_message_object(message_object);
        }
    }
    else if (e.data.type == "data_processing_worker__client_info_from_server")
    {
        server_msg.process_client_info_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_full_from_server")
    {
        let full_channel = get_channel_by_id(g_channel_list, e.data.value.message.channel_id);
        let full_channel_name = (full_channel != null && full_channel.name != null) ? full_channel.name : "channel";
        custom_alert("'" + full_channel_name + "' is full");
    }
    else if (e.data.type == "data_processing_worker__server_settings_values_from_server")
    {
        let msg = e.data.value;
        document.getElementById("server-settings-general-display-flags-checkbox").checked = msg.message.display_country_flags == true;
        document.getElementById("server-settings-general-enable-audio").checked = msg.message.enable_audio == true;
        document.getElementById("server-settings-general-enable-music-bot-audio-checkbox").checked = msg.message.enable_music_bot_audio == true;
        document.getElementById("server-settings-general-hide-clients-in-password-protected-channels").checked = msg.message.hide_clients_in_password_channels == true;
        document.getElementById("server-settings-general-allow-temp-channels-checkbox").checked = msg.message.allow_temp_channels == true;
        document.getElementById("server-settings-general-allow-typing-indicator-checkbox").checked = msg.message.allow_typing_indicator == true;
        document.getElementById("server-settings-general-allow-renames-checkbox").checked = msg.message.allow_client_renames == true;
        document.getElementById("server-settings-general-allow-text-to-idle-clients-checkbox").checked = msg.message.is_sending_text_to_idle_clients_allowed == true;
        document.getElementById("server-settings-general-allow-private-messages-checkbox").checked = msg.message.allow_private_messages == true;
        document.getElementById("server-settings-general-allow-file-uploads-checkbox").checked = msg.message.allow_file_uploads == true;
        document.getElementById("server-settings-general-file-upload-max-size-input").value = (typeof msg.message.file_upload_max_size_mb === "number") ? msg.message.file_upload_max_size_mb : 10;
        refresh_file_upload_size_visibility();
        document.getElementById("server-settings-general-allow-pictures-checkbox").checked = msg.message.allow_chat_pictures == true;
        document.getElementById("server-settings-general-picture-max-size-input").value = (typeof msg.message.chat_picture_max_size_mb === "number") ? msg.message.chat_picture_max_size_mb : 4;
        refresh_picture_size_visibility();
        document.getElementById("server-settings-general-allow-same-ip-checkbox").checked = msg.message.is_same_ip_address_allowed == true;
        document.getElementById("server-settings-general-minimum-rsa-bits-input").value = (typeof msg.message.minimum_rsa_key_bits === "number") ? msg.message.minimum_rsa_key_bits : 2048;
        document.getElementById("server-settings-general-announce-rsa-bits-checkbox").checked = msg.message.announce_minimum_rsa_key_bits == true;
        document.getElementById("server-settings-general-country-blocking-checkbox").checked = msg.message.is_country_blocking_active == true;
        set_blocked_countries_from_server(msg.message.blocked_countries);
        refresh_country_blocking_visibility();
        UI.render_bans_list(msg.message.bans);
        document.getElementById("server-settings-log-joins-checkbox").checked = msg.message.log_client_joins == true;
        document.getElementById("server-settings-log-renames-checkbox").checked = msg.message.log_username_changes == true;
        document.getElementById("server-settings-log-tags-checkbox").checked = msg.message.log_tag_changes == true;
        document.getElementById("server-settings-log-settings-checkbox").checked = msg.message.log_server_settings_updates == true;
        document.getElementById("server-settings-log-kicks-bans-checkbox").checked = msg.message.log_kicks_and_bans == true;
        document.getElementById("server-settings-log-disconnects-checkbox").checked = msg.message.log_client_disconnects == true;
        document.getElementById("server-settings-log-failed-checkbox").checked = msg.message.log_failed_attempts == true;
        document.getElementById("server-settings-log-max-size-input").value = (typeof msg.message.admin_log_max_size_mb === "number") ? msg.message.admin_log_max_size_mb : 10;
        document.getElementById("server-settings-log-retention-select").value = (typeof msg.message.admin_log_retention_days === "number") ? String(msg.message.admin_log_retention_days) : "7";
    }
    else if (e.data.type == "data_processing_worker__admin_log_from_server")
    {
        server_msg.process_admin_log_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_policy_from_server")
    {
        apply_server_policy_fields(e.data.value.message);
    }
    else if (e.data.type == "data_processing_worker__channel_join_from_server")
    {
        server_msg.process_channel_join_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_chat_message_id_for_local_message_id_from_server")
    {
        server_msg.process_server_chat_message_id_for_local_message_id_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_connect_from_server")
    {
        server_msg.process_client_connect_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_rename_from_server")
    {
        server_msg.process_client_rename_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_disconnect_from_server")
    {
        server_msg.process_client_disconnect_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__audio_state_of_single_client_from_server")
    {

        let client = {
            client_id: e.data.value.message.client_id,
            audio_state: e.data.value.message.value,
        }
        process_audio_state_of_single_client(client);
    }
    else if (e.data.type == "data_processing_worker__current_channel_active_microphone_usage_from_server")
    {
        console.log("data_processing_worker__current_channel_active_microphone_usage_from_server");

        console.log(e.data.value);
        for (var i = 0; i < e.data.value.message.clients.length; i++)
        {
            process_audio_state_of_single_client(e.data.value.message.clients[i]);
        }
    }
    else if (e.data.type == "data_processing_worker__sdp_offer_from_server")
    {
        server_msg.process_sdp_offer_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__ice_candidate_from_server")
    {
        server_msg.process_ice_candidate_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_info_broadcast_from_server")
    {
        server_msg.process_server_info_broadcast_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_edit_from_server")
    {
        server_msg.process_channel_edit_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_going_to_idle_mode")
    {
        server_msg.process_client_client_going_to_idle_mode_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_coming_back_from_idle_mode")
    {
        server_msg.process_client_coming_back_from_idle_mode_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_delete_from_server")
    {
        server_msg.process_channel_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_create_from_server")
    {
        server_msg.process_channel_create_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture_metadata")
    {
        server_msg.process_channel_chat_picture_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture_metadata")
    {
        server_msg.process_direct_chat_picture_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_file_metadata")
    {
        server_msg.process_channel_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_metadata")
    {
        server_msg.process_direct_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file")
    {
        server_msg.process_direct_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_file")
    {
        server_msg.process_channel_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_file_decrypt_failed")
    {
        server_msg.process_chat_file_decrypt_failed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_send_error_from_server")
    {
        server_msg.process_file_send_error_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__seen_receipt_message_result")
    {
        websocket_worker_send(e.data.seen_receipt_message_content);
    }
    else if (e.data.type == "data_processing_worker__channel_maintainer_id")
    {
        server_msg.process_channel_maintainer_id_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_message")
    {
        server_msg.process_channel_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_message")
    {
        server_msg.process_direct_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__offline_chat_message")
    {
        server_msg.process_offline_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture")
    {
        server_msg.process_direct_chat_picture_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture")
    {
        server_msg.process_channel_chat_picture_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__image_sent_status_from_server")
    {
        server_msg.process_image_sent_status_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__call_from_server")
    {
        server_msg.process_call_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__create_websocket_channel_keys_message_result")
    {
        console.log("sending channel keys to user -> " + e.data.username);
        websocket_worker_send(e.data.channel_keys_message_content);
    }
    else if (e.data.type == "data_processing_worker__local_channel_keys_for_ui")
    {
        // node is the maintainer: it generated these keys itself, so no inbound frame
        // exists to forward. deliver our own copy to the attached ui directly
        if (g_node_frame_listener != null)
        {
            console.log("delivering locally generated channel keys to the ui");
            g_node_frame_listener(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__new_channel_keys_from_data_processing_worker")
    {
        current_channel_keys = e.data.value;

        // data processing worker created new channel keys and sent them to UI thread, now send them from UI thread to opus_decoder worker
        g_opus_decoder_worker.postMessage({
            type: "mainthread__channel_keys_for_opus_decoder",
            value: current_channel_keys
        });
    }
    else if (e.data.type == "data_processing_worker__tell_websocket_worker_to_send_data")
    {
        // the last stop before the socket: if a message reaches here and still does not
        // arrive, the fault is past the webview (node or the server), not in it
        custom_log("[send-out] worker produced "
            + (e.data.data_to_be_sent_over_websocket || "").length + " chars for the socket");
        websocket_worker_send(e.data.data_to_be_sent_over_websocket);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts")
    {
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        // the send path took the upload lock before handing the picture to the worker
        file_send_intent = "direct_chat_picture_file";
        file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts")
    {
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        // the send path took the upload lock before handing the picture to the worker
        file_send_intent = "channel_chat_picture_file";
        file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts"
        || e.data.type == "data_processing_worker__channel_chat_file_to_be_uploaded_by_parts")
    {
        // the send path took the upload lock before handing the file to the worker, so this
        // cannot collide with another upload the way a refused picture could
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        file_send_intent = (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts") ? "direct_chat_file" : "channel_chat_file";
        file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__chat_file_send_failed")
    {
        // the worker could not encrypt it (bad receiver key); the card and the lock are ours to clean up
        release_file_upload_lock();
        custom_alert("file not sent: " + e.data.reason);
        mark_local_chat_file_card_failed(e.data.local_message_id, "not sent: " + e.data.reason);
    }
    else if (e.data.type == "data_processing_worker__music_bot_song_list_from_server")
    {
        server_msg.process_music_bot_song_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_send_success_from_server")
    {
        server_msg.process_file_send_success_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_receive_chunk_from_server")
    {
        server_msg.process_file_receive_chunk_from_server_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_receive_completed_from_server")
    {
        server_msg.process_file_receive_completed_from_server_from_server(e.data.value);
    }
}

// hands encrypted data to the websocket worker for sending; also counts the bytes
// into g_session_bytes_sent for the session-info card
function websocket_worker_send(data_to_send)
{
    if (data_to_send != null && data_to_send.length > 0) { g_session_bytes_sent += data_to_send.length; }

    g_websocket_worker.postMessage({
        type: "send",
        value: data_to_send
    });
}

// builds, encrypts and ships one client->server message - the process/encrypt/send
// triple that used to be pasted at every call site
function send_message_object(message_object)
{
    let message_json_string = process_message_before_sending(message_object);
    let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);
    websocket_worker_send(data);
}

// ids of every channel below the given one (fully recursive), used when collapsing
function find_subchannels_of_channel_for_collapse(channel_to_find)
{
    let result = [];

    for (var i = 0; i < g_channel_list.length; i++)
    {
        if (g_channel_list[i].parent_channel_id == channel_to_find)
        {
            result.push(g_channel_list[i].channel_id);
            let result1 = find_subchannels_of_channel_for_collapse(g_channel_list[i].channel_id);
            result = result.concat(result1);
        }
    }

    return result;
}

// ids of channels below the given one, but does not descend into subchannels that are
// themselves directly collapsed (those stay folded when expanding)
function find_subchannels_of_channel_to_expand(channel_to_find)
{
    let result = [];

    for (var i = 0; i < g_channel_list.length; i++)
    {
        if (g_channel_list[i].parent_channel_id == channel_to_find)
        {
            result.push(g_channel_list[i].channel_id);

            if (g_channel_list[i].is_channel_directly_collapsed == true)
            {
                console.log("skipping channel" + g_channel_list[i].name);
                continue;
            }
            else
            {
                console.log("trying to find subchannels of channel " + g_channel_list[i].name);
                let result1 = find_subchannels_of_channel_to_expand(g_channel_list[i].channel_id);
                result = result.concat(result1);
            }
        }
    }

    return result;
}

// client ids of everybody sitting in any of the given channel ids
function find_clients_in_channel(channel_ids)
{
    let result = [];

    for (var i = 0; i < g_client_list.length; i++)
    {
        if (channel_ids.includes(g_client_list[i].channel_id))
        {
            result.push(g_client_list[i].client_id);
        }
    }
    return result;
}

// file extension without the dot, "" when the name has none
function get_file_extension(filename)
{
    var ext = /^.+\.([^.]+)$/.exec(filename);
    return ext == null ? "" : ext[1];
}

// paces the song's mp3 chunks into the opus encoder worker; bails out early when the
// server sends stop_song_stream, otherwise announces the song end after the last chunk
async function stream_local_mp3_file_to_other_clients(data_chunks, mp3_sample_rate)
{
    // a song stream is a fresh spurt like a press: reset the encoder and mark the
    // boundary, so receivers scrub this sender's decoder before the first music frame
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
    g_voice_send_sequence_number = (g_voice_send_sequence_number + OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP) & 0xffff;

    for (var i = 0; i < data_chunks.length; i++)
    {
        let message = {
            type: "encode_mp3_chunk",
            value: data_chunks[i],
            mp3_sample_rate: mp3_sample_rate
        };

        g_opus_encoder_worker.postMessage(message);

        if (g_stop_song_stream_message_received)
        {
            g_stop_song_stream_message_received = false;

            // three situations exist where song stream end must be handled.
            // 1 when clicking on button to pause song,
            // 2 when clicking on button that de-activates microphone
            // 3 when song stops playing when all bytes are sent.
            // all must be handled slightly differently

            // when song is playing and bytes are sent to server using this for loop
            // what makes the loop stop in cases 1 and 2 is "stop_song_stream" message from server
            // when stop_song_stream message from server is handled, g_stop_song_stream_message_received is set to true
            // loop is async and uses await sleep, while loop is not running, g_stop_song_stream_message_received can be set and affect loop

            // if condition is met only if user clicked "pause song" button located near file image select button
            // if user clicks de-activate microphone button, audio_state is set to 3 (deactivated), not checking audio_state
            // would cause the state go from 3 to 2 again..

            if (g_client_list[get_client_index_in_array_by_client_id(local_client_id)].audio_state == AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
            {
                g_is_microphone_active = true;
                is_playing_music = false;
                g_last_sent_value_microphone_usage = 2;

                let message_object2 = {
                    message: {
                        type: "microphone_usage",
                        value: 2,
                    }
                };

                send_message_object(message_object2);
            }

            return;
        }

        // problem with this function is that it sends data either too fast or too slow.
        // loop stops running because it already managed to send all bytes of the song
        // and request is sent to server that makes microphone of the client appear as not active
        // but song is still being played in backgroun in other clients devices

        // also works with 60
        // 100 - starts lagging
        await sleep(80);
    }

    // this is case 3 when song ends because it finished playing
    // update marquee animation and microphone state for other users

    let message_object1 = {
        message:
        {
            type: "stop_song_stream"
        }
    };

    send_message_object(message_object1);

    document.getElementById("custom-file-upload-button-song").style.display = "inline-block";
    document.getElementById("stop-song").style.display = "none";

    g_is_microphone_active = true;
    is_playing_music = false;
    g_last_sent_value_microphone_usage = 2;

    let message_object3 = {
        message: {
            type: "microphone_usage",
            value: 2,
        }
    };

    send_message_object(message_object3);
}

// wav ArrayBuffer -> array of Float32Array chunks: skips the 44-byte header and
// converts the 16-bit signed samples to floats
function chunk_buffers(arrayBuffer, chunkLength)
{
    var chunkedBuffers = [];

    var totalFile = new Int16Array(arrayBuffer);
    // Skip wave header; 44 bytes
    for (i = 22; i < totalFile.length; i += chunkLength)
    {

        // Convert 16 bit signed int to 32bit float
        var bufferChunk = new Float32Array(chunkLength);
        for (j = 0; j < chunkLength; j++)
        {
            bufferChunk[j] = (totalFile[i + j] + 0.5) / 32767.5;
        }

        chunkedBuffers.push(bufferChunk);
    };

    return chunkedBuffers
}

// logs the outgoing message type and returns the message object as a JSON string
function process_message_before_sending(message_object)
{
    let outgoing_type = message_object.message.type;

    if (outgoing_type != "ice_candidate" && outgoing_type != "sdp_answer"
        && outgoing_type != "client_connection_check" && outgoing_type != "create_new_webrtc_datachannel_connection")
    {
        custom_log('[S] msg.message.type : ' + outgoing_type);
    }
    return JSON.stringify(message_object);
}

// ---------------------------------------------------------------
// grid layout engine (desktop only): the three columns (channels, chat, info) and the
// chat-input row live in one css grid on #communication-system-container. column order
// and the input row's column/top-bottom position are re-arrangeable in layout-edit mode
// (the "layout" button); column widths are dragged via the two resize handles. the grid
// minmax() limits make width overshoot structurally impossible (channels >= 150px, chat
// >= 330px) - including on window resize, with no reclamping code. everything persists
// in localStorage under "lemon_layout". touch devices keep the legacy inline-block
// layout: g_layout_grid_active stays false there and every entry point below no-ops.
// ---------------------------------------------------------------
var g_layout_grid_active = false;
var g_layout_edit_active = false;
var g_layout_panels = null;   // name -> panel element, filled at init
var g_layout_drag = null;     // active column-width drag
var g_layout_edit_dragged = null; // panel name being moved in edit mode

var g_layout_state = {
    order: ["channels", "chat", "info"], // left-to-right column order
    input_col: "chat",                   // which column holds the chat-input row
    input_pos: "bottom",                 // "top" or "bottom" inside that column
    col_channels: "15%",                 // channels column width (becomes px after a drag)
    col_info: "13%"                      // info column width
};

// restores g_layout_state from localStorage "lemon_layout" when the saved shape is valid;
// snaps a collapsed info column (saved before the 90px minimum) back to its default width
function layout_load_saved_state()
{
    try
    {
        let raw = localStorage.getItem("lemon_layout");
        if (raw == null) { return; }
        let saved = JSON.parse(raw);
        if (saved != null && Array.isArray(saved.order) && saved.order.length == 3
            && saved.order.indexOf("channels") != -1 && saved.order.indexOf("chat") != -1
            && saved.order.indexOf("info") != -1 && saved.input_col != null && saved.input_pos != null)
        {
            // a collapsed info column (saved before the 90px minimum existed) looks like
            // the panel was deleted - snap it back to the default width
            if (parseInt(saved.col_info, 10) < 90) { saved.col_info = "13%"; }
            g_layout_state = saved;
        }
    }
    catch (e) { console.warn("layout state restore failed:", e.message); }
}

// persists g_layout_state to localStorage under "lemon_layout"
function layout_save_state()
{
    try { localStorage.setItem("lemon_layout", JSON.stringify(g_layout_state)); } catch (e) { console.warn("failed to save layout state:", e.message); }
}

// (re)builds the grid templates from g_layout_state + g_is_chat_hidden and pins the panels
// to their areas. does not touch the container's display - connect/disconnect owns that
function layout_apply()
{
    if (g_layout_grid_active == false) { return; }

    let container = document.getElementById("communication-system-container");

    // fill everything under the head menu: the themes' height:80% left a dead band at
    // the bottom (the legacy layout covered it only by overflowing past the container)
    let head_menu = document.getElementById("communication-system-head-menu");
    container.style.height = "calc(100vh - " + ((head_menu != null) ? head_menu.offsetHeight : 30) + "px)";

    let order = g_layout_state.order.slice();
    if (g_is_chat_hidden == true)
    {
        order = order.filter(function(col) { return col != "chat"; });
    }

    // the input row lives inside the chat column, because the composer must align with the
    // chat panel above it - same left edge, same right edge, in every theme
    let row_a = [], row_b = [];
    for (let i = 0; i < order.length; i++)
    {
        let col = order[i];
        let holds_input = (g_is_chat_hidden == false && g_layout_state.input_col == col);
        row_a.push((holds_input && g_layout_state.input_pos == "top") ? "input" : col);
        row_b.push((holds_input && g_layout_state.input_pos == "bottom") ? "input" : col);
    }

    let widths = order.map(function(col) {
        if (col == "channels") { return "minmax(150px, " + g_layout_state.col_channels + ")"; }
        if (col == "info") { return "minmax(90px, " + g_layout_state.col_info + ")"; } // min 90: collapsing to 0 made the panel look deleted
        return "minmax(330px, 1fr)"; // chat soaks up the leftover space
    });
    if (g_is_chat_hidden == true && order.indexOf("channels") != -1)
    {
        widths[order.indexOf("channels")] = "minmax(150px, 1fr)"; // chat gone: channels takes the space
    }

    container.style.gridTemplateAreas = '"' + row_a.join(" ") + '" "' + row_b.join(" ") + '"';
    container.style.gridTemplateColumns = widths.join(" ");
    container.style.gridTemplateRows = (g_layout_state.input_pos == "top") ? "auto minmax(0, 1fr)" : "minmax(0, 1fr) auto";

    g_layout_panels.channels.style.gridArea = "channels";
    g_layout_panels.chat.style.gridArea = "chat";
    g_layout_panels.info.style.gridArea = "info";
    g_layout_panels.input.style.gridArea = "input";

    // neutralize the legacy inline-block sizing: the grid alone decides the geometry
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++)
    {
        let panel = g_layout_panels[names[i]];
        panel.style.width = "auto";
        panel.style.minWidth = "0";
        panel.style.height = "auto";
        panel.style.left = "0px";
    }

    // no left inset here: the input sits in the chat column, so the corner mic button is
    // outside it and the composer starts flush with the chat panel's edge
    g_layout_panels.input.style.paddingLeft = "";

    document.getElementById("space-devider3").style.display = "none"; // legacy spacer row, obsolete in the grid

    g_layout_panels.chat.style.display = (g_is_chat_hidden == true) ? "none" : "block";
    g_layout_panels.input.style.display = (g_is_chat_hidden == true) ? "none" : "block";
}

// ---- column-width dragging (the 2px line at the chat panel's left edge, and the
// ---- matching handle at the info panel's left edge) ----

// the handle sits at a panel's left edge, so the drag moves the boundary between that
// panel and its left neighbour: neighbour width += dx, own width -= dx. the elastic
// chat column (1fr) has no stored width - when it is one of the pair, adjusting only
// the other column achieves the same boundary move (the fr track absorbs the rest)
function layout_column_drag_start(e, panel_name)
{
    if (g_layout_grid_active == false || g_layout_edit_active == true) { return; }

    let order = g_layout_state.order;
    let idx = order.indexOf(panel_name);
    if (idx == -1 || idx == 0) { return; } // leftmost: no boundary to its left

    let neighbour = order[idx - 1];
    let width_of = function(name) { return Math.round(g_layout_panels[name].getBoundingClientRect().width); };

    // dx limits so no column in the pair leaves its minimum (channels 150px, info 0px);
    // the chat minimum (330px) is guarded by the single-target max computed in the move handler
    let dx_min = -Infinity, dx_max = Infinity;
    let targets = [];
    if (neighbour != "chat")
    {
        let mn = (neighbour == "channels") ? 150 : 90;
        targets.push({ name: neighbour, sign: 1, start: width_of(neighbour) });
        dx_min = Math.max(dx_min, mn - width_of(neighbour));
    }
    if (panel_name != "chat")
    {
        let mn = (panel_name == "channels") ? 150 : 90;
        targets.push({ name: panel_name, sign: -1, start: width_of(panel_name) });
        dx_max = Math.min(dx_max, width_of(panel_name) - mn);
    }

    g_layout_drag = {
        targets: targets,
        pair_has_chat: (neighbour == "chat" || panel_name == "chat"),
        dx_min: dx_min,
        dx_max: dx_max,
        start_x: e.clientX
    };
    document.documentElement.addEventListener("mousemove", layout_column_drag_move, false);
    document.documentElement.addEventListener("mouseup", layout_column_drag_stop, false);
    e.preventDefault();
}

// live column drag: clamps dx to the stored limits (plus the chat 330px minimum when chat
// is the elastic side), writes the new widths into g_layout_state and re-applies the grid
function layout_column_drag_move(e)
{
    if (g_layout_drag == null) { return; }

    let dx = e.clientX - g_layout_drag.start_x;

    if (g_layout_drag.pair_has_chat == true && g_layout_drag.targets.length == 1)
    {
        // chat is the elastic side of the pair: cap the single target so chat keeps >= 330px
        let container = document.getElementById("communication-system-container");
        let total = container.getBoundingClientRect().width;
        let t = g_layout_drag.targets[0];
        let other = (t.name == "channels") ? "info" : "channels";
        let other_width = Math.round(g_layout_panels[other].getBoundingClientRect().width);
        let max_width = total - other_width - ((g_is_chat_hidden == false) ? 330 : 0) - 8;
        if (t.sign == 1) { dx = Math.min(dx, max_width - t.start); }
        else { dx = Math.max(dx, t.start - max_width); }
    }

    if (dx < g_layout_drag.dx_min) { dx = g_layout_drag.dx_min; }
    if (dx > g_layout_drag.dx_max) { dx = g_layout_drag.dx_max; }

    for (let i = 0; i < g_layout_drag.targets.length; i++)
    {
        let t = g_layout_drag.targets[i];
        let new_width = t.start + t.sign * dx;
        if (t.name == "channels") { g_layout_state.col_channels = new_width + "px"; }
        else { g_layout_state.col_info = new_width + "px"; }
    }

    layout_apply();
}

// ends a column drag: persists the widths once and removes the document listeners
function layout_column_drag_stop()
{
    if (g_layout_drag != null) { layout_save_state(); }
    g_layout_drag = null;
    document.documentElement.removeEventListener("mousemove", layout_column_drag_move, false);
    document.documentElement.removeEventListener("mouseup", layout_column_drag_stop, false);
}

// ---- layout-edit mode: drag whole panels to re-arrange, then lock ----

// toggles layout-edit mode (body attribute + button label); leaving it saves the
// layout and clears any in-progress panel drag
function layout_edit_toggle()
{
    g_layout_edit_active = !g_layout_edit_active;
    document.body.setAttribute("data-layout-edit", g_layout_edit_active ? "1" : "0");
    document.getElementById("layout-edit-button").value = g_layout_edit_active ? "lock layout" : "layout";
    if (g_layout_edit_active == false)
    {
        layout_save_state();
        layout_edit_clear_highlight();
        g_layout_edit_dragged = null;
    }
}

// which layout panel the event landed in ("channels"/"chat"/"info"/"input"), or null
function layout_panel_name_from_event(e)
{
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++)
    {
        if (g_layout_panels[names[i]].contains(e.target)) { return names[i]; }
    }
    return null;
}

// removes the dragging/drop-target highlight classes from all four panels
function layout_edit_clear_highlight()
{
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++)
    {
        g_layout_panels[names[i]].classList.remove("layout-drop-target");
        g_layout_panels[names[i]].classList.remove("layout-dragging");
    }
}

// edit mode: starts dragging the panel under the cursor and marks it visually
function layout_edit_mousedown(e)
{
    if (g_layout_edit_active == false || g_layout_grid_active == false) { return; }
    let name = layout_panel_name_from_event(e);
    if (name == null) { return; }
    g_layout_edit_dragged = name;
    g_layout_panels[name].classList.add("layout-dragging");
    e.preventDefault();
    e.stopPropagation();
}

// edit mode: highlights the panel currently hovered over as the drop target
function layout_edit_mousemove(e)
{
    if (g_layout_edit_dragged == null) { return; }
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++) { g_layout_panels[names[i]].classList.remove("layout-drop-target"); }
    let over = layout_panel_name_from_event(e);
    if (over != null && over != g_layout_edit_dragged)
    {
        g_layout_panels[over].classList.add("layout-drop-target");
    }
}

// edit mode drop: swaps two columns, or parks the input row in a column (dropping it
// on its own column flips top/bottom), then re-applies the grid
function layout_edit_mouseup(e)
{
    if (g_layout_edit_dragged == null) { return; }
    let source = g_layout_edit_dragged;
    let target = layout_panel_name_from_event(e);
    g_layout_edit_dragged = null;
    layout_edit_clear_highlight();
    if (target == null || target == source) { return; }

    if (source == "input" || target == "input")
    {
        // moving the input row: dropping it on a column parks it there; dropping it on
        // the column it already lives in flips it between top and bottom
        let col = (source == "input") ? target : source;
        if (g_layout_state.input_col == col)
        {
            g_layout_state.input_pos = (g_layout_state.input_pos == "top") ? "bottom" : "top";
        }
        else
        {
            g_layout_state.input_col = col;
        }
    }
    else
    {
        // two columns: swap their places
        let a = g_layout_state.order.indexOf(source);
        let b = g_layout_state.order.indexOf(target);
        g_layout_state.order[a] = target;
        g_layout_state.order[b] = source;
    }
    layout_apply();
}

var g_show_hide_toggle = false;

// stops event bubbling; wired where a click must not reach parent handlers
function stop_propagation(event)
{
    event.stopPropagation();
}

// mirrors the three toggle states onto css classes on their toolbar buttons. themes that
// want state-specific g_icons (oldschool) style .sfx-on / .mic-on / .chat-hidden with
// !important, which overrides the inline background-image the default theme sets via js, so
// only those themes are affected. safe to call anytime; it just reads the current bools.
function sync_toolbar_state_classes()
{
    let sfx = document.getElementById("sound-effects-button");
    let mic = document.getElementById("microphone-always-broadcasting-audio-button");
    let hide = document.getElementById("hide-chat-button");
    if (sfx != null) { sfx.classList.toggle("sfx-on", g_are_sound_effects_enabled == true); }
    if (mic != null) { mic.classList.toggle("mic-on", g_is_microphone_always_on == true); }
    if (hide != null) { hide.classList.toggle("chat-hidden", g_is_chat_hidden == true); }
}

// requests getUserMedia (with a plain-HTTP explainer when unavailable), builds the mic
// capture chain once, and reports microphone_usage=2 (enabled, not sending) to the server
function activate_microphone()
{
    // getUserMedia is only exposed in a "secure context". in practice that means the page was
    // loaded one of these ways:
    // 1. over HTTPS with a valid certificate, or
    // 2. from localhost (http://localhost or http://127.0.0.1), or
    // 3. straight from disk as a file:// page (yes, opening the .html from the desktop works too)
    // when the client is instead served over plain HTTP from a remote host the context is not
    // secure, so navigator.mediaDevices is undefined; accessing it would throw and the mic would
    // silently never activate. detect that and tell the user, instead of the old dead
    // navigator.getUserMedia shim (which could itself throw)
    if (navigator.mediaDevices == null || typeof navigator.mediaDevices.getUserMedia != "function")
    {
        custom_alert("the microphone needs a secure connection: open the client over HTTPS, from localhost, or as a local file. you are most likely loading it over plain HTTP.");
        return;
    }

    let microphone_constraints = { audio: true };

    // a chosen input device rides along as "ideal", because "exact" would fail the whole mic
    // activation when that device is unplugged - the browser then falls back to its default
    if (g_selected_microphone_device_id != "")
    {
        microphone_constraints = { audio: { deviceId: { ideal: g_selected_microphone_device_id } } };
    }

    navigator.mediaDevices.getUserMedia(microphone_constraints)
    .then(
        function (stream)
        {
            if (g_alert_push_to_talk_key_shown_once == false)
            {
                // only display the alert if touch device is not used
                if (!g_is_client_running_under_touch_device)
                {
                    custom_alert("press Q to talk");
                }
                g_alert_push_to_talk_key_shown_once = true;
            }

            document.getElementById("play-pause-song-container").style.visibility = "visible";
            document.getElementById("stop-song").style.display = "none";
            document.getElementById("custom-file-upload-button-song").style.visibility = "visible";
            document.getElementById("custom-file-upload-button-song").style.display = "block";

            // only do this once
            if (g_microphone_recorder == null)
            {
                g_opus_encoder_worker.postMessage({
                    type: "init",
                    sampleRate: audio_context.sampleRate
                });

                stream.getTracks()[0].enabled = false;

                g_local_audio_stream = stream;
                g_audio_input = audio_context.createMediaStreamSource(stream);
                g_audio_recorder_gain_node = audio_context.createGain();

                // AudioWorkletNode capture when the worklet module is available, ScriptProcessorNode
                // otherwise; also wires mic -> gain -> capture node -> destination
                create_microphone_capture_node();

                const audioTracks = stream.getAudioTracks();

                custom_log('Using audio device: ' + audioTracks[0].label);
                stream.oninactive = function ()
                {
                    custom_log('Stream ended');
                };
            }

            g_is_microphone_enabled = true;

            document.getElementById("play-pause-song-container").style.visibility = "visible";
            document.getElementById("toggle-microphone-label").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAE30lEQVR4Xu2ae+jfUxjHX2MyImxJ5hKSrLX5T/xpNZeUkcsfiCSstTFrbRTij03mtpFEolzKJffLpqTUUlptjBqR27BFsdxyp3c7nzy/0/f3+5znfM75RPuc/z7f33Oe5znv53p+55lEv+twYB5wOnAEcGgQ/wXwKfAS8DywtS+1JvUk6BDgBuBSYPcWmX8BTwPLAihVVewDgDOBR4B9nCf5AbgQeMG5z0VeG4CrgDuA3Vxa/Ussb7gauCtzf+u2mgDI8nLl3MM3yguEs2p5Qi0AlNy2ZLj9eBZTOBwLfNVqUidBLQAeBC5x6tJG/gBwWRuR9+81AFCp+zgh23t1/TOUTpXMYqsGAFcCa4ppOJbRQuCekrxrALAWOLWkkobXK6GJKsa+BgAfAkcX03Asow9CMizGvgYAytjepif1QD8C+6YSp9DVAODvFMEdaIrqXJRZONQAQAfrpmwtarSizAYP2InAEAIpftyBpqjXFmU2hMAQAkMOGJLgUAV2gTIY1/m4kvTdB7TpM2HFzSmDbQIHADo0OSlb2zzOZVQX8TiNTptCKYfy0LTJc53JRTwAMLrTa7OIx7optG3yXEZ1EQ8e8N/zgMnA75HbuIzqIg6CfgP2MEKnAL+a75r/FP0e2M/IOjh6LpNue6bEUUOTA8B3wP5GyEHA1+Zb/7o+xqOEg/Z9YIahnw28Y76/BaY5+JEDwHvATCPkBOAt893nw4heoJ81st8FBEryygFAAiW4WVcA95tvPV/dnayBj3ABcK/ZshrQDEKzngHO9rDMAeBaYKUR8hRwnvk+DPikwuPoH8CRgH0cfRs4zsheDqyqDcCJwJtGiBLTdOAn85uesjUPVHLdB8w3DBWGcnlrxOOBDR6hOR6giQ9NcenQ44WBhqI0IFHqGUsga0Bim5EpQC433/IMPc27LmM5AEimJr5uMsI1D6DsrDLUrNOAFwuEgkZklHPEq1kqfx8Be5vfrgNWeKwv2lwApgKfRY+g1wC3RApoVuDODnNCOvziEUn1SeBcI0u9h6y/oy8AJOd2YIkRqGZIMbg5UuIM4NGMcJDbXxCGJy1LDVo+F8m4NcwVes+f7QESpGZIh1XWb5amPZUkt0eaHAjIRVXG1L5OtGT1xwBldBvz2jMLeAM4wDD4MvyuBs29ckOgETQXeDUKJYFyyggQtEfTY82orEqaHZVVHnk5jMONmgM6ClgPKP6bpYR3MvCa++RhQ1cAxEYxrji1S4c5B9iUq1i0bw7wOCBPskuybRi6xZUAQLO/DwPnR9J/AW4MANnq4FFyL2BpqDpx6ChMLgY0PZa9SgAg4VJOCtmOsFFK5eq2MC/8c6KmKm8XAddH/Uaz/YmQIDsdXsxKAdCAoDosi40aj9V8jy5KrwMbQzP1Tbi+aqZIOUEXGY3SK7fI+vFSglSrK2DUGndeJQFolFG8ajrcdoqdFQ1Xbrn8uhLMGh41ABBv3cmVnBZl1P/4fOoHdLvU1Lnu+0VXLQAaJdUx6rqquWHbL6Qc4nPgoTAqX/zgtT0gPqCAVpd4EnBzy+l13Vae0K3OdbFJQXWUYjn7uuxpO1Rtrxyje6/CguQBgBb36dUovQobPGAnAkMIDCEwMQK9hmWvwnaVHNAW4116CO0tarSizBItPADQEYGiRivK7P/oAf8Ae0zZQdfLrKYAAAAASUVORK5CYII=)";

            let message_object = {
                message: {
                    type: "microphone_usage",
                    value: 2,
                }
            };

            let message_json_string = process_message_before_sending(message_object);
            let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

            websocket_worker_send(data);

            g_last_sent_value_microphone_usage = 2;

            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_enabled_on_touch_device = true;
            }

            // legacy always-on used to hide the button and open the mic right here;
            // continuous mode is a tap-toggle now, so the button stays and stays quiet

            // desktop continuous armed the flag before getUserMedia resolved, so the
            // start call found g_is_microphone_enabled false and returned silently
            if (g_is_continuous_transmission_active == true)
            {
                process_start_sending_audio();
            }

        },
        function (fail) {
            console.log("activate_microphone is acting weird");
            console.log(fail);
        }
    )
    .catch(function (error)
    {
        const errorMessage = 'navigator.MediaDevices.getUserMedia error: ' + error.message + ' ' + error.name;
        console.log(errorMessage);
        document.getElementById("toggle-microphone").checked = false;
    });
}

// page entry point: detects android webview / touch device, applies predefined server
// autoconnect config, sets up ui visibility, gestures and all element event handlers
async function window_onload()
{
    if (typeof Android !== 'undefined')
    {
        g_is_running_in_android_webview = true;
        g_is_client_running_under_touch_device = true;
        g_is_autoconnect_without_user_action_active = false;
        g_are_server_details_predefined = false;
    }

    // touch detection lives in platform-detection.js; the android webview force above stays
    if (detect_touch_device() == true)
    {
        g_is_client_running_under_touch_device = true;
    }

    if (g_is_running_in_android_webview == true)
    {
        // in android, connection settings are stored in its own container..
        // these ui elements are not needed
        document.getElementById("add-key-button").style.visibility = "hidden";
        document.getElementById("connect-form-sub-container-1").style.visibility = "hidden";
        document.getElementById("connect-form-sub-container-2").style.visibility = "hidden";
    }

    // web: if the server baked connection details into the page, enable autoconnect before the form-visibility logic below runs; hide only the connect form (ip/port), leaving the loading/connecting status visible so the page isn't blank while it connects
    if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.port && g_is_running_in_android_webview == false)
    {
        g_are_server_details_predefined = true;
        g_is_autoconnect_without_user_action_active = true;
        g_autoconnect_details = {
            host: window.location.hostname,
            port: window.__SERVER_CONFIG__.port,
            keys: window.__SERVER_CONFIG__.keys,
            wss_port: window.__SERVER_CONFIG__.wss_port
        };
        document.getElementById("connect-form-sub-container-1").style.display = "none";
        document.getElementById("connect-form-sub-container-2").style.display = "none";

        // the address is baked in, so the fields bookmarks fill are never consulted
        document.getElementById("server-bookmarks-container").style.display = "none";
    }

    if (g_is_running_in_android_webview == false)
    {
        if (g_are_server_details_predefined == false)
        {
            document.getElementById("connect-form-sub-container-1").style.visibility = "visible";
            document.getElementById("connect-form-sub-container-2").style.visibility = "visible";
            document.getElementById("add-key-button").style.visibility = "visible";
            document.getElementById("connect-button").style.visibility = "visible";
        }

        if (g_are_server_details_predefined == true && g_is_autoconnect_without_user_action_active == false)
        {
            document.getElementById("connect-button").style.visibility = "visible";
        }

        if (g_is_autoconnect_without_user_action_active == true)
        {
            document.getElementById("import-identity-button").style.display = "none";
        }
    }

    asm = create_opus_webassembly_instance(); // do not delete this line

    document.getElementById("another-buttons-loading-container").style.backgroundImage = "url(" + g_loading_gif + ")";

    g_textarea_log = document.getElementById("textarea-log");

    // -----------------------------------------------------------------
    // member-strip gestures. themes that use the right-pane member list
    // as their people surface tap the CLONES, but clones are
    // rebuilt on every sync and never carry the tree rows' handlers.
    // delegated listeners forward the gestures to the real elements:
    //  tap a person circle    -> the row's short-click path (open chat)
    //  hold a person circle   -> the row actions menu (600ms, like the tree)
    //  tap the channel circle -> show the channel chat again
    //  hold the channel circle OR a channel pill -> channel switch list
    // themes that keep #member-list-container hidden never fire these.
    // both touch and mouse listeners are registered; the handlers check
    // g_is_client_running_under_touch_device so the browser's synthesized
    // mouse events after a tap can never double-fire an action.
    // -----------------------------------------------------------------

    function member_strip_show_channel_switch_menu(click_x, click_y)
    {
        UI.delete_contextmenus();

        let menu_items_html = "";
        for (let i = 0; i < g_channel_list.length; i++)
        {
            let channel = g_channel_list[i];
            let lock_html = (channel.is_using_password == true) ? "🔒 " : "";
            let current_html_class = (parseInt(channel.channel_id) == parseInt(current_channel_id)) ? " context-menu-item-disabled" : "";
            // context-menu-item keeps document_onmousedown from closing the menu
            // before the item's own click handler can run
            menu_items_html += "<p class='context-menu-item channel-switch-menu-item" + current_html_class + "' data-switch-channel-id='" + channel.channel_id + "'>" + lock_html + sanitize_string(channel.name) + "</p>\n";
        }

        let menu_html = "<div class=\"channel-list-context-menu\" style=\"top: " + click_y + "px; left: " + click_x + "px;\">\n\
                            <div class='channel-list-context-menu-background'>\n\
                            </div>\n\
                            <div class='channel-list-context-menu-items'>\n\
                                " + menu_items_html + "\n\
                            </div>\n\
                        </div>";

        document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", menu_html);

        let switch_items = document.getElementsByClassName("channel-switch-menu-item");
        for (let i = 0; i < switch_items.length; i++)
        {
            switch_items[i].onclick = function()
            {
                let switch_channel_id = this.getAttribute("data-switch-channel-id");
                UI.delete_contextmenus(true);

                // same flow as double clicking the channel in the tree: handles the
                // current-channel guard and the password dialog for locked channels
                let name_element = document.querySelector('#channel-list-container .single-channel[data-channel-id="' + switch_channel_id + '"] [data-channel-name-id]');
                if (name_element != null)
                {
                    UI.single_channel_doubleclick_join({ stopPropagation: function() {}, target: name_element });
                }
            };
        }
    }

    // returns true when the gesture landed on a circle and was handled - the
    // caller then stops the event so document_onmousedown does not instantly
    // close whatever menu the gesture opened (the tree rows' own handler
    // protects itself the same way)
    function member_strip_forward(target_element, click_x, click_y, is_short_click)
    {
        let channel_circle = target_element.closest(".member-list-channel");
        if (channel_circle != null)
        {
            if (is_short_click == true)
            {
                // bring the channel chat back on screen - same as tapping its pill. a pill
                // fresh from the html has no handler yet - give it the standard one
                let channel_pill = document.querySelector('.chat-context-selector[data-chat-context-selector-id="channel-' + channel_circle.getAttribute("data-member-list-channel-id") + '"]');
                if (channel_pill != null)
                {
                    if (channel_pill.onclick == null)
                    {
                        channel_pill.onclick = UI.chat_context_selector_onclick;
                    }

                    channel_pill.click();
                }
            }
            else
            {
                member_strip_show_channel_switch_menu(click_x, click_y);
            }
            return true;
        }

        let clone = target_element.closest(".member-list-client");
        if (clone == null) { return false; }

        // offline contacts have no live row to forward to. a HOLD shows what we know about
        // them; a TAP opens their conversation when the server keeps messages for people
        // who are away (we can tell: we were given their public key), else the info popup
        if (clone.classList.contains("offline-contact") == true)
        {
            let offline_alias_element = clone.querySelector(".client-alias");
            let offline_alias = (offline_alias_element != null) ? offline_alias_element.textContent : "";
            let offline_contact = get_stored_client_by_alias(offline_alias);

            // a TAP opens the conversation like it does for anybody else - the chat itself
            // says whether it can actually be delivered. a HOLD shows their details.
            if (is_short_click == true && offline_contact != null)
            {
                UI.open_offline_chat_context(offline_contact);
            }
            else
            {
                UI.show_offline_contact_info_popup(offline_alias);
            }
            return true;
        }

        let strip_client_id = clone.getAttribute("data-connected-client-id");
        if (strip_client_id == null) { return false; }

        let tree_row = document.querySelector('#channel-list-container .connected-client[data-connected-client-id="' + strip_client_id + '"], #idle-clients-container .connected-client[data-connected-client-id="' + strip_client_id + '"]');
        if (tree_row == null) { return false; }

        // the row handler accepts an explicit target + coordinates, so the strip can
        // hand it the REAL tree row no matter which clone was tapped
        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, click_x, click_y, tree_row, is_short_click);
        return true;
    }

    let member_strip_body = document.getElementById("member-list-body");
    if (member_strip_body != null)
    {
        let member_strip_press_timer = null;
        let member_strip_long_press = false;
        let member_strip_moved = false;

        member_strip_body.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            member_strip_long_press = false;
            member_strip_moved = false;

            const target_element = event.target;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            member_strip_press_timer = window.setTimeout(function() {
                member_strip_long_press = true;
                member_strip_forward(target_element, click_x, click_y, false);
            }, 600);
        }, { passive: true });

        member_strip_body.addEventListener("touchmove", function() {
            // finger is scrolling the strip, not pressing a circle
            member_strip_moved = true;
            clearTimeout(member_strip_press_timer);
        }, { passive: true });

        member_strip_body.addEventListener("touchend", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            clearTimeout(member_strip_press_timer);

            if (member_strip_long_press == false && member_strip_moved == false)
            {
                member_strip_forward(event.target, event.changedTouches[0].clientX, event.changedTouches[0].clientY, true);
            }
        });

        member_strip_body.addEventListener("mousedown", function(event) {
            if (g_is_client_running_under_touch_device == true) { return; }

            // left opens, right = actions / channel list - same split the tree rows use
            if (event.which == 1 || event.which == 3)
            {
                if (member_strip_forward(event.target, event.clientX, event.clientY, event.which == 1) == true)
                {
                    event.stopPropagation();
                }
            }
        });
    }

    // holding a channel pill offers the channel switch list too, but only while a
    // theme hides the channel tree (the strip themes) - elsewhere the tree does that job
    let context_pill_container = document.getElementById("chat-context-selectors-container");
    if (context_pill_container != null)
    {
        function is_channel_tree_hidden()
        {
            let tree_pane = document.getElementById("space-devider1");
            return (tree_pane != null && getComputedStyle(tree_pane).display == "none");
        }

        function pill_press_target(event_target)
        {
            let pill = event_target.closest('.chat-context-selector[data-chat-context-selector-type="channel"]');
            return pill;
        }

        let pill_press_timer = null;
        let pill_long_press = false;

        context_pill_container.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }
            if (pill_press_target(event.target) == null || is_channel_tree_hidden() == false) { return; }

            pill_long_press = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            pill_press_timer = window.setTimeout(function() {
                pill_long_press = true;
                member_strip_show_channel_switch_menu(click_x, click_y);
            }, 600);
        }, { passive: true });

        context_pill_container.addEventListener("touchmove", function() {
            clearTimeout(pill_press_timer);
        }, { passive: true });

        context_pill_container.addEventListener("touchend", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            clearTimeout(pill_press_timer);

            // a long press must not ALSO fire the pill's own click
            if (pill_long_press == true)
            {
                event.preventDefault();
            }
        });

        context_pill_container.addEventListener("mousedown", function(event) {
            if (g_is_client_running_under_touch_device == true) { return; }

            if (event.which == 3 && pill_press_target(event.target) != null && is_channel_tree_hidden() == true)
            {
                event.stopPropagation(); // or document_onmousedown closes it in the same event
                member_strip_show_channel_switch_menu(event.clientX, event.clientY);
            }
        });
    }

    // long press on an own message = the delete/edit menu. desktop gets it via real
    // right-click (mousedown which==3 on the message <p>); phones never produce that,
    // so a 600ms hold synthesizes the exact same mousedown on the same element and the
    // existing per-message handler does the rest. delegated once - message elements
    // come and go with every chat render.
    let chat_context_container_element = document.getElementById("chat-context-container");
    if (chat_context_container_element != null)
    {
        let message_press_timer = null;
        let message_press_moved = false;

        chat_context_container_element.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            let message_p = event.target.closest(".local-single-chat-message-content-p, .local-client-chat-picture-img");
            if (message_p == null) { return; }

            message_press_moved = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            message_press_timer = window.setTimeout(function() {
                if (message_press_moved == true) { return; }
                // bubbles:false is load-bearing: the handler lives ON the message element
                // (target phase fires either way), while a bubbling mousedown would reach
                // document_onmousedown, whose menu cleanup deletes the just-created menu
                // within this very dispatch
                message_p.dispatchEvent(new MouseEvent("mousedown", {
                    bubbles: false,
                    cancelable: true,
                    button: 2,
                    clientX: click_x,
                    clientY: click_y
                }));
            }, 600);
        }, { passive: true });

        chat_context_container_element.addEventListener("touchmove", function() {
            // finger is scrolling the chat, not holding a message
            message_press_moved = true;
            clearTimeout(message_press_timer);
        }, { passive: true });

        chat_context_container_element.addEventListener("touchend", function() {
            clearTimeout(message_press_timer);
        });
    }

    // member-strip preferences, persisted like the theme. plain body classes /
    // css variables, so themes that ignore them (all but the strip themes, which also
    // unhides these controls) are completely unaffected:
    //  orientation button - top row vs side rail
    //  neighbors button   - hide everyone outside the local client's channel
    //  me button          - own actions menu (set/delete avatar)
    //  size slider        - continuous circle size via the --msgr-* variables
    let saved_strip_vertical = null;
    let saved_strip_scale = null;
    let saved_strip_neighbors = null;
    try
    {
        saved_strip_vertical = localStorage.getItem("lemon_strip_vertical");
        saved_strip_scale = localStorage.getItem("lemon_strip_scale");
        saved_strip_neighbors = localStorage.getItem("lemon_strip_neighbors");
    }
    catch (e) { console.warn("strip preference restore failed:", e.message); }

    if (saved_strip_vertical == "1") { document.body.classList.add("msgr-vertical"); }
    if (saved_strip_neighbors == "1") { document.body.classList.add("msgr-neighbors-only"); }

    // on unless it was turned off. the badge only appears for people with audio
    // off, so leaving it on costs nothing until there is something worth saying
    let saved_audio_availability = null;
    try { saved_audio_availability = localStorage.getItem("lemon_audio_availability"); }
    catch (e) { }

    if (saved_audio_availability != "0") { document.body.classList.add("msgr-audio-availability"); }

    function apply_strip_scale(circle_px)
    {
        document.body.style.setProperty("--msgr-circle", circle_px + "px");
        document.body.style.setProperty("--msgr-item", (circle_px + 16) + "px");
        document.body.style.setProperty("--msgr-strip", (circle_px + 52) + "px");
        document.body.style.setProperty("--msgr-name", (circle_px <= 46 ? 10 : (circle_px <= 56 ? 11 : 12)) + "px");
    }

    // legacy values were the steps "1"/"2"/"3"; anything bigger is pixels
    let restored_circle_px = 46;
    if (saved_strip_scale != null)
    {
        if (saved_strip_scale == "2") { restored_circle_px = 54; }
        else if (saved_strip_scale == "3") { restored_circle_px = 62; }
        else if (parseInt(saved_strip_scale) >= 38) { restored_circle_px = parseInt(saved_strip_scale); }
    }
    apply_strip_scale(restored_circle_px);

    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin",
        "<input type='button' id='msgr-orientation-button' title='people strip: switch between top row and side rail'>" +
        "<input type='button' id='msgr-neighbors-button' title='show only people in your channel'>" +
        "<input type='button' id='msgr-me-button' title='you: avatar and account actions'>" +
        "<input type='range' id='msgr-scale-slider' min='38' max='64' step='2' value='" + restored_circle_px + "' title='people size'>");

    document.getElementById("msgr-orientation-button").onclick = function()
    {
        let vertical_now = document.body.classList.toggle("msgr-vertical");
        try { localStorage.setItem("lemon_strip_vertical", vertical_now ? "1" : "0"); } catch (e) { console.warn("failed to persist strip orientation:", e.message); }
    };

    document.getElementById("msgr-neighbors-button").onclick = function()
    {
        let neighbors_now = document.body.classList.toggle("msgr-neighbors-only");
        try { localStorage.setItem("lemon_strip_neighbors", neighbors_now ? "1" : "0"); } catch (e) { console.warn("failed to persist neighbors filter:", e.message); }
    };

    // own actions (set avatar / delete avatar): the same menu a long press on
    // the own row would open - the row itself is hidden in the strip theme
    document.getElementById("msgr-me-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");
        if (local_row == null) { return; }

        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, window.innerWidth - 190, 48, local_row, false);
    };

    document.getElementById("msgr-scale-slider").oninput = function()
    {
        let circle_px = parseInt(this.value);
        apply_strip_scale(circle_px);
        try { localStorage.setItem("lemon_strip_scale", "" + circle_px); } catch (e) { console.warn("failed to persist strip scale:", e.message); }
    };

    // session info: its own icon in the bar, a small card with connection status,
    // ping (heartbeat round trip), traffic counters and session length. values
    // refresh once a second, but only while the card is open.
    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin",
        "<input type='button' id='msgr-session-button' title='connection and session info'>");

    document.body.insertAdjacentHTML("beforeend",
        "<div id='msgr-session-card'>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>status</span><span id='msgr-session-status'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>ping</span><span id='msgr-session-ping'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>sent</span><span id='msgr-session-sent'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>received</span><span id='msgr-session-received'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>session</span><span id='msgr-session-uptime'></span></p>" +
        "</div>");

    function format_byte_count(byte_count)
    {
        if (byte_count >= 1048576) { return (byte_count / 1048576).toFixed(1) + " MB"; }
        if (byte_count >= 1024) { return (byte_count / 1024).toFixed(1) + " KB"; }
        return byte_count + " B";
    }

    function refresh_session_card()
    {
        let now = new Date().valueOf();
        let is_connection_alive = (g_is_authenticated == true && (g_connection_check_message_response_received_timestamp + g_connection_check_lost_threshold_ms) >= now);

        document.getElementById("msgr-session-status").textContent = is_connection_alive ? "connected" : "not connected";
        document.getElementById("msgr-session-status").style.color = is_connection_alive ? "#3ddc84" : "#e05a4e";
        document.getElementById("msgr-session-ping").textContent = (is_connection_alive && g_session_last_ping_ms >= 0) ? (g_session_last_ping_ms + " ms") : "-";
        document.getElementById("msgr-session-sent").textContent = format_byte_count(g_session_bytes_sent);
        document.getElementById("msgr-session-received").textContent = format_byte_count(g_session_bytes_received);

        let uptime_text = "-";
        if (is_connection_alive == true && g_session_connected_at > 0)
        {
            let total_seconds = Math.floor((now - g_session_connected_at) / 1000);
            uptime_text = Math.floor(total_seconds / 3600) + "h " + Math.floor((total_seconds % 3600) / 60) + "m " + (total_seconds % 60) + "s";
        }
        document.getElementById("msgr-session-uptime").textContent = uptime_text;
    }

    let session_card_refresh_timer = null;

    document.getElementById("msgr-session-button").onclick = function()
    {
        let card = document.getElementById("msgr-session-card");
        let opening = (card.classList.contains("msgr-session-card-open") == false);
        card.classList.toggle("msgr-session-card-open", opening);

        clearInterval(session_card_refresh_timer);
        if (opening == true)
        {
            refresh_session_card();
            session_card_refresh_timer = setInterval(refresh_session_card, 1000);
        }
    };

    // tapping anywhere else closes the card - it is a glance, not a screen
    document.addEventListener("mousedown", function(event) {
        let session_card = document.getElementById("msgr-session-card");
        if (session_card == null || session_card.classList.contains("msgr-session-card-open") == false) { return; }
        if (event.target.closest("#msgr-session-card") != null || event.target.closest("#msgr-session-button") != null) { return; }

        session_card.classList.remove("msgr-session-card-open");
        clearInterval(session_card_refresh_timer);
    });

    // grid layout engine: desktop only. touch keeps the legacy inline-block layout,
    // so there g_layout_grid_active stays false and every layout_* entry point no-ops
    g_layout_panels = {
        channels: document.getElementById("space-devider1"),
        chat: document.getElementById("space-devider2"),
        info: document.getElementById("space-devider-42"),
        input: document.getElementById("space-devider4")
    };
    if (g_is_client_running_under_touch_device == false)
    {
        layout_load_saved_state();
        g_layout_grid_active = true;
        layout_apply();

        document.getElementById("layout-edit-button").onclick = layout_edit_toggle;
        document.getElementById("drag-resize-info").addEventListener("mousedown", function(e) {
            layout_column_drag_start(e, "info");
        }, false);
        document.getElementById("drag-resize-channels").addEventListener("mousedown", function(e) {
            layout_column_drag_start(e, "channels");
        }, false);

        // edit mode: capture-phase so a panel drag wins over every inner click handler
        document.getElementById("communication-system-container").addEventListener("mousedown", layout_edit_mousedown, true);
        document.addEventListener("mousemove", layout_edit_mousemove, false);
        document.addEventListener("mouseup", layout_edit_mouseup, false);
    }
    else
    {
        document.getElementById("layout-edit-button").style.display = "none";
        document.getElementById("drag-resize-info").style.display = "none";
        document.getElementById("drag-resize-channels").style.display = "none";
    }

    document.addEventListener("keydown", document_onkeydown);
    document.addEventListener("mousedown", document_onmousedown);
    document.addEventListener("keyup", document_onkeyup);

    document.getElementById("channel-properties-edit").addEventListener("mousedown", stop_propagation);
    document.getElementById("background-container").addEventListener("mousedown", stop_propagation);

    document.getElementById("chat-input-container-send-chat-input").onclick = UI.send_chat_input_on_click;

    // the pills shipped in the html (the root channel's) have no handlers until a rebind loop
    // happens to run; bind them now so they work on a fresh session too - opening an offline
    // chat used to leave the channel pill (and the strip's channel circle) dead
    for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
    {
        document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
    }
    for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
    {
        document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
    }
    document.getElementById("connect-button").onclick = UI.connect_button_onclick;
    wire_server_bookmarks();
    document.getElementById("chat-input-container-text-input").onkeyup = UI.chat_input_on_keyup;
    document.getElementById("choose_image_input").onchange = UI.choose_image_input;
    document.getElementById("choose-file-input").onchange = choose_chat_file_input_onchange;
    document.getElementById("image-upload-preview-remove").onclick = clear_pending_chat_picture;
    document.getElementById("server-settings-general-file-upload-max-size-input").oninput = refresh_file_upload_size_warning;
    document.getElementById("server-settings-general-allow-file-uploads-checkbox").onchange = refresh_file_upload_size_visibility;
    document.getElementById("server-settings-general-allow-pictures-checkbox").onchange = refresh_picture_size_visibility;
    document.getElementById("server-settings-general-country-blocking-checkbox").onchange = refresh_country_blocking_visibility;
    document.getElementById("server-settings-country-block-select").onchange = country_block_select_onchange;
    setup_chat_file_drag_and_drop();
    apply_file_upload_policy_to_ui();
    document.getElementById("add-key-button").onclick = UI.add_key_button_on_click;
    document.getElementById("show-hide-log-button").onclick = UI.show_hide_log_on_click;
    document.querySelector("[data-chat-context-remove-selector-id='channel-0']").onclick = UI.chat_context_remove_onclick;
    document.getElementById("close-button").onclick = UI.channel_properties_closebutton_onclick;
    document.getElementById("create-channel-button").onclick = UI.create_channel_button_onclick;
    document.getElementById("edit-channel-button").onclick = UI.edit_channel_button_onclick;
    document.getElementById("channel-properties-limit-clients-checkbox").onchange = UI.refresh_channel_limit_input_visibility;
    document.getElementById("close-button-enter-password").onclick = UI.enter_channel_password_closebutton_onclick;
    document.getElementById("close-button-admin-password").onclick = UI.close_admin_password_context_button;
    document.getElementById("close-button-secret-identity-string").onclick = UI.close_button_secret_identity_string_onclick;
    document.getElementById("close-button-identity-string-use").onclick = UI.close_button_identity_string_use;
    document.getElementById("join-passworded-channel-button").onclick = UI.channel_join_button_onclick;
    document.getElementById("admin-password-send-button").onclick = UI.admin_password_send_button_onclick;
    document.getElementById("clear-chat-button").onclick = UI.clear_chat_button_onclick;
    document.getElementById("hide-chat-button").onclick = UI.hide_chat_container;
    document.getElementById("show-admin-password-entry").onclick = UI.enter_admin_password_button_onclick;
    document.getElementById("choose-song-file-input").onchange = UI.choose_song_file_input_onchange;
    document.getElementById("choose_icon_input").onchange = UI.choose_icon_input_onchange;
    document.getElementById("drag-resize-chat").addEventListener('mousedown', UI.drag_resize_chat_onclick, false);
    document.getElementById("stop-song").onclick = UI.stop_song_onclick;
    document.getElementById("add-new-tag-to-server-button").onclick = UI.add_new_tag_to_server_button_onlick;
    document.getElementById("save-server-settings-button").onclick = UI.save_server_settings_button_onclick;
    document.getElementById("server-settings-log-save-button").onclick = UI.server_settings_log_save_button_onclick;
    document.getElementById("server-settings-log-refresh-button").onclick = UI.server_settings_log_refresh_button_onclick;
    document.getElementById("server-settings-log-clear-button").onclick = UI.server_settings_log_clear_button_onclick;
    document.getElementById("enter-server-settings").onclick = UI.enter_server_settings_onclick;
    document.getElementById("channel-flatten-toggle-button").onclick = UI.channel_flatten_toggle_onclick;
    document.getElementById("show-secret-identity-string").onclick = UI.show_secret_identity_string_onclick;
    document.getElementById("export-identity-file-button").onclick = UI.export_identity_file_button_onclick;
    document.getElementById("choose-identity-file-button").onclick = UI.choose_identity_file_button_onclick;
    document.getElementById("identity-file-input").onchange = UI.identity_file_input_onchange;
    document.getElementById("identity-file-confirm-button").onclick = UI.identity_file_confirm_button_onclick;
    document.getElementById("identity-mode-toggle-link").onclick = UI.identity_mode_toggle_onclick;
    // dropping a .lmn file on the identity dialog behaves like picking it; stopPropagation keeps
    // the drop away from the chat-file handlers
    {
        let identity_dialog = document.getElementById("identity-string-use-container-enter");
        identity_dialog.addEventListener("dragover", function(drag_event) { drag_event.preventDefault(); drag_event.stopPropagation(); });
        identity_dialog.addEventListener("drop", function(drop_event)
        {
            drop_event.preventDefault();
            drop_event.stopPropagation();

            if (document.getElementById("identity-file-mode").style.display == "none")
            {
                UI.identity_mode_toggle_onclick(drop_event);
            }

            if (drop_event.dataTransfer != null && drop_event.dataTransfer.files != null && drop_event.dataTransfer.files.length > 0)
            {
                UI.identity_read_identity_file(drop_event.dataTransfer.files[0]);
            }
        });
    }
    document.getElementById("import-identity-button").onclick = UI.import_identity_button_onclick;
    // top bar: same passphrase dialog; entered while connected it disconnects, regenerates
    // the keypair from the passphrase and reconnects as that identity (see the confirm handler)
    document.getElementById("switch-identity-button").onclick = UI.import_identity_button_onclick;
    document.getElementById("identity-string-use-confirm-button").onclick = UI.identity_string_use_confirm_button;
    document.getElementById("close-button-poke-client").onclick = UI.close_button_poke_client_onclick;
    document.getElementById("close-button-client-info").onclick = UI.close_button_client_info_onclick;
    document.getElementById("close-button-set-new-username").onclick = UI.close_button_set_new_username_onclick;
    document.getElementById("close-button-server-settings").onclick = UI.close_button_server_settings_onclick;
    document.getElementById("poke-client-send-button").onclick = UI.poke_client_send_button_onclick;
    document.getElementById("set-alias-send-button").onclick = UI.set_alias_send_button_onclick;
    document.getElementById("close-button-set-alias").onclick = UI.close_button_set_alias_onclick;

    // channel edit form: clicking the icon box opens the shared icon picker for that channel.
    // no admin gate here: the picker always opens, the server rejects the request if not allowed
    document.getElementById("channel-properties-icon-box").onclick = function()
    {
        if (g_channel_properties_edit_channel_id != null)
        {
            UI.open_channel_icon_picker(g_channel_properties_edit_channel_id);
        }
    };

    // "remove icon" next to the box: same set_channel_icon request with no icon_id, which the
    // server treats as clearing it; its broadcast repaints the tree row and the edit form box
    document.getElementById("channel-properties-icon-remove-button").onclick = function()
    {
        if (g_channel_properties_edit_channel_id != null)
        {
            UI.send_set_channel_icon_request(g_channel_properties_edit_channel_id, "none");
        }
    };

    // adjust-volume dialog: close button + live slider (updates the worklet lane gain as it moves)
    document.getElementById("close-button-adjust-volume").onclick = function()
    {
        document.getElementById("adjust-volume-container").style.display = "none";
    };
    document.getElementById("adjust-volume-slider").oninput = function()
    {
        let target_client_id = this.getAttribute("data-target-client-id");
        let gain = parseInt(this.value) / 100;

        document.getElementById("adjust-volume-value-label").innerHTML = this.value + "%";

        if (target_client_id != null)
        {
            g_client_volume_by_id[target_client_id] = gain;
            set_client_playback_volume(parseInt(target_client_id), gain);
        }
    };
    document.getElementById("set-new-username-send-button").onclick = UI.set_new_username_send_button_onclick;
    document.getElementById("chat-input-file-input-container").onmousedown = UI.chat_input_container_on_mousedown;
    document.getElementById("color-picker-input").addEventListener("input", UI.color_picker_oninput);
    document.getElementById("chat-input-font-size-range").addEventListener("input", UI.chat_input_font_size_range_oninput);
    document.getElementById("hide-show-flags-button").onclick = UI.hide_show_flags_button_onclick;
    document.getElementById("microphone-always-broadcasting-audio-button").onclick = UI.activate_continous_audio_broadcast_onclick;
    document.getElementById("sound-effects-button").onclick = UI.sound_effects_button_onclick;

    // local settings: device-only preferences, no admin involved. new messages only -
    // already-rendered ones keep whatever they had
    document.getElementById("local-settings-button").onclick = function()
    {
        let panel = document.getElementById("local-settings-container");
        document.getElementById("local-settings-show-avatars").checked = g_show_message_avatars;
        document.getElementById("local-settings-rsa-key-bits").value = String(g_rsa_key_bits);
        document.getElementById("local-settings-seen-indicator").checked = g_show_seen_indicator;
        document.getElementById("local-settings-send-seen").checked = g_send_seen_receipts;
        document.getElementById("local-settings-hide-mic").checked = g_hide_microphone_button;
        document.getElementById("local-settings-sound-effects").checked = g_are_sound_effects_enabled;
        document.getElementById("local-settings-country-flags").checked = (g_show_hide_toggle == false);
        render_mic_mode_controls();
        document.getElementById("local-settings-file-logging").checked = g_is_file_logging_enabled;
        document.getElementById("local-settings-show-log").checked =
            (document.getElementById("show-hide-log-button").style.display !== "none");
        document.getElementById("local-settings-strip-vertical").checked = document.body.classList.contains("msgr-vertical");
        document.getElementById("local-settings-strip-neighbors").checked = document.body.classList.contains("msgr-neighbors-only");
        document.getElementById("local-settings-audio-availability").checked = document.body.classList.contains("msgr-audio-availability");
        populate_microphone_device_list();

        if (panel.style.display === "none")
        {
            panel.classList.remove("closing");

            // clear it rather than force "block": an inline display beats the stylesheet,
            // and the phone themes need the panel to stay the flex column that lets the
            // row list scroll under a fixed title bar and save button
            panel.style.display = "";
        }
        else
        {
            close_local_settings();
        }
    };

    // fade OUT as well as in - display:none mid-animation just made it vanish.
    // a declaration, not a let: it is referenced above this line too, and hoisting
    // keeps that working if this block is ever reordered
    function close_local_settings()
    {
        let panel = document.getElementById("local-settings-container");
        panel.classList.add("closing");

        window.setTimeout(function()
        {
            panel.style.display = "none";
            panel.classList.remove("closing");
        }, 200);
    }

    // the select and the touch segments are two faces of g_is_continuous_mic_mode; a theme
    // shows one of them, and this paints whichever is on screen from that one variable
    function render_mic_mode_controls()
    {
        document.getElementById("local-settings-mic-mode").value = g_is_continuous_mic_mode ? "continuous" : "push-to-talk";
        document.getElementById("local-settings-ptt-key").value = g_push_to_talk_key_label;

        // the key only means anything in push-to-talk mode
        document.getElementById("local-settings-ptt-key-row").style.display = g_is_continuous_mic_mode ? "none" : "";

        let segments = document.querySelectorAll("#local-settings-mic-mode-segmented .local-settings-segment");

        for (let x = 0; x < segments.length; x++)
        {
            let is_selected = (segments[x].getAttribute("data-mic-mode") === "continuous") === g_is_continuous_mic_mode;
            segments[x].classList.toggle("segment-selected", is_selected);
        }
    }

    // fills the microphone picker with the audio inputs the browser reports. the row always
    // shows; before the microphone has been used once the browser hides the real device list
    // behind the permission, so a disabled hint entry explains why there is nothing to pick yet
    function populate_microphone_device_list()
    {
        let row = document.getElementById("local-settings-mic-device-row");

        if (row == null || navigator.mediaDevices == null || typeof navigator.mediaDevices.enumerateDevices != "function")
        {
            return;
        }

        navigator.mediaDevices.enumerateDevices().then(function(devices)
        {
            // a device with an empty id is the pre-permission placeholder, not a real choice
            let inputs = devices.filter(function(device) { return device.kind === "audioinput" && device.deviceId !== ""; });

            let select = document.getElementById("local-settings-mic-device");
            let html = "<option value=\"\">system default</option>";

            if (inputs.length == 0)
            {
                html += "<option value=\"\" disabled>(use the microphone once to list devices)</option>";
            }

            for (let i = 0; i < inputs.length; i++)
            {
                let label = (typeof inputs[i].label === "string" && inputs[i].label.length > 0) ? inputs[i].label : ("microphone " + (i + 1));
                html += "<option value=\"" + sanitize_string(inputs[i].deviceId) + "\">" + sanitize_string(label) + "</option>";
            }

            select.innerHTML = html;
            select.value = g_selected_microphone_device_id;

            // a remembered device that no longer exists falls back to the default entry
            if (select.value !== g_selected_microphone_device_id)
            {
                select.value = "";
            }

            row.style.display = "";
        }).catch(function() { row.style.display = "none"; });
    }

    // switches the live capture to the chosen device without touching the capture graph: a new
    // source joins the same gain node and the old track just stops. nothing is ever disconnected,
    // because a capture-edge disconnect once stalled chromium's whole mic delivery
    function apply_selected_microphone_device()
    {
        if (g_local_audio_stream == null || navigator.mediaDevices == null)
        {
            return; // the mic is not built yet; the choice applies when it activates
        }

        let switch_constraints = (g_selected_microphone_device_id != "")
            ? { audio: { deviceId: { ideal: g_selected_microphone_device_id } } }
            : { audio: true };

        navigator.mediaDevices.getUserMedia(switch_constraints).then(function(new_stream)
        {
            let old_track = g_local_audio_stream.getAudioTracks()[0];
            let new_track = new_stream.getAudioTracks()[0];

            // the new track inherits the transmit state, so a switch mid-talk keeps talking
            new_track.enabled = (old_track != null) ? old_track.enabled : false;

            let new_source = audio_context.createMediaStreamSource(new_stream);
            new_source.connect(g_audio_recorder_gain_node);

            if (old_track != null)
            {
                old_track.stop();
            }

            g_local_audio_stream = new_stream;
            g_audio_input = new_source;

            custom_log("microphone switched to: " + (new_track.label || "default device"));
        }).catch(function(switch_error)
        {
            custom_alert("could not switch the microphone: " + switch_error);
        });
    }

    function apply_mic_mode(is_continuous)
    {
        g_is_continuous_mic_mode = is_continuous;
        try { localStorage.setItem("lemon_continuous_mic", g_is_continuous_mic_mode ? "1" : "0"); } catch (e) { }

        // switching modes mid-transmission: stop cleanly, the button state must not lie
        if (g_is_continuous_mic_mode == false && g_is_continuous_transmission_active == true)
        {
            g_is_continuous_transmission_active = false;
            document.getElementById("microphone-push-to-talk-button-touch-device").classList.remove("mic-continuous-active");
            process_stop_sending_audio();
        }

        render_mic_mode_controls();
    }

    // every setting applies live, so this is a "save" only in the sense of "done"
    document.getElementById("local-settings-close-button").onclick = close_local_settings;
    document.getElementById("local-settings-x-button").onclick = close_local_settings;

    // the people-strip preferences moved here from the toolbar; the toolbar buttons
    // still exist for the strip themes, so both drive the same body classes
    document.getElementById("local-settings-strip-vertical").onchange = function()
    {
        document.body.classList.toggle("msgr-vertical", this.checked);
        try { localStorage.setItem("lemon_strip_vertical", this.checked ? "1" : "0"); } catch (e) { }
    };

    document.getElementById("local-settings-strip-neighbors").onchange = function()
    {
        document.body.classList.toggle("msgr-neighbors-only", this.checked);
        try { localStorage.setItem("lemon_strip_neighbors", this.checked ? "1" : "0"); } catch (e) { }
    };

    document.getElementById("local-settings-audio-availability").onchange = function()
    {
        document.body.classList.toggle("msgr-audio-availability", this.checked);
        try { localStorage.setItem("lemon_audio_availability", this.checked ? "1" : "0"); } catch (e) { }
    };

    // avatar / account actions: opens the same menu the msgr "me" button opens
    document.getElementById("local-settings-avatar-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");

        if (local_row == null)
        {
            custom_alert("connect first to change your avatar");
            return;
        }

        close_local_settings();

        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, window.innerWidth - 190, 48, local_row, false);
    };

    document.getElementById("local-settings-show-log").onchange = function()
    {
        let log_button = document.getElementById("show-hide-log-button");
        log_button.style.display = this.checked ? "" : "none";

        if (this.checked == false)
        {
            document.getElementById("textarea-log").style.display = "none";
        }

        try { localStorage.setItem("lemon_show_log_button", this.checked ? "1" : "0"); } catch (e) { }
    };

    document.getElementById("local-settings-show-avatars").onchange = function()
    {
        g_show_message_avatars = this.checked;
        try { localStorage.setItem("lemon_show_message_avatars", this.checked ? "1" : "0"); } catch (e) { }
    };

    // changing the key size changes the identity itself, so it only takes effect on the next
    // keypair: the current connection keeps the key it already has
    document.getElementById("local-settings-rsa-key-bits").onchange = function()
    {
        let chosen_bits = parseInt(this.value);

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(chosen_bits) < 0) { return; }

        g_rsa_key_bits = chosen_bits;
        try { localStorage.setItem("lemon_rsa_key_bits", String(chosen_bits)); } catch (e) { }

        // a size the user picked by hand replaces whatever a server last asked for
        g_rsa_key_too_weak_prompted_for_bits = 0;

        custom_alert("identity key size set to " + chosen_bits + " bits. it applies to the next identity you create - use the identity button to switch now");
    };

    document.getElementById("rsa-key-too-weak-no-button").onclick = hide_rsa_key_too_weak_dialog;
    document.getElementById("close-button-rsa-key-too-weak").onclick = hide_rsa_key_too_weak_dialog;

    document.getElementById("rsa-key-too-weak-yes-button").onclick = function()
    {
        let target_bits = parseInt(this.getAttribute("data-target-bits"));

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(target_bits) < 0) { return; }

        g_rsa_key_bits = target_bits;
        try { localStorage.setItem("lemon_rsa_key_bits", String(target_bits)); } catch (e) { }

        hide_rsa_key_too_weak_dialog();
        custom_alert("creating a " + target_bits + "-bit identity key, this can take a while ...");

        // keep the same passphrase where there is one: at the new size it derives a different
        // keypair, but it stays reproducible from what the user already saved
        request_identity((typeof identity_string === "string" && identity_string.length >= 199) ? identity_string : null);
        request_connect("button");
    };
    document.getElementById("local-settings-seen-indicator").onchange = function()
    {
        g_show_seen_indicator = this.checked;
        try { localStorage.setItem("lemon_seen_indicator", this.checked ? "1" : "0"); } catch (e) { }
        render_seen_indicator();
    };

    document.getElementById("local-settings-send-seen").onchange = function()
    {
        g_send_seen_receipts = this.checked;
        try { localStorage.setItem("lemon_send_seen", this.checked ? "1" : "0"); } catch (e) { }
    };
    document.getElementById("local-settings-hide-mic").onchange = function()
    {
        g_hide_microphone_button = this.checked;
        try { localStorage.setItem("lemon_hide_mic", this.checked ? "1" : "0"); } catch (e) { }
        update_microphone_button();
    };

    document.getElementById("local-settings-file-logging").onchange = function()
    {
        g_is_file_logging_enabled = this.checked;

        // java saves it and turns the logging on or off on both sides
        if (typeof Android !== "undefined" && typeof Android.JavaExportSetFileLogging === "function")
        {
            Android.JavaExportSetFileLogging(this.checked);
        }
    };

    document.getElementById("local-settings-sound-effects").onchange = function()
    {
        // route through the one toggle that also syncs the mute gate and the android side
        if (this.checked != g_are_sound_effects_enabled)
        {
            UI.sound_effects_button_onclick();
        }
        try { localStorage.setItem("lemon_sound_effects", this.checked ? "1" : "0"); } catch (e) { }
    };

    document.getElementById("local-settings-country-flags").onchange = function()
    {
        UI.hide_show_flags_button_onclick();
        try { localStorage.setItem("lemon_show_flags", this.checked ? "1" : "0"); } catch (e) { }
    };

    document.getElementById("local-settings-mic-mode").onchange = function()
    {
        apply_mic_mode(this.value === "continuous");
    };

    document.getElementById("local-settings-mic-device").onchange = function()
    {
        g_selected_microphone_device_id = this.value;
        try { localStorage.setItem("lemon_mic_device_id", g_selected_microphone_device_id); } catch (e) { }
        apply_selected_microphone_device();
    };

    {
        let segments = document.querySelectorAll("#local-settings-mic-mode-segmented .local-settings-segment");

        for (let x = 0; x < segments.length; x++)
        {
            segments[x].onclick = function()
            {
                apply_mic_mode(this.getAttribute("data-mic-mode") === "continuous");
            };
        }
    }

    // press the button, then press the key you want. escape cancels
    document.getElementById("local-settings-ptt-key").onclick = function()
    {
        let key_button = this;
        key_button.value = "press a key…";

        let capture_key = function(event)
        {
            event.preventDefault();
            event.stopPropagation();
            window.removeEventListener("keydown", capture_key, true);

            if (event.key !== "Escape")
            {
                g_push_to_talk_key_code = event.which || event.keyCode;
                g_push_to_talk_key_label = (event.key.length === 1) ? event.key.toUpperCase() : event.key;

                try
                {
                    localStorage.setItem("lemon_ptt_key_code", g_push_to_talk_key_code);
                    localStorage.setItem("lemon_ptt_key_label", g_push_to_talk_key_label);
                }
                catch (e) { }
            }

            key_button.value = g_push_to_talk_key_label;
        };

        window.addEventListener("keydown", capture_key, true);
    };

    // these toggles live in local settings now; hiding the toolbar copies frees icon space
    document.getElementById("sound-effects-button").style.display = "none";
    document.getElementById("hide-show-flags-button").style.display = "none";
    document.getElementById("microphone-always-broadcasting-audio-button").style.display = "none";

    // restore the persisted sound preference; runs after the var initializers, so it wins
    try
    {
        let stored_sound = localStorage.getItem("lemon_sound_effects");

        if (stored_sound != null)
        {
            g_are_sound_effects_enabled = (stored_sound === "1");
            apply_sound_effects_muted();
        }
    }
    catch (e) { }
    sync_toolbar_state_classes(); // seed the state classes from the initial bools so oldschool shows the right g_icons on load
    document.getElementById("musicbot-management-close-button").onclick = UI.musicbot_management_close_button_onclick;
    document.getElementById("musicbot-management-background-container-upload-button-confirm").onclick = UI.musicbot_management_confirm_upload_button_onclick;

    document.querySelector("a-toast").onclick = UI.a_toast_onclick;
    document.addEventListener("contextmenu", e =>
    {
        e.preventDefault();
    });

    if (g_is_client_running_under_touch_device)
    {
        document.getElementById("microphone-push-to-talk-button-touch-device").addEventListener("touchstart", UI.toggle_microphone_onclick);
        document.getElementById("microphone-push-to-talk-button-touch-device").addEventListener("touchend", UI.toggle_microphone_onclick);
    }
    else
    {
        document.getElementById("toggle-microphone").addEventListener("change", UI.toggle_microphone_onclick);
    }

    for (let i = 0; i < document.getElementsByClassName("server-settings-tab-li").length; i++)
    {
        document.getElementsByClassName("server-settings-tab-li")[i].onclick = UI.server_settings_tab_li_onclick;
    }

    // identity list: delete buttons are rendered dynamically, so delegate their clicks. a
    // delete sends the identity hash; the server clears it and strips the holder's g_tags
    // send an add/remove of a single tag on a stored identity (works offline)
    let send_modify_identity_tag = function(identity_hash, tag_id, add)
    {
        if (identity_hash == null || identity_hash.length == 0) { return; }
        let message_object = { message: { type: "modify_identity_tag", public_key_hash: identity_hash, tag_id: parseInt(tag_id), add: add } };
        send_message_object(message_object);
    };

    // registers (or with an empty alias, unregisters) a STORED identity by hash, so it works
    // whether or not its owner is connected. an empty result is normal: the server refuses
    // silently when the alias is already taken by somebody else
    let send_set_identity_alias = function(identity_hash, alias)
    {
        if (identity_hash == null || identity_hash.length == 0) { return; }

        let message_object = { message: { type: "set_identity_alias_request", public_key_hash: identity_hash, alias: ("" + alias).trim() } };
        send_message_object(message_object);

        // the list is a server-rendered snapshot; ask for a fresh one so the alias and the
        // registered column reflect what the server actually accepted
        setTimeout(function()
        {
            let refresh_object = { message: { type: "request_identity_list" } };
            let refresh_json = process_message_before_sending(refresh_object);
            websocket_worker_send(encrypt_all_message_data_and_convert_to_base64(refresh_json));
        }, 250);
    };

    document.getElementById("server-settings-identities-list").addEventListener("click", function(event)
    {
        // remove a single tag from an identity (the chip's ✕)
        let tag_remove = event.target.closest(".identity-tag-remove");
        if (tag_remove != null)
        {
            send_modify_identity_tag(tag_remove.getAttribute("data-identity-hash"), tag_remove.getAttribute("data-tag-id"), false);
            return;
        }

        // save the alias typed into this row (registers the identity)
        let alias_set_button = event.target.closest(".identity-alias-set-button");
        if (alias_set_button != null)
        {
            let alias_input = document.querySelector('.identity-alias-input[data-identity-hash="' + alias_set_button.getAttribute("data-identity-hash") + '"]');
            send_set_identity_alias(alias_set_button.getAttribute("data-identity-hash"), (alias_input != null) ? alias_input.value : "");
            return;
        }

        // clear the alias (unregisters the identity)
        let alias_clear_button = event.target.closest(".identity-alias-clear-button");
        if (alias_clear_button != null)
        {
            send_set_identity_alias(alias_clear_button.getAttribute("data-identity-hash"), "");
            return;
        }

        // delete the whole identity (the row's ✕)
        let button = event.target.closest(".identity-delete-button");
        if (button == null) { return; }

        let identity_hash = button.getAttribute("data-identity-hash");
        if (identity_hash == null || identity_hash.length == 0) { return; }

        let message_object = { message: { type: "delete_identity", public_key_hash: identity_hash } };
        send_message_object(message_object);
    });

    // enter in the alias field saves it, same as pressing the ✓
    document.getElementById("server-settings-identities-list").addEventListener("keyup", function(event)
    {
        if (event.key != "Enter") { return; }

        let alias_input = event.target.closest(".identity-alias-input");
        if (alias_input == null) { return; }

        send_set_identity_alias(alias_input.getAttribute("data-identity-hash"), alias_input.value);
    });

    // give an identity a tag (the "+ tag" dropdown)
    document.getElementById("server-settings-identities-list").addEventListener("change", function(event)
    {
        let select = event.target.closest(".identity-tag-add");
        if (select == null || select.value === "") { return; }
        send_modify_identity_tag(select.getAttribute("data-identity-hash"), select.value, true);
        select.value = "";
    });

    // make the server settings window draggable by its left nav column (tab clicks still work,
    // since a click without movement does not reposition the window)
    (function() {
        let settings_window = document.getElementById("server-settings-tab");
        let drag_handle = document.getElementById("server-settings-tab-subcontainer");
        let is_dragging = false;
        let drag_offset_x = 0;
        let drag_offset_y = 0;

        if (settings_window != null && drag_handle != null)
        {
            drag_handle.addEventListener("mousedown", function(e) {
                let rect = settings_window.getBoundingClientRect();
                is_dragging = true;
                drag_offset_x = e.clientX - rect.left;
                drag_offset_y = e.clientY - rect.top;
                e.preventDefault();
            });

            document.addEventListener("mousemove", function(e) {
                if (is_dragging == false)
                {
                    return;
                }
                settings_window.style.left = (e.clientX - drag_offset_x) + "px";
                settings_window.style.top = (e.clientY - drag_offset_y) + "px";
            });

            document.addEventListener("mouseup", function() {
                is_dragging = false;
            });
        }
    })();

    // make every popup draggable by its title bar (#menu-bar-container), like a real window.
    // dragging pins the dialog with inline position:fixed + left/top; those pins are cleared
    // again every time the dialog is re-opened, so popups always START centered (the css
    // margin:auto centering). without the reset a pin lives forever - worst case 0,0 from a
    // mousedown that landed while the dialog had no layout (hidden = zero rect), which made
    // every popup open in the top-left corner from then on
    (function() {
        let dragging = null, ox = 0, oy = 0;
        let bars = document.querySelectorAll('[id="menu-bar-container"]');

        let clear_drag_pin = function(dialog) {
            dialog.style.position = "";
            dialog.style.margin = "";
            dialog.style.left = "";
            dialog.style.top = "";
        };

        // recenter on every open: watch each dialog's container for its display flipping on
        let recenter_observer = new MutationObserver(function(mutations) {
            for (let i = 0; i < mutations.length; i++) {
                let container = mutations[i].target;
                if (container.style.display == "none" || container.style.display == "") { continue; }
                let dialog = container.querySelector('[data-recenter-on-open="true"]');
                if (dialog != null)
                {
                    // center by js instead of trusting theme css: some themes (oldschool)
                    // give the overlay container zero size, which parked dialogs top-left
                    clear_drag_pin(dialog);
                    dialog.style.position = "fixed";
                    dialog.style.margin = "0";
                    // measure with offsetWidth/Height (layout size) not getBoundingClientRect:
                    // a theme's pop-in scale animation (e.g. termix) makes the client rect
                    // smaller than the real box and pushed the centering off. center on both axes.
                    let dw = dialog.offsetWidth;
                    let dh = dialog.offsetHeight;
                    dialog.style.left = Math.max(0, Math.round((window.innerWidth - dw) / 2)) + "px";
                    dialog.style.top = Math.max(0, Math.round((window.innerHeight - dh) / 2)) + "px";
                }
            }
        });

        for (let i = 0; i < bars.length; i++)
        {
            let dialog = bars[i].parentElement;
            dialog.setAttribute("data-recenter-on-open", "true");
            if (dialog.parentElement != null)
            {
                recenter_observer.observe(dialog.parentElement, { attributes: true, attributeFilter: ["style"] });
            }

            bars[i].addEventListener("mousedown", function(e) {
                if (e.target.closest(".close-button") != null) { return; }
                let dialog = this.parentElement;
                let rect = dialog.getBoundingClientRect();
                if (rect.width == 0 && rect.height == 0) { return; } // no layout (hidden): a pin here would stick the dialog to 0,0
                dragging = dialog;
                ox = e.clientX - rect.left;
                oy = e.clientY - rect.top;
                dialog.style.position = "fixed";
                dialog.style.margin = "0";
                dialog.style.left = rect.left + "px";
                dialog.style.top = rect.top + "px";
                e.preventDefault();
            });
        }
        document.addEventListener("mousemove", function(e) {
            if (dragging == null) { return; }
            dragging.style.left = (e.clientX - ox) + "px";
            dragging.style.top = (e.clientY - oy) + "px";
        });
        document.addEventListener("mouseup", function() { dragging = null; });
    })();

    for (let i = 0; i < document.getElementsByClassName("choose-theme-item").length; i++)
    {
        document.getElementsByClassName("choose-theme-item")[i].onclick = UI.choose_theme_item_onclick;
    }

    // on a touch device, hide every theme not tagged mobile-capable, so a phone can only pick
    // themes that are actually laid out for it (currently just the "mobile" theme)
    if (g_is_client_running_under_touch_device)
    {
        let theme_items = document.getElementsByClassName("choose-theme-item");
        for (let i = 0; i < theme_items.length; i++)
        {
            if (theme_items[i].getAttribute("data-mobile") !== "true")
            {
                theme_items[i].style.display = "none";
            }
        }
    }

    // browsers create an AudioContext suspended unless it is made during a user gesture; resume it on the first click/tap so audio works after an autoconnect, with no dedicated connect click.
    // the context that matters is audio_context (created at authentication time, all voice/music playback runs through it).
    // the listeners must stay attached until audio_context exists and is running: a gesture can fire before authentication creates it
    let unlock_audio_on_first_user_gesture = function()
    {
        if (typeof audio_context !== "undefined" && audio_context != null && audio_context.state === "suspended")
        {
            audio_context.resume();
        }

        if (typeof audio_context !== "undefined" && audio_context != null && audio_context.state === "running")
        {
            document.removeEventListener("click", unlock_audio_on_first_user_gesture, true);
            document.removeEventListener("touchend", unlock_audio_on_first_user_gesture, true);
            document.removeEventListener("keydown", unlock_audio_on_first_user_gesture, true);
        }
    };
    document.addEventListener("click", unlock_audio_on_first_user_gesture, true);
    document.addEventListener("touchend", unlock_audio_on_first_user_gesture, true);
    document.addEventListener("keydown", unlock_audio_on_first_user_gesture, true);

    g_data_processing_worker = create_new_webworker_in_same_file("data_processing_worker");
    g_websocket_worker = create_new_webworker_in_same_file("websocket_worker");
    g_opus_encoder_worker = create_new_webworker_in_same_file("opus_encoder_worker");
    g_opus_decoder_worker = create_new_webworker_in_same_file("opus_decoder_worker");
    g_minimp3_worker = create_new_webworker_in_same_file("minimp3_worker");

    // restore a persisted identity so the same keypair is reused across launches (like the
    // saved theme). the "identity string" is the 200-char passphrase that
    // lemon_crypto.generateRSAKey() deterministically turns back into the keypair, so persisting
    // it means reconstructing rather than generating a fresh random identity on every launch.
    // SECURITY: this passphrase is private-key-equivalent - anything that can read this origin's
    // localStorage (another script on the page, an XSS bug, someone at the machine) can lift the
    // whole identity. acceptable for a self-hosted / personal deployment; not for shared machines.
    // server opt-in: only restore/persist when window.__SERVER_CONFIG__.persist_identity is true
    // (default off). off keeps every localhost window on its own fresh random identity, so you can
    // open several windows and connect as distinct clients.
    let persist_identity_enabled = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true);

    // avatars: server opt-in (default off) + the accepted max upload size (default 50 KB)
    g_avatars_allowed = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.allow_avatars === true);
    if (g_avatars_allowed == true && typeof window.__SERVER_CONFIG__.avatar_max_size === "number" && window.__SERVER_CONFIG__.avatar_max_size > 0)
    {
        g_avatar_max_upload_bytes = window.__SERVER_CONFIG__.avatar_max_size;
    }

    let persisted_identity = null;
    if (persist_identity_enabled == true)
    {
        try { persisted_identity = localStorage.getItem("lemon_identity_string"); }
        catch (e) { console.warn("failed to read persisted identity:", e.message); }
    }

    // only trust a stored value of the expected length; a short/corrupt one would silently seed
    // a DIFFERENT keypair, so fall back to a fresh random identity in that case
    let use_persisted_identity = (persisted_identity != null && persisted_identity.length >= 199);

    // the android webview talks to node, which owns the identity - no keypair needed there
    if (typeof Android === "undefined")
    {
        request_identity(use_persisted_identity ? persisted_identity : null);
    }

    g_minimp3_worker.postMessage({
        type: "init"
    });

    // set css settings, set default theme
    // chat will support few themes at most, its up to people to modify their client.html to use own theme

    for (let i = 0; i < document.getElementsByClassName("style-theme-html-element").length; i++)
    {
        document.getElementsByClassName("style-theme-html-element")[i].setAttribute("media","max-width: 1px");
    }

    if (g_is_client_running_under_touch_device)
    {
        document.getElementById("style-theme-default-mobile").removeAttribute("media"); // set default theme
    }
    else
    {
        // document.getElementById("style-theme-default-mobile").removeAttribute("media"); //set default theme
        document.getElementById("style-theme-default").removeAttribute("media"); // set default theme
    }

    // server-baked default theme (from the injected config); applied unless the user has their own saved choice. persist=false so it never sticks.
    // on a phone a desktop theme is ignored here so the mobile default set above stays in effect
    let startup_theme_applied = false;

    if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.theme && UI.is_theme_allowed_on_this_device(window.__SERVER_CONFIG__.theme))
    {
        UI.apply_theme(window.__SERVER_CONFIG__.theme, false);
        startup_theme_applied = true;
    }

    // restore a theme the user previously picked (from the in-app menu); wins over the server default.
    // a saved desktop theme is ignored on a phone (keeps the mobile default)
    try
    {
        let saved_theme = localStorage.getItem("lemon_theme");
        if (saved_theme != null && UI.is_theme_allowed_on_this_device(saved_theme))
        {
            UI.apply_theme(saved_theme, false);
            startup_theme_applied = true;
        }
    }
    catch (e) { console.warn("failed to restore saved theme:", e.message); }

    // no server-baked theme and nothing saved: activate the default sheet explicitly.
    // without this ALL four theme <style> elements stay active at once and the cascade
    // mixes them (the last sheet mostly wins), which is never an intended look
    if (startup_theme_applied == false)
    {
        UI.apply_theme(g_is_client_running_under_touch_device ? "default-mobile" : "default", false);
    }

    // the wrapper app's mode drives the theme (both directions). runs after the restore
    // above so it wins, and is re-run whenever android pushes a mode change
    apply_theme_for_app_mode();

    // restore the flat-channel-list preference (it only has a visible effect under a theme that
    // styles the .channels-flattened class, i.e. termix, but the class is applied regardless so
    // switching back to termix keeps the choice)
    try { g_is_channel_list_flattened = (localStorage.getItem("lemon_channels_flat") == "1"); } catch (e) { console.warn("failed to restore channel-flatten preference:", e.message); }
    UI.apply_channel_flatten_state();

    if (g_is_running_in_android_webview)
    {
        g_send_go_to_idle_mode_request = android_js_bridge.send_go_to_idle_mode_request_android;
        g_send_come_from_idle_mode_request = android_js_bridge.send_come_from_idle_mode_request_android;
        g_mark_call_accept_presence = android_js_bridge.mark_call_accept_presence_android;
        g_set_username_on_connect = android_js_bridge.set_username_on_connect_android;
        g_accept_current_settings_from_android = android_js_bridge.accept_current_settings_from_android;
        g_nudge_loopback_reattach = android_js_bridge.nudge_loopback_reattach_android;
        g_show_connection_phase = android_js_bridge.show_connection_phase_android;

        // a push that raced ahead of this wiring was held by the page shim - use it now
        if (typeof g_pending_android_settings === "string" && g_pending_android_settings != null)
        {
            console.log("connect-path: applying settings that arrived before onload");
            g_accept_current_settings_from_android(g_pending_android_settings);
            g_pending_android_settings = null;
        }

        // lets a theme style itself for the wrapper app (the strip themes give the settings gear a
        // real slot in its top bar; css alone cannot tell app from browser)
        document.body.classList.add("android-app");

        // the app loads client.html from its assets, never from the chat-server's http
        // server, so the serve-time config (and with it allow_avatars) never arrives and
        // the set/delete-avatar menu entries silently vanished. assume avatars here: a
        // server that has them off just ignores the upload, nothing breaks
        g_avatars_allowed = true;

        document.getElementById("import-identity-button").style.display = "none";

        // the bar gear stays hidden on the connect screen (the in-flow button below
        // owns that state) and appears once connected. tapped while connected it warns
        // first: the user is about to edit the details of the connection he is on
        document.getElementById("android-settings-button").onclick = function() {
            if (g_is_authenticated)
            {
                document.getElementById("android-settings-warning-container").style.display = "block";
                return;
            }
            Android.JavaExportGoToSettings();
        }

        document.getElementById("android-settings-warning-continue-button").onclick = function() {
            document.getElementById("android-settings-warning-container").style.display = "none";
            Android.JavaExportGoToSettings();
        }

        let close_settings_warning = function() {
            document.getElementById("android-settings-warning-container").style.display = "none";
        }
        document.getElementById("android-settings-warning-cancel-button").onclick = close_settings_warning;
        document.getElementById("close-button-android-settings-warning").onclick = close_settings_warning;

        // in-flow twin of the top-bar gear: on the not-connected screen the user
        // should not have to hunt a corner icon to find where the address lives
        document.getElementById("android-server-settings-container").style.display = "";
        document.getElementById("android-server-settings-button").onclick = function() {
            Android.JavaExportGoToSettings();
        }

        Android.JavaExportRequestCurrentSettingsFromAndroid();

        // the settings callback sets the connection target; the driver takes it from there
    }
    else if (g_are_server_details_predefined == true)
    {
        // website with baked-in config: request_connect dials it under autoconnect
        request_connect("settings");
    }

    // the page loads with the spinner up (see the template); arm its reveal deadline so a
    // client that connects to nothing still shows the connect page after a moment.
    // the setter call paints the theme's background onto it (opaque page one)
    set_connect_holdback_loader_visible(true);
    g_connect_holdback_started = new Date().valueOf();
    g_connect_holdback_deadline = g_connect_holdback_started + 2500;
    setTimeout(connect_holdback_check, 250);

    // a browser client in manual mode knows at load that nothing will dial: no spinner
    if (g_is_running_in_android_webview == false && g_is_autoconnect_without_user_action_active == false)
    {
        reveal_connect_page();
    }

    start_connection_status_ticker();
    connection_driver();
}

// needs to be checked so webworkers that this code is shared with do not try to use window object

if (typeof window !== 'undefined')
{
    window.onload = window_onload;
}


// ---------------------------------------------------------------------------
// country blocking, the admin's join block list in the server settings tab
// the selectable countries are harvested from the flag stylesheet the client already ships,
// so no second country list exists that could drift out of sync; display names come from the
// browser (Intl), falling back to the bare code. nothing here runs at load time
// ---------------------------------------------------------------------------

// the admin's unsaved block list, replaced whenever server_settings_values arrives
var g_blocked_countries_draft = [];

// stylesheet harvest and the Intl helper, both resolved once on first use
var g_country_code_cache = null;
var g_country_display_names = null;
var g_is_country_select_populated = false;

// english name for an iso code, or the code itself when the browser cannot name it
function get_country_display_name(code)
{
    if (g_country_display_names === null)
    {
        try
        {
            g_country_display_names = new Intl.DisplayNames(["en"], { type: "region" });
        }
        catch (intl_error)
        {
            g_country_display_names = false;
        }
    }

    if (g_country_display_names !== false)
    {
        try
        {
            let name = g_country_display_names.of(code);

            if (typeof name === "string" && name.length > 0)
            {
                return name;
            }
        }
        catch (lookup_error) { }
    }

    return code;
}

// every code the flag stylesheet can draw, sorted by display name; [] headless
function get_all_country_codes()
{
    if (g_country_code_cache != null)
    {
        return g_country_code_cache;
    }

    let codes = {};

    if (typeof document !== "undefined" && document.styleSheets != null)
    {
        for (let sheet_index = 0; sheet_index < document.styleSheets.length; sheet_index++)
        {
            let rules = null;

            try
            {
                rules = document.styleSheets[sheet_index].cssRules;
            }
            catch (rules_error)
            {
                continue;
            }

            if (rules == null)
            {
                continue;
            }

            for (let rule_index = 0; rule_index < rules.length; rule_index++)
            {
                let selector = rules[rule_index].selectorText;

                if (typeof selector !== "string")
                {
                    continue;
                }

                let matches = selector.match(/\.country-flag-([a-z]{2})\b/g);

                if (matches == null)
                {
                    continue;
                }

                for (let match_index = 0; match_index < matches.length; match_index++)
                {
                    codes[matches[match_index].slice(-2).toUpperCase()] = true;
                }
            }
        }
    }

    g_country_code_cache = Object.keys(codes).sort(function(a, b)
    {
        return get_country_display_name(a).localeCompare(get_country_display_name(b));
    });

    return g_country_code_cache;
}

// fills the picker once, the first time the section becomes visible
function populate_country_block_select()
{
    if (g_is_country_select_populated == true)
    {
        return;
    }

    let select = document.getElementById("server-settings-country-block-select");

    if (select == null)
    {
        return;
    }

    let codes = get_all_country_codes();

    if (codes.length == 0)
    {
        return;
    }

    let html = "<option value=\"\">add a country to the block list ...</option>";

    for (let i = 0; i < codes.length; i++)
    {
        html += "<option value=\"" + codes[i] + "\">" + sanitize_string(get_country_display_name(codes[i])) + " (" + codes[i] + ")</option>";
    }

    select.innerHTML = html;
    g_is_country_select_populated = true;
}

function render_blocked_countries_list()
{
    let container = document.getElementById("server-settings-blocked-countries-list");

    if (container == null)
    {
        return;
    }

    container.innerHTML = "";

    if (g_blocked_countries_draft.length == 0)
    {
        let empty = document.createElement("p");
        empty.className = "blocked-country-empty";
        empty.innerText = "no blocked countries";
        container.appendChild(empty);
        return;
    }

    for (let i = 0; i < g_blocked_countries_draft.length; i++)
    {
        let code = g_blocked_countries_draft[i];

        let row = document.createElement("div");
        row.className = "blocked-country-entry";

        let flag = document.createElement("div");
        flag.className = "blocked-country-flag country-flag-" + code.toLowerCase();
        row.appendChild(flag);

        let name = document.createElement("span");
        name.className = "blocked-country-name";
        name.innerText = get_country_display_name(code) + " (" + code + ")";
        row.appendChild(name);

        let remove = document.createElement("span");
        remove.className = "blocked-country-remove";
        remove.title = "remove";
        remove.innerText = "✕";
        remove.setAttribute("data-country-code", code);
        remove.onclick = function(event)
        {
            event.stopPropagation();

            let removed_code = event.currentTarget.getAttribute("data-country-code");

            g_blocked_countries_draft = g_blocked_countries_draft.filter(function(entry) { return entry != removed_code; });
            render_blocked_countries_list();
        };
        row.appendChild(remove);

        container.appendChild(row);
    }
}

// picking an option adds it to the draft; the picker snaps back to its placeholder
function country_block_select_onchange()
{
    let select = document.getElementById("server-settings-country-block-select");
    let code = select.value;

    select.value = "";

    if (typeof code !== "string" || /^[A-Z]{2}$/.test(code) == false)
    {
        return;
    }

    if (g_blocked_countries_draft.indexOf(code) != -1)
    {
        return;
    }

    if (g_blocked_countries_draft.length >= 100)
    {
        custom_alert("the block list is full (100 countries)");
        return;
    }

    g_blocked_countries_draft.push(code);
    g_blocked_countries_draft.sort(function(a, b)
    {
        return get_country_display_name(a).localeCompare(get_country_display_name(b));
    });
    render_blocked_countries_list();
}

// the checkbox shows or hides the picker below it (the block list itself is server state
// either way; unchecking just disables enforcement, it does not clear the list)
function refresh_country_blocking_visibility()
{
    let checkbox = document.getElementById("server-settings-general-country-blocking-checkbox");
    let container = document.getElementById("server-settings-country-blocking-container");

    if (checkbox == null || container == null)
    {
        return;
    }

    if (checkbox.checked == true)
    {
        populate_country_block_select();
        container.style.display = "block";
    }
    else
    {
        container.style.display = "none";
    }
}

// what the server currently has, straight from server_settings_values
function set_blocked_countries_from_server(list)
{
    g_blocked_countries_draft = [];

    if (Array.isArray(list))
    {
        for (let i = 0; i < list.length; i++)
        {
            if (typeof list[i] === "string" && /^[A-Za-z]{2}$/.test(list[i]) == true
                && g_blocked_countries_draft.indexOf(list[i].toUpperCase()) == -1)
            {
                g_blocked_countries_draft.push(list[i].toUpperCase());
            }
        }
    }

    render_blocked_countries_list();
}


// the export seam for the headless (nodejs-mobile) build. every g_* and both message
// tables are LOCALS of moduleFactory, which until now returned nothing, so there was no
// way for a node host to dispatch a message or read the result.
//
// this changes nothing in a browser. the umd head at the top of aes-js.js picks its
// branch by environment: node/commonjs takes `module.exports = factory()` and receives
// this object, a page takes `root.what = factory()` and simply discards it.
return {
    server_msg: server_msg,
    client_msg: client_msg,
    // the java -> js entry points (messages.js:3183). the service runtime needs these:
    // accept_current_settings_from_android is how the android settings screen reaches the client
    android_js_bridge: android_js_bridge,

    // call once before connecting: stands up the transport and the log sink
    init_node_runtime: init_node_runtime,

    // the webview handover: false closes the socket and parks reconnect, true re-arms
    node_set_connection_wanted: node_set_connection_wanted,

    // sets the username this client asks for while connecting, because a headless host
    // has no page variable to edit; "" goes back to the assigned name
    node_set_chosen_username: function(username)
    {
        g_chosen_username = (typeof username === "string") ? username : "";
    },

    // a plaintext request from the loopback ui: encrypt it and send to the real server
    node_forward_raw_request: function(json_string)
    {
        websocket_worker_send(encrypt_all_message_data_and_convert_to_base64(json_string));
    },

    // callback(json_string) for every decrypted server frame, raw - feeds the ui replay
    set_frame_listener: function(callback)
    {
        g_node_frame_listener = (typeof callback === "function") ? callback : null;
    },

    // callback(status) on every connection status change - additive, every caller gets called
    set_connection_status_listener: function(callback)
    {
        if (typeof callback === "function") { g_connection_status_listeners.push(callback); }
    },

    // callback(total) whenever the unread count changes - drives the app icon badge
    set_unread_listener: function(callback)
    {
        g_node_unread_listener = (typeof callback === "function") ? callback : null;
    },

    // callback(caller_username, channel_id): headless node's only route to the
    // native accept/decline screen
    set_incoming_call_listener: function(callback)
    {
        g_node_incoming_call_listener = (typeof callback === "function") ? callback : null;
    },

    // a ui attached to / left the loopback. while one is attached the user is looking,
    // so node stops counting and clears what it accumulated; the ui owns the badge then
    node_set_ui_attached: function(is_attached, is_from_host)
    {
        if (is_from_host === true)
        {
            g_node_ui_visibility_from_host = true;
        }
        else if (g_node_ui_visibility_from_host == true)
        {
            // the webview redialling the loopback in the background is not the user
            // coming back, and java has already said so
            return;
        }

        let was_attached = g_node_has_attached_ui;
        g_node_has_attached_ui = (is_attached === true);

        if (g_node_has_attached_ui == true)
        {
            g_channel_unread_counts = {};

            for (let i = 0; i < g_client_list.length; i++)
            {
                g_client_list[i].unread_count = 0;
            }

            update_app_unread_badge();
        }

        // headless node is a client nobody is sitting at, so it belongs in idle -
        // that is what lets other people CALL this phone. without this it just stood
        // in the root channel looking present
        if (g_is_authenticated == true && was_attached != g_node_has_attached_ui)
        {
            node_apply_idle_for_ui_state();
        }
    },

    // java's connectivity watch: the only trustworthy "is there a network" on android
    node_set_device_network: function(is_available)
    {
        g_device_has_network = (is_available === true);

        // say it immediately - the retry text would otherwise keep the stale guess
        if (g_device_has_network === false && g_is_authenticated == false)
        {
            g_last_disconnect_reason = "no network connection (wifi and mobile data are off)";
            report_connection_status(g_connection_status.state, g_last_disconnect_reason);
        }
    },

    get_connection_status: function()
    {
        return g_connection_status;
    },

    // a ui attached to the loopback: it wants to be online. an attach is always the
    // downstream of an authorized decision, so request_connect dials for it
    node_connect_intent: function()
    {
        if (g_have_received_android_settings == true)
        {
            request_connect("attach");
        }
    },

    // the auth frame, whenever it arrived - replay leads with it regardless of start order
    get_auth_frame: function()
    {
        return g_node_cached_auth_frame;
    },

    // callback(message_type, had_error), fired after every dispatched message - the
    // host's "state may have changed" signal. additive: every caller gets called
    set_on_message_processed: function(callback)
    {
        if (typeof callback === "function") { g_node_message_listeners.push(callback); }
    },

    // built fresh per call, since several of these are reassigned rather than mutated.
    // READ-ONLY: the arrays come back live, and writing through them bypasses the
    // invariants the handlers keep (e.g. g_client_list vs its index map). the smoke test
    // seeds state this way on purpose; production code must not.
    read_state: function()
    {
        return {
            g_client_list: g_client_list,
            g_channel_list: g_channel_list,
            g_chat_context_array: g_chat_context_array,
            g_map_client_id_to_array_index: g_map_client_id_to_array_index,
            g_offline_client_list: g_offline_client_list,
            g_chat_message_author_public_keys: g_chat_message_author_public_keys,
            g_tags: g_tags,
            g_icons: g_icons,
            current_channel_id: current_channel_id,
            current_channel_keys: current_channel_keys,
            local_client_id: local_client_id,
            g_local_username: g_local_username,
            g_current_chat_context_id: g_current_chat_context_id,
            g_chat_message_receiver_type: g_chat_message_receiver_type,
            g_is_client_list_retrieved: g_is_client_list_retrieved,
            g_is_channel_list_retrieved: g_is_channel_list_retrieved,
            // the live connection signal (g_is_websocket_connected is a dead flag, see ui.js)
            g_is_authenticated: g_is_authenticated,
            g_is_microphone_always_on: g_is_microphone_always_on,
            g_have_received_android_settings: g_have_received_android_settings
        };
    }
};
}));
