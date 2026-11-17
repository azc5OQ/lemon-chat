// handles identity sizes, connection encryption and channel key distribution
// workers.js applies message encryption in the data worker

// vote again when the channel maintainer does not deliver keys before the timeout
var MAINTAINER_KEYS_WAIT_TIMEOUT_MS = 5000;

var maintainer_keys_wait_timer = null;

var maintainer_keys_wait_channel_id = -1;
var channel_key_expiry_timer = null;
var channel_key_expiry_deadline = 0;
var CHANNEL_KEY_HISTORY_MS = 10 * 60 * 1000;

/**
 * @brief creates four random AES keys and starting counters for encrypting a direct message
 *
 * @return array the keys as [{ key_bytes, iv_bytes }]
 */
function keys__create_random_message_keys()
{
    let keys = [];
    let i = 0;

    for (i = 0; i < 4; i++)
    {
        keys.push(
        {
            key_bytes: Array.from(crypto.getRandomValues(new Uint8Array(32))),
            iv_bytes: Array.from(crypto.getRandomValues(new Uint8Array(16)))
        });
    }

    return keys;
}

/**
 * @brief rejects missing or incomplete content keys so an edit cannot be sent without encryption
 *
 * @param array keys -> the four AES key and counter pairs
 *
 * @return boolean whether every pair has the expected bytes
 */
function keys__are_message_keys_valid(keys)
{
    return Array.isArray(keys) && keys.length === 4 && keys.every(function(key)
    {
        return key != null && Array.isArray(key.key_bytes) && key.key_bytes.length === 32 && Array.isArray(key.iv_bytes) && key.iv_bytes.length === 16
            && key.key_bytes.concat(key.iv_bytes).every(function(byte)
            {
                return Number.isInteger(byte) && byte >= 0 && byte <= 255;
            });
    });
}

/**
 * @brief encrypts direct text or pictures using only the four fresh key IVs
 *
 * @param array keys -> four fresh content keys and IVs
 * @param string plaintext -> text or picture data URL
 * @param number padding -> minimum plaintext size, defaults to 1024 bytes
 *
 * @return string|null internal {data} envelope, or null on failure
 */
function keys__encrypt_direct_content(keys, plaintext, padding)
{
    let floor = typeof padding === "number" ? padding : 1024;
    let bytes = null;
    let data = null;
    let result = null;
    let key = null;

    try
    {
        if (!keys__are_message_keys_valid(keys) || typeof plaintext !== "string" || plaintext.length > 40 * 1024 * 1024 || !Number.isSafeInteger(floor) || floor < 0 || floor > 4096)
        {
            return null;
        }
        bytes = aesjs.utils.utf8.toBytes(plaintext);
        data = new Uint8Array(Math.max(bytes.length, floor));
        data.set(bytes);

        // use each key's own IV
        for (key of keys)
        {
            data = keys__aes_ctr(key.key_bytes, key.iv_bytes, data);
        }
        result = JSON.stringify({data: keys___packet_to_base64(data)});
        return result.length <= 40 * 1024 * 1024 ? result : null;
    }
    catch (error)
    {
        return null;
    }
}

/**
 * @brief decrypts direct text or pictures using the four key IVs
 *
 * @param array keys -> recovered content keys and IVs
 * @param string content -> internal {data} envelope
 *
 * @return string|null plaintext, or null for invalid content
 */
function keys__decrypt_direct_content(keys, content)
{
    let body = null;
    let data = null;
    let end = 0;
    let index = 0;

    try
    {
        if (!keys__are_message_keys_valid(keys) || typeof content !== "string" || content.length > 40 * 1024 * 1024)
        {
            return null;
        }
        body = JSON.parse(content);
        if (body == null || typeof body !== "object" || Array.isArray(body) || Object.keys(body).length !== 1)
        {
            return null;
        }
        data = keys___packet_from_base64(body.data);
        if (data == null || !data.length)
        {
            return null;
        }

        // remove encryption in reverse order
        for (index = keys.length - 1; index >= 0; index--)
        {
            data = keys__aes_ctr(keys[index].key_bytes, keys[index].iv_bytes, data);
        }
        end = data.indexOf(0);
        if (end >= 0 && data.subarray(end).some(function(byte)
        {
            return byte !== 0;
        }))
        {
            return null;
        }
        return new TextDecoder("utf-8", {fatal: true}).decode(end >= 0 ? data.subarray(0, end) : data);
    }
    catch (error)
    {
        return null;
    }
}

/**
 * @brief protects the four fresh keys and their IVs for the direct recipient
 *
 * @param string public_key -> recipient's RSA public key
 * @param array keys -> four fresh content keys and IVs
 * @param string content -> internal {data} or {data,tag} envelope
 *
 * @return object|null direct envelope, or null on failure
 */
function keys__seal_direct_content(public_key, keys, content)
{
    let body = null;
    let encrypted_keys = null;
    let value = null;

    try
    {
        if (!keys__are_message_keys_valid(keys) || typeof content !== "string" || content.length > 40 * 1024 * 1024)
        {
            return null;
        }
        body = JSON.parse(content);
        if (body == null || Array.isArray(body) || typeof body.data !== "string" || !body.data.length)
        {
            return null;
        }

        // preserve the file check
        if (Object.prototype.hasOwnProperty.call(body, "tag"))
        {
            if (Object.keys(body).length !== 2 || typeof body.tag !== "string" || !/^[A-Za-z0-9+/]{43}=$/.test(body.tag))
            {
                return null;
            }
            value = JSON.stringify({data: body.data, tag: body.tag});
        }
        else
        {
            if (Object.keys(body).length !== 1)
            {
                return null;
            }
            value = body.data;
        }

        // protect the four key and IV pairs
        encrypted_keys = lemon_crypto.encrypt(JSON.stringify({keys: keys}), public_key);
        if (encrypted_keys == null || encrypted_keys.status !== "success")
        {
            return null;
        }
        return {message_keys: encrypted_keys.cipher, message_value: value};
    }
    catch (error)
    {
        return null;
    }
}

/**
 * @brief recovers the four direct message keys and their IVs
 *
 * @param object envelope -> received direct envelope
 * @param object private_key -> recipient's RSA private key
 *
 * @return object|null keys and internal content, or null on failure
 */
function keys__open_direct_content(envelope, private_key)
{
    let decrypted_keys = null;
    let protected_keys = null;
    let body = null;

    try
    {
        if (envelope == null || typeof envelope !== "object" || Array.isArray(envelope) || Object.keys(envelope).length !== 2 || typeof envelope.message_keys !== "string" || envelope.message_keys.length > 16384 || typeof envelope.message_value !== "string" || !envelope.message_value.length || envelope.message_value.length > 40 * 1024 * 1024)
        {
            return null;
        }

        // read the keys encrypted for this recipient
        decrypted_keys = lemon_crypto.decrypt(envelope.message_keys, private_key);
        if (decrypted_keys == null || decrypted_keys.status !== "success")
        {
            return null;
        }
        protected_keys = JSON.parse(decrypted_keys.plaintext);
        if (protected_keys == null || Array.isArray(protected_keys) || Object.keys(protected_keys).length !== 1 || !keys__are_message_keys_valid(protected_keys.keys))
        {
            return null;
        }

        // keep text and file content separate
        if (envelope.message_value[0] === "{")
        {
            body = JSON.parse(envelope.message_value);
            if (body == null || Array.isArray(body) || Object.keys(body).length !== 2 || typeof body.data !== "string" || !body.data.length || typeof body.tag !== "string" || !/^[A-Za-z0-9+/]{43}=$/.test(body.tag))
            {
                return null;
            }
        }
        else
        {
            body = {data: envelope.message_value};
        }
        return {keys: protected_keys.keys, value: JSON.stringify(body)};
    }
    catch (error)
    {
        return null;
    }
}

/**
 * @brief encodes a packet without building a large array of individual numbers
 *
 * @param Uint8Array bytes -> packet bytes
 *
 * @return string Base64 data
 */
function keys___packet_to_base64(bytes)
{
    let chunks = [];
    let offset = 0;

    if (typeof btoa !== "function")
    {
        return utils__bytesToBase64String(bytes);
    }
    for (offset = 0; offset < bytes.length; offset += 32768)
    {
        chunks.push(String.fromCharCode.apply(null, bytes.subarray(offset, offset + 32768)));
    }
    return btoa(chunks.join(""));
}

/**
 * @brief reads bounded Base64 packet data
 *
 * @param string text -> Base64 data
 *
 * @return Uint8Array|null decoded bytes, or null for invalid input
 */
function keys___packet_from_base64(text)
{
    let decoded = null;
    let bytes = null;
    let index = 0;

    if (typeof text !== "string" || !text.length || text.length > 40 * 1024 * 1024 || text.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(text))
    {
        return null;
    }
    if (typeof atob !== "function")
    {
        return new Uint8Array(utils__base64StringToBytes(text));
    }
    decoded = atob(text);
    bytes = new Uint8Array(decoded.length);
    for (index = 0; index < decoded.length; index++)
    {
        bytes[index] = decoded.charCodeAt(index);
    }
    return bytes;
}

/**
 * @brief derives a separate AES key for hiding a channel message IV
 *
 * @param array keys -> four channel key and IV pairs
 *
 * @return array the IV encryption key, 32 bytes
 */
function keys___channel_iv_key(keys)
{
    let material = [];
    let key = null;

    for (key of keys)
    {
        material = material.concat(key.key_bytes, key.iv_bytes);
    }
    return keys___hkdf_sha256(material, new Uint8Array(32), aesjs.utils.utf8.toBytes("lemon-channel-envelope-v3"), 32);
}

/**
 * @brief hides the message IV before sending encrypted channel content
 *
 * @param array keys -> four channel key and IV pairs
 * @param string content -> internal encrypted text, picture or file envelope
 *
 * @return string|null channel envelope with a protected IV, or null on failure
 */
function keys__seal_channel_content(keys, content)
{
    let body = null;
    let iv = null;
    let data = null;
    let tag = null;
    let packet = null;
    let offset = 17;
    let result = null;

    try
    {
        if (!keys__are_message_keys_valid(keys) || typeof content !== "string" || content.length > 40 * 1024 * 1024)
        {
            return null;
        }
        body = JSON.parse(content);
        if (body == null || Array.isArray(body))
        {
            return null;
        }
        iv = keys___packet_from_base64(body.iv);
        data = keys___packet_from_base64(body.data);
        if (iv == null || iv.length !== 16 || data == null || !data.length)
        {
            return null;
        }

        // keep the existing file tag
        if (Object.prototype.hasOwnProperty.call(body, "v"))
        {
            tag = keys___packet_from_base64(body.tag);
            if (body.v !== 2 || Object.keys(body).length !== 4 || tag == null || tag.length !== 32)
            {
                return null;
            }
            offset = 49;
        }
        else if (Object.keys(body).length !== 2)
        {
            return null;
        }

        // encrypt only the IV block
        packet = new Uint8Array(offset + data.length);
        packet[0] = tag == null ? 0 : 1;
        packet.set(new aesjs.ModeOfOperation.ecb(keys___channel_iv_key(keys)).encrypt(iv), 1);
        if (tag != null)
        {
            packet.set(tag, 17);
        }
        packet.set(data, offset);
        result = JSON.stringify({message_value: keys___packet_to_base64(packet)});
        return result.length <= 40 * 1024 * 1024 ? result : null;
    }
    catch (error)
    {
        return null;
    }
}

/**
 * @brief recovers a channel message IV for internal content decryption
 *
 * @param array keys -> current or previous channel key and IV pairs
 * @param string encrypted -> received channel envelope with a protected IV
 *
 * @return string|null restored internal envelope, or null for invalid input
 */
function keys__open_channel_content(keys, encrypted)
{
    let envelope = null;
    let packet = null;
    let offset = 0;
    let iv = null;
    let body = null;
    let result = null;

    try
    {
        if (!keys__are_message_keys_valid(keys) || typeof encrypted !== "string" || encrypted.length > 40 * 1024 * 1024)
        {
            return null;
        }
        envelope = JSON.parse(encrypted);
        if (envelope == null || typeof envelope !== "object" || Array.isArray(envelope) || Object.keys(envelope).length !== 1)
        {
            return null;
        }
        packet = keys___packet_from_base64(envelope.message_value);
        if (packet == null || packet.length < 18 || packet[0] > 1)
        {
            return null;
        }
        offset = packet[0] === 1 ? 49 : 17;
        if (packet.length <= offset)
        {
            return null;
        }

        // restore the IV locally
        iv = new aesjs.ModeOfOperation.ecb(keys___channel_iv_key(keys)).decrypt(packet.subarray(1, 17));
        body = {iv: keys___packet_to_base64(iv), data: keys___packet_to_base64(packet.subarray(offset))};
        if (packet[0] === 1)
        {
            body.v = 2;
            body.tag = keys___packet_to_base64(packet.subarray(17, 49));
        }
        result = JSON.stringify(body);
        return result.length <= 40 * 1024 * 1024 ? result : null;
    }
    catch (error)
    {
        return null;
    }
}

/**
 * @brief reads channel keys only when their encrypted channel and maintainer version match this client's channel
 *
 * @param object message -> the received message with its decrypted contents in some_json
 * @param number channel_id -> the client's current channel
 * @param number maintainer_generation -> the current maintainer version announced by the server
 *
 * @return array|null four valid AES keys, or null for an invalid delivery
 */
function keys__read_channel_key_message(message, channel_id, maintainer_generation)
{
    let content = message.some_json;
    let keys = null;
    if (content == null || content.type !== "channel_keys_from_maintainer" || !Number.isSafeInteger(content.channel_id) || content.channel_id < 0 || !Number.isSafeInteger(content.maintainer_generation) || content.maintainer_generation < 0 || content.channel_id !== channel_id || content.maintainer_generation !== maintainer_generation)
    {
        return null;
    }
    try
    {
        keys = JSON.parse(content.value);
    }
    catch (error)
    {
        return null;
    }
    if (!Array.isArray(keys) || keys.length !== 4)
    {
        return null;
    }
    for (let key of keys)
    {
        if (key == null || !Array.isArray(key.key_bytes) || key.key_bytes.length !== 32 || !Array.isArray(key.iv_bytes) || key.iv_bytes.length !== 16
            || !key.key_bytes.concat(key.iv_bytes).every(function(byte)
            {
                return Number.isInteger(byte) && byte >= 0 && byte <= 255;
            }))
        {
            return null;
        }
    }
    return keys;
}

/**
 * @brief removes previous channel keys ten minutes after they were replaced
 *
 * @return void
 */
function keys__remove_expired_channel_keys()
{
    let now = performance.now();
    let deadline = 0;
    let retained = new Set();
    let entry = null;
    let file_id = null;
    g_historic_keys_of_current_channel = g_historic_keys_of_current_channel.filter(function(entry)
    {
        return entry.expires_at > now;
    });
    retained.add(g_current_channel_keys);
    for (entry of g_historic_keys_of_current_channel)
    {
        retained.add(entry.keys);
        if (deadline === 0 || entry.expires_at < deadline)
        {
            deadline = entry.expires_at;
        }
    }
    // a file download may remember which keys opened its header
    // forget that copy too when the keys expire
    if (typeof chat_file_keys_by_file_id !== "undefined")
    {
        for (file_id of Object.keys(chat_file_keys_by_file_id))
        {
            if (!retained.has(chat_file_keys_by_file_id[file_id]))
            {
                delete chat_file_keys_by_file_id[file_id];
            }
        }
    }
    if (deadline !== channel_key_expiry_deadline)
    {
        clearTimeout(channel_key_expiry_timer);
        channel_key_expiry_timer = null;
        channel_key_expiry_deadline = deadline;
        if (deadline > 0)
        {
            channel_key_expiry_timer = setTimeout(function()
            {
                channel_key_expiry_deadline = 0;
                channel_key_expiry_timer = null;
                keys__remove_expired_channel_keys();
            }, Math.max(1, deadline - now));
        }
    }
}

/**
 * @brief replaces channel keys and retains old sets for ten minutes
 *
 * @param array|null keys -> the new set, or null while waiting for replacement keys
 *
 * @return void
 */
function keys__set_current_channel_keys(keys)
{
    if (g_current_channel_keys !== keys)
    {
        if (g_current_channel_keys != null)
        {
            g_historic_keys_of_current_channel.push(
            {
                keys: g_current_channel_keys,
                expires_at: performance.now() + CHANNEL_KEY_HISTORY_MS
            });
        }
        g_current_channel_keys = keys;
    }
    keys__remove_expired_channel_keys();

    // send waiting message
    if (keys != null && typeof chat__send_message_waiting_for_channel_keys === "function")
    {
        chat__send_message_waiting_for_channel_keys();
    }
}

/**
 * @brief returns the current keys and unexpired previous sets, newest first
 *
 * @param array|null preferred -> the set that decrypted a file header, if still valid
 *
 * @return array the key sets that may still be tried
 */
function keys__get_channel_key_candidates(preferred = null)
{
    let candidates = [];
    let preferred_index = -1;
    let i = 0;
    keys__remove_expired_channel_keys();
    if (g_current_channel_keys != null)
    {
        candidates.push(g_current_channel_keys);
    }
    for (i = g_historic_keys_of_current_channel.length - 1; i >= 0; i--)
    {
        candidates.push(g_historic_keys_of_current_channel[i].keys);
    }
    preferred_index = candidates.indexOf(preferred);
    if (preferred_index > 0)
    {
        candidates.splice(preferred_index, 1);
        candidates.unshift(preferred);
    }
    return candidates;
}

/**
 * @brief forgets the current and previous channel keys after a full connection reset
 *
 * @return void
 */
function keys__clear_channel_keys()
{
    g_current_channel_keys = null;
    g_historic_keys_of_current_channel.length = 0;
    keys__remove_expired_channel_keys();
}

/**
 * @brief tries current and unexpired keys when channel text or a picture arrives late
 *
 * @param string encrypted -> the encrypted message contents
 * @param boolean picture -> true for an image data URL, false for chat text in JSON
 *
 * @return string|null the readable contents, or null when no retained set works
 */
function keys__decrypt_channel_contents(encrypted, picture)
{
    let candidates = keys__get_channel_key_candidates();
    let envelope = null;
    let content = null;
    let bytes = null;
    let iv = null;
    let prefix = null;
    let block = null;
    let text = "";
    let parsed = null;
    let keys = null;
    let i = 0;
    if (typeof encrypted !== "string")
    {
        return null;
    }
    try
    {
        for (keys of candidates)
        {
            content = keys__open_channel_content(keys, encrypted);
            if (content == null)
            {
                continue;
            }
            envelope = JSON.parse(content);
            if (Object.prototype.hasOwnProperty.call(envelope, "v"))
            {
                continue;
            }
            iv = keys___packet_from_base64(envelope.iv);
            bytes = keys___packet_from_base64(envelope.data);
            // try the keys on the first few bytes before decrypting the whole image
            prefix = bytes.slice(0, 64);
            for (i = keys.length - 1; i >= 0; i--)
            {
                block = iv == null ? keys[i].iv_bytes : keys__media_xor_blocks(keys[i].iv_bytes, iv);
                prefix = keys__aes_ctr(keys[i].key_bytes, block, prefix);
            }
            text = new TextDecoder().decode(prefix);
            if (picture ? !text.startsWith("data:image/") : !text.trimStart().startsWith("{"))
            {
                continue;
            }
            text = keys__decrypt_base64_contents_with_aes_keys(keys, content);
            if (picture)
            {
                return text;
            }
            try
            {
                parsed = JSON.parse(text);
                if (parsed != null && (typeof parsed.value === "string" || (parsed.type === "chat_reaction" && typeof parsed.message_id === "string" && /^[a-f0-9]{32}$/.test(parsed.message_id) && typeof parsed.emoji === "string" && typeof parsed.active === "boolean") || (parsed.type === "chat_message_edit" && typeof parsed.message_id === "string" && /^[a-f0-9]{32}$/.test(parsed.message_id) && typeof parsed.author_public_key === "string" && typeof parsed.new_message_value === "string")))
                {
                    return text;
                }
            }
            catch (parse_error)
            {
                // this set did not produce the expected message. try the next retained set
            }
        }
    }
    catch (decode_error)
    {
        return null;
    }
    return null;
}

// ---- identity and message encryption ----

/**
 * @brief selects the smallest supported key size satisfying the server minimum
 *
 * @param number minimum_bits -> the size the server demands
 *
 * @return number the size to generate, 0 when no offered size is big enough
 */
function keys__pick_rsa_key_bits_for_minimum(minimum_bits)
{
    for (let i = 0; i < G_ALLOWED_RSA_KEY_BITS.length; i++)
    {
        if (G_ALLOWED_RSA_KEY_BITS[i] >= minimum_bits)
        {
            return G_ALLOWED_RSA_KEY_BITS[i];
        }
    }
    return 0;
}

/**
 * @brief offers a supported identity size after the server rejects a smaller key
 *
 * @param number|string announced_minimum_bits -> the size from the notice, treated as untrusted input
 *
 * @return void
 */
function keys__handle_rsa_key_too_weak_notice(announced_minimum_bits)
{
    let minimum_bits = parseInt(announced_minimum_bits);

    // this notice arrives before authentication. reject sizes outside the supported range
    if (isNaN(minimum_bits) || minimum_bits < 2048 || minimum_bits > 8192)
    {
        console.warn("ignoring an implausible key size requirement from the server: " + announced_minimum_bits);
        return;
    }

    // compare the selected identity, not the preference for the next identity
    const current_bits = g_identity_rsa_key_bits || identity_file__generation_size(g_rsa_public_key_string);
    if (current_bits >= minimum_bits)
    {
        return;
    }

    let target_bits = keys__pick_rsa_key_bits_for_minimum(minimum_bits);

    if (target_bits == 0)
    {
        utils__custom_alert("this server requires an RSA key of " + minimum_bits + " bits, which this client cannot create");
        return;
    }

    // do not reopen this dialog on every rejected reconnect attempt
    if (g_rsa_key_too_weak_prompted_for_bits == minimum_bits)
    {
        return;
    }
    g_rsa_key_too_weak_prompted_for_bits = minimum_bits;

    document.getElementById("rsa-key-too-weak-text").textContent = "This server requires an identity key of at least " + minimum_bits + " bits. Your key is " + (current_bits || "an unknown number of") + " bits, so the server refused the connection. Switch this device to " + target_bits + "-bit keys and reconnect? Creating the key may take a while, and it gives " + "you a NEW identity here - the same passphrase produces a different key at a different size.";

    document.getElementById("rsa-key-too-weak-yes-button").setAttribute("data-target-bits", target_bits);
    document.getElementById("rsa-key-too-weak-container").style.display = "block";
    document.getElementById("background-container").style.display = "block";
}

/**
 * @brief hides the "stronger key required" dialog
 *
 * @return void
 */
function keys__hide_rsa_key_too_weak_dialog()
{
    document.getElementById("rsa-key-too-weak-container").style.display = "none";
    document.getElementById("background-container").style.display = "none";
}

/**
 * @brief hashes configured passwords and sends the connection keys to the data worker
 *
 * @return void
 */
function keys__init_keys_object()
{
    g_metadata_keys.length = 0; // clear it in case client is reconnecting
    g_media_keys_from_host = null;
    g_media_server_key_layers_from_host = null;

    // prefer the current form rows. use predefined keys only when no rows were built
    let key_input_elements = (typeof document !== "undefined" && typeof document.querySelectorAll === "function") ? document.querySelectorAll('#connect-keys-grid input[id^="input-key-"]') : [];
    let keys_grid = (typeof document !== "undefined" && typeof document.getElementById === "function") ? document.getElementById("connect-keys-grid") : null;
    let are_key_rows_built = (keys_grid != null && typeof keys_grid.getAttribute === "function" && keys_grid.getAttribute("data-written-in-keys-built") === "true");

    if (g_are_server_details_predefined == true && key_input_elements.length == 0 && are_key_rows_built == false)
    {
        for (let i = 0; i < g_autoconnect_details.keys.length; i++)
        {
            let entry = connection_crypto__password_entry(g_autoconnect_details.keys[i]);
            let key_string_value = entry.password;
            // keep key material out of logs
            let hash = _sha256.create();

            hash.update(key_string_value);
            hash.array();
            key_bytes = hash.array();

            let single_key = {
                info: "aes-ctr",
                key_type: entry.type,
                key_string: key_string_value,
                key_bytes: key_bytes
            };

            g_metadata_keys[i] = single_key;
        }
    }
    else
    {
        // use the remaining input rows in order because removed rows leave gaps in their IDs
        for (let i = 0; i < key_input_elements.length; i++)
        {
            let key_string_value = key_input_elements[i].value;
            // keep key material out of logs

            // ignore empty password rows
            if (key_string_value.length == 0)
            {
                continue;
            }

            let hash = _sha256.create();

            hash.update(key_string_value);
            hash.array();
            key_bytes = hash.array();

            // read the cipher selected for this password
            let key_type_select = document.getElementById("input-key-type-" + key_input_elements[i].getAttribute("data-id"));

            let single_key = {
                info: "aes-ctr",
                key_type: (key_type_select != null && typeof key_type_select.value === "string") ? key_type_select.value : "aes256",
                key_string: key_string_value,
                key_bytes: key_bytes
            };

            g_metadata_keys.push(single_key);
        }
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__metadata_keys",
        value: g_metadata_keys
    });

    // keep connection keys out of logs
}

/**
 * @brief generates a secure Diffie Hellman exponent and computes its public value
 *
 * @return bigint the public mix g^x mod p
 */
function keys__get_public_mix()
{
    // use 256 exponent bits for a 2048 bit modulus and 512 for larger moduli
    let dh_exponent_bits = (dh_modulus_bits == 2048) ? 256 : 512;

    // generate secret bits with the cryptographic random source
    let random_exponent_bytes = new Uint8Array(dh_exponent_bits / 8);
    crypto.getRandomValues(random_exponent_bytes);

    let random_exponent_binary_string = "0b";

    // set the first bit to keep the exponent at the requested length
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

    let A = lemon_crypto.modpow(g_dh_generator, g_dh_secret_exponent, g_dh_modulus);

    return A;
}

/**
 * @brief encrypts bytes with each AES CTR key in order
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param array|Uint8Array data -> the bytes to encrypt
 *
 * @return Uint8Array the encrypted bytes
 */
function keys__encrypt_data_with_aes_keys(keys, data)
{
    let encrypted_data_in_current_iteration = new Uint8Array(data);

    for (let i = 0; i < keys.length; i++)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        encrypted_data_in_current_iteration = aesCtrCustom.encrypt(encrypted_data_in_current_iteration);
    }
    return encrypted_data_in_current_iteration;
}

/**
 * @brief decrypts bytes with the AES CTR keys in reverse order
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param array|Uint8Array data -> the bytes to decrypt
 *
 * @return Uint8Array the decrypted bytes
 */
function keys__decrypt_data_with_aes_keys(keys, data)
{
    let current_data = new Uint8Array(data);

    for (let i = (keys.length - 1); i >= 0; i--)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        current_data = aesCtrCustom.decrypt(current_data);
    }

    return current_data;
}

/**
 * @brief pads and encrypts channel content with a fresh message IV
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string string_to_encrypt -> the plaintext
 *
 * @return string a JSON text { "iv": <base64 of the message iv>, "data": <base64 of the ciphertext> }
 */
function keys__encrypt_with_aes_keys_and_convert_to_base64(keys, string_to_encrypt)
{
    let textBytes = aesjs.utils.utf8.toBytes(string_to_encrypt);
    let byteArrayToSend = new Uint8Array(textBytes.length > 1024 ? textBytes.length : 1024);
    let message_iv = new Uint8Array(16);

    byteArrayToSend.set(textBytes, 0);
    crypto.getRandomValues(message_iv);

    let encrypted_data_in_current_iteration = byteArrayToSend;

    for (let i = 0; i < keys.length; i++)
    {
        encrypted_data_in_current_iteration = keys__aes_ctr(keys[i].key_bytes, keys__media_xor_blocks(keys[i].iv_bytes, message_iv), encrypted_data_in_current_iteration);
    }
    return JSON.stringify({ iv: utils__bytesToBase64String(message_iv), data: utils__bytesToBase64String(encrypted_data_in_current_iteration) });
}

/**
 * @brief removes AES layers and zero padding from encrypted content
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string string_to_decrypt -> the JSON text from the encrypt function, or an old bare base64 text
 *
 * @return string the plaintext
 */
function keys__decrypt_base64_contents_with_aes_keys(keys, string_to_decrypt)
{
    let message_iv = null;
    let payload = string_to_decrypt;

    if (typeof string_to_decrypt === "string" && string_to_decrypt.charAt(0) === "{")
    {
        try
        {
            let envelope = JSON.parse(string_to_decrypt);
            if (envelope != null && typeof envelope.iv === "string" && typeof envelope.data === "string")
            {
                message_iv = new Uint8Array(utils__base64StringToBytes(envelope.iv));
                payload = envelope.data;
            }
        }
        catch (parse_error)
        {
            // not an envelope after all: decrypted as an old message below
        }
    }

    let encrypted_data_in_current_iteration = new Uint8Array(utils__base64StringToBytes(payload));

    // decrypt by applying keys in reverse order
    for (let i = (keys.length - 1); i >= 0; i--)
    {
        let block = message_iv != null ? keys__media_xor_blocks(keys[i].iv_bytes, message_iv) : new Uint8Array(keys[i].iv_bytes);
        encrypted_data_in_current_iteration = keys__aes_ctr(keys[i].key_bytes, block, encrypted_data_in_current_iteration);
    }

    let str1 = new TextDecoder().decode(encrypted_data_in_current_iteration);
    let decrypted_metadata = utils__substringByNullTerminator(str1);
    return decrypted_metadata;
}

/**
 * @brief hashes bytes for metadata HKDF and HMAC
 *
 * @param array bytes -> the bytes to hash, plain or typed
 *
 * @return array the 32 hash bytes
 */
function keys___sha256_bytes(bytes)
{
    let h = _sha256.create();
    h.update(bytes);
    return h.array();
}

/**
 * @brief standard HMAC-SHA256 built on the plain hash
 *
 * @param array key_bytes -> the mac key, plain or typed
 * @param array msg_bytes -> the message, plain or typed
 *
 * @return array the 32 mac bytes
 */
function keys___hmac_sha256(key_bytes, msg_bytes)
{
    // use plain arrays so concat appends bytes instead of a typed array object
    let k = Array.from(key_bytes);
    let msg = Array.from(msg_bytes);
    if (k.length > 64)
    {
        k = keys___sha256_bytes(k);
    }
    while (k.length < 64)
    {
        k.push(0);
    }
    let ipad = k.map(b => b ^ 0x36);
    let opad = k.map(b => b ^ 0x5c);
    let inner = keys___sha256_bytes(ipad.concat(msg));
    return keys___sha256_bytes(opad.concat(inner));
}

/**
 * @brief HKDF-SHA256 extract + expand
 *
 * @param array ikm_bytes -> the input key material
 * @param array salt_bytes -> the salt
 * @param array info_bytes -> the context info
 * @param number length -> how many derived bytes to produce
 *
 * @return array the derived bytes as a plain array
 */
function keys___hkdf_sha256(ikm_bytes, salt_bytes, info_bytes, length)
{
    let info = Array.from(info_bytes); // plain array so t.concat(info) spreads the bytes
    let prk = keys___hmac_sha256(salt_bytes, ikm_bytes);
    let okm = [];
    let t = [];
    let counter = 1;
    while (okm.length < length)
    {
        t = keys___hmac_sha256(prk, t.concat(info).concat([counter & 0xff]));
        okm = okm.concat(t);
        counter++;
    }
    return okm.slice(0, length);
}

/**
 * @brief derives DH encryption and MAC keys with the same HKDF constants as the server
 *
 * @param string shared_secret_string -> the shared secret as a decimal string
 *
 * @return object { enc_key, mac_key }, 32 bytes each
 */
function keys__dh_derive_keys(shared_secret_string)
{
    let ikm = aesjs.utils.utf8.toBytes(shared_secret_string);
    let salt = aesjs.utils.utf8.toBytes("lemonchat-hkdf-salt-v1");
    let info = aesjs.utils.utf8.toBytes("lemonchat-dh-keys-v1");
    let okm = keys___hkdf_sha256(ikm, salt, info, 64);
    return { enc_key: okm.slice(0, 32), mac_key: okm.slice(32, 64) };
}

// use the stored cipher type and reject unsupported types
/**
 * @brief applies one connection cipher with the outgoing or incoming key
 *
 * @param object key -> cipher type and directional key bytes
 * @param array|Uint8Array message_iv -> message IV, 16 bytes
 * @param array|Uint8Array data -> input bytes
 * @param boolean encrypt -> true to encrypt outgoing data, false to decrypt received data
 *
 * @return Uint8Array -> transformed bytes
 */
function keys__crypt_metadata_layer(key, message_iv, data, encrypt)
{
    let type = key.key_type || (key.info === "aes-ctr" ? "aes256" : key.info);
    let bytes = !encrypt && key.receive_key_bytes ? key.receive_key_bytes : key.key_bytes;
    if (type === "aes256")
    {
        if (bytes.length !== 32 || message_iv.length !== 16)
        {
            throw new Error("Invalid AES-256 key or IV");
        }
        return keys__aes_ctr(bytes, message_iv, data);
    }
    return connection_crypto__crypt(type, bytes, message_iv, data);
}

/**
 * @brief builds the encrypted metadata envelope or forwards plaintext on the local connection
 *
 * @param string string_to_encrypt -> the message json
 *
 * @return string the envelope json, or the plaintext itself on the loopback
 */
function keys__encrypt_all_message_data_and_convert_to_base64(string_to_encrypt)
{
    // trace which runtime applies connection encryption
    if (typeof utils__custom_log === "function" && string_to_encrypt.indexOf("direct_chat_message") !== -1)
    {
        utils__custom_log("[crypto] direct message, ui_only=" + android_host__is_ui_only_runtime() + " loopback_port=" + g_loopback_port + " -> " + (android_host__is_ui_only_runtime() ? "left plain" : "wrapped"));
    }

    // Node encrypts local page requests before forwarding them to the server
    if (android_host__is_ui_only_runtime())
    {
        return string_to_encrypt;
    }

    let textBytes = aesjs.utils.utf8.toBytes(string_to_encrypt);
    let byteArrayToSend = new Uint8Array(Math.max(1024, textBytes.length));
    byteArrayToSend.set(textBytes);

    // generate a fresh public IV for all layers of this message
    let message_iv = new Uint8Array(16);
    crypto.getRandomValues(message_iv);

    let encrypted_data_in_current_iteration = byteArrayToSend;

    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        // the key made from this connection's shared secret is first in the
        // list. encrypt with it first, then with the keys from the server settings
        encrypted_data_in_current_iteration = keys__crypt_metadata_layer( g_metadata_keys[i], message_iv, encrypted_data_in_current_iteration, true);
    }

    // encode the result even when no encryption layers are configured
    let base64EncryptedData = utils__bytesToBase64String(encrypted_data_in_current_iteration);

    let iv_base64 = utils__bytesToBase64String(message_iv);
    let envelope = { iv: iv_base64, data: base64EncryptedData };

    // authenticate the IV and ciphertext when a connection MAC key exists
    let dh_mac_key = null;
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes)
        {
            dh_mac_key = g_metadata_keys[i].mac_bytes;
            break;
        }
    }
    if (dh_mac_key)
    {
        let tag_bytes = keys___hmac_sha256(dh_mac_key, aesjs.utils.utf8.toBytes(iv_base64 + base64EncryptedData));
        envelope.tag = utils__bytesToBase64String(tag_bytes);
    }

    return JSON.stringify(envelope);
}

/**
 * @brief verifies the MAC and removes connection encryption layers
 *
 * @param string envelope_json -> the { iv, data [, tag] } envelope as received
 *
 * @return string the plaintext, "" on any failure
 */
function keys__decrypt_message_metadata(envelope_json)
{
    // read the public IV and ciphertext from the received envelope
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

    // verify the MAC over the IV and ciphertext before decrypting
    let dh_mac_key = null;
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes)
        {
            dh_mac_key = g_metadata_keys[i].receive_mac_bytes || g_metadata_keys[i].mac_bytes;
            break;
        }
    }
    if (dh_mac_key)
    {
        if (typeof envelope.tag != "string")
        {
            return "";
        }
        let expected_tag = utils__bytesToBase64String(keys___hmac_sha256(dh_mac_key, aesjs.utils.utf8.toBytes(envelope.iv + envelope.data)));
        if (expected_tag !== envelope.tag)
        {
            return "";
        }
    }

    try
    {
        let message_iv = utils__base64StringToBytes(envelope.iv);
        let current = new Uint8Array(utils__base64StringToBytes(envelope.data));
        if (message_iv.length !== 16)
        {
            return "";
        }
        // remove connection encryption first, then configured keys in reverse order
        for (let i = g_metadata_keys.length - 1; i >= 0; i--)
        {
            if (g_metadata_keys[i].session_layer || g_metadata_keys[i].mac_bytes)
            {
                current = keys__crypt_metadata_layer(g_metadata_keys[i], message_iv, current, false);
            }
        }
        for (let i = g_metadata_keys.length - 1; i >= 0; i--)
        {
            if (!g_metadata_keys[i].session_layer && !g_metadata_keys[i].mac_bytes)
            {
                current = keys__crypt_metadata_layer(g_metadata_keys[i], message_iv, current, false);
            }
        }

        return utils__substringByNullTerminator(new TextDecoder().decode(current));
    }
    catch (exception)
    {
        return "";
    }
}

// ---- channel keys ----

/**
 * @brief starts the channel key wait timer when another maintainer owns delivery
 *
 * @return void
 */
function keys__arm_maintainer_keys_wait_timer()
{
    // only the connection owner may vote to replace the maintainer
    if (android_host__is_ui_only_runtime())
    {
        return;
    }

    let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

    keys__cancel_maintainer_keys_wait_timer();

    if (channel_index == -1)
    {
        return;
    }

    if (!g_channel_list[channel_index].has_maintainer)
    {
        return;
    }

    if (g_channel_list[channel_index].maintainer_id == g_local_client_id)
    {
        return;
    }

    maintainer_keys_wait_channel_id = g_current_channel_id;
    maintainer_keys_wait_timer = setTimeout(keys__maintainer_keys_wait_timer_fired, MAINTAINER_KEYS_WAIT_TIMEOUT_MS);
}

/**
 * @brief stops and clears the channel key wait timer
 *
 * @return void
 */
function keys__cancel_maintainer_keys_wait_timer()
{
    if (maintainer_keys_wait_timer != null)
    {
        clearTimeout(maintainer_keys_wait_timer);
        maintainer_keys_wait_timer = null;
    }
}

/**
 * @brief rechecks channel state and repeats the reset vote while keys are missing
 *
 * @return void
 */
function keys__maintainer_keys_wait_timer_fired()
{
    maintainer_keys_wait_timer = null;

    if (maintainer_keys_wait_channel_id != g_current_channel_id)
    {
        return;
    }

    if (g_current_channel_keys != null)
    {
        return;
    }

    let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

    if (channel_index == -1)
    {
        return;
    }

    if (!g_channel_list[channel_index].has_maintainer)
    {
        return;
    }

    if (g_channel_list[channel_index].maintainer_id == g_local_client_id)
    {
        return;
    }

    console.log("%c no channel keys from maintainer (id " + g_channel_list[channel_index].maintainer_id + ") after " + MAINTAINER_KEYS_WAIT_TIMEOUT_MS + " ms, sending reset_channel_maintainer vote", "color: red");

    // the server records one reset vote per client
    let message_object = {
        message: {
            type: "reset_channel_maintainer"
        }
    };

    connection__send_message_object(message_object);

    // repeat while keyless so a lost request does not leave this client waiting
    maintainer_keys_wait_timer = setTimeout(keys__maintainer_keys_wait_timer_fired, MAINTAINER_KEYS_WAIT_TIMEOUT_MS);
}

var channel_key_creation_timer = null;
var channel_key_creation_generation = 0;
var channel_key_creation_members = null;
var channel_key_creation_pending = false;
var channel_key_creation_force_fresh = false;
var channel_key_creation_arrivals = new Set();

/**
 * @brief compares the settings that decide when existing channel keys may be kept
 *
 * @param object policy -> the server's announced settings
 *
 * @return string the settings in a fixed order
 */
function keys__channel_key_reuse_policy_signature(policy)
{
    return JSON.stringify([policy.is_channel_key_reuse_enabled, policy.is_channel_key_reuse_on_join_allowed, policy.is_channel_key_reuse_on_leave_allowed, policy.channel_key_reuse_join_min_clients, policy.channel_key_reuse_leave_min_clients]);
}

/**
 * @brief checks whether the server allows keeping keys for this change and channel size
 *
 * @param string event -> "join" or "leave"
 * @param number count -> active people remaining in the channel after the change
 *
 * @return boolean true when this change may keep the existing keys
 */
function keys__may_reuse_channel_keys(event, count)
{
    let policy = typeof g_server_policy === "undefined" ? {} : g_server_policy;
    let threshold = policy["channel_key_reuse_" + event + "_min_clients"];
    return policy.is_channel_key_reuse_enabled === true && policy["is_channel_key_reuse_on_" + event + "_allowed"] === true && Number.isSafeInteger(threshold) && threshold >= 30 && count >= threshold;
}

/**
 * @brief counts people who need keys, excluding idle clients and server music bots
 *
 * @return number active people in this client's channel
 */
function keys__current_channel_member_count()
{
    return g_client_list.filter(function(client)
    {
        return client.channel_id === g_current_channel_id && !client.is_idle && !client.is_music_bot;
    }).length;
}

/**
 * @brief gives an arrival the current keys, or requests fresh keys for everyone when required
 *
 * @param object client -> the person who has just entered the channel
 */
function keys__channel_member_joined(client)
{
    let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);
    if (client == null || client.client_id === g_local_client_id || client.is_idle || client.is_music_bot || client.channel_id !== g_current_channel_id || index < 0)
    {
        return;
    }
    if (!keys__may_reuse_channel_keys("join", keys__current_channel_member_count()))
    {
        keys__set_current_channel_keys(null);
    }
    if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id === g_local_client_id)
    {
        keys__create_and_send_new_channel_keys(client.client_id);
    }
    else if (g_current_channel_keys == null)
    {
        keys__arm_maintainer_keys_wait_timer();
    }
}

/**
 * @brief replaces keys after a departure unless the server permits keeping them
 *
 * @param object client -> the departing person, with their previous channel id
 */
function keys__channel_member_left(client)
{
    let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);
    if (client == null || client.is_music_bot || client.is_idle || client.channel_id !== g_current_channel_id || index < 0)
    {
        return;
    }
    if (client.client_id === g_local_client_id || client.client_id === g_channel_list[index].maintainer_id)
    {
        keys__cancel_channel_key_creation();
        keys__set_current_channel_keys(null);
        return;
    }
    if (keys__may_reuse_channel_keys("leave", keys__current_channel_member_count()))
    {
        return;
    }
    keys__cancel_channel_key_creation();
    keys__set_current_channel_keys(null);
    if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id === g_local_client_id)
    {
        keys__create_and_send_new_channel_keys();
    }
    else
    {
        keys__arm_maintainer_keys_wait_timer();
    }
}


/**
 * @brief stops unfinished channel key work when its member list is no longer current
 *
 * @return void
 */
function keys__cancel_channel_key_creation()
{
    channel_key_creation_generation++;
    clearTimeout(channel_key_creation_timer);
    channel_key_creation_timer = null;
    channel_key_creation_members = null;
    channel_key_creation_pending = false;
    channel_key_creation_force_fresh = false;
    channel_key_creation_arrivals.clear();
    if (g_data_processing_worker != null)
    {
        g_data_processing_worker.postMessage({ type: "mainthread__cancel_channel_key_creation" });
    }
}

/**
 * @brief checks that a key result still belongs to this channel and its current members
 *
 * @param object result -> the worker's result, including its generation and channel id
 *
 * @return boolean true when this client should still distribute these keys
 */
function keys__is_channel_key_creation_current(result)
{
    let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);
    let local_client = channel_tree__get_client_by_client_id(g_local_client_id);
    return g_is_authenticated && result.generation === channel_key_creation_generation && result.channel_id === g_current_channel_id && index >= 0 && local_client != null && !local_client.is_idle && g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id === g_local_client_id && (result.maintainer_generation == null || result.maintainer_generation === g_channel_list[index].maintainer_generation);
}

/**
 * @brief stops distributing keys if a recipient has left or changed identity
 *
 * @return void
 */
function keys__check_channel_key_recipients()
{
    let client = null;
    let request = { generation: channel_key_creation_generation, channel_id: g_current_channel_id };
    if (channel_key_creation_members == null)
    {
        return;
    }
    for (let recipient of channel_key_creation_members)
    {
        client = channel_tree__get_client_by_client_id(recipient.client_id);
        if (client == null || client.is_idle || client.channel_id !== recipient.channel_id || client.public_key !== recipient.public_key)
        {
            if ((client == null || client.is_idle || client.channel_id !== recipient.channel_id) && keys__may_reuse_channel_keys("leave", keys__current_channel_member_count()))
            {
                continue;
            }
            keys__cancel_channel_key_creation();
            keys__set_current_channel_keys(null);
            request.generation = channel_key_creation_generation;
            if (keys__is_channel_key_creation_current(request))
            {
                keys__create_and_send_new_channel_keys();
            }
            return;
        }
    }
}

/**
 * @brief starts one more distribution if people joined while the previous one was being sent
 *
 * @return void
 */
function keys__finish_channel_key_creation()
{
    channel_key_creation_members = null;
    if (channel_key_creation_pending)
    {
        keys__start_pending_channel_key_creation();
    }
}

/**
 * @brief queues a new member for keys or creates a fresh set for the channel
 *
 * @param number|null joining_client_id -> the arrival's id. null requires fresh keys for everyone
 */
function keys__create_and_send_new_channel_keys(joining_client_id = null)
{
    if (android_host__is_ui_only_runtime())
    {
        return;
    }
    channel_key_creation_pending = true;
    if (joining_client_id == null)
    {
        channel_key_creation_force_fresh = true;
    }
    else
    {
        channel_key_creation_arrivals.add(joining_client_id);
        if (!keys__may_reuse_channel_keys("join", keys__current_channel_member_count()))
        {
            channel_key_creation_force_fresh = true;
        }
    }
    keys__start_pending_channel_key_creation();
}

/**
 * @brief combines nearby arrivals and waits for an unfinished distribution before starting another
 */
function keys__start_pending_channel_key_creation()
{
    if (channel_key_creation_timer != null || channel_key_creation_members != null)
    {
        return;
    }
    // more arrivals must not keep restarting the wait, or the keys may never be sent
    channel_key_creation_timer = setTimeout(function()
    {
        let request =
        {
            type: "mainthread__create_websocket_channel_keys_message",
            generation: ++channel_key_creation_generation,
            channel_id: g_current_channel_id,
            local_client_id: g_local_client_id,
            current_channel_id: g_current_channel_id
        };
        channel_key_creation_timer = null;
        channel_key_creation_pending = false;
        if (!keys__is_channel_key_creation_current(request))
        {
            channel_key_creation_force_fresh = false;
            channel_key_creation_arrivals.clear();
            return;
        }
        request.maintainer_generation = g_channel_list[ channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, request.channel_id)].maintainer_generation;
        request.clients = g_client_list.filter(function(client)
        {
            return client.channel_id === request.channel_id && !client.is_idle && !client.is_music_bot;
        }).map(function(client)
        {
            return { client_id: client.client_id, username: client.username,
                channel_id: client.channel_id, public_key: client.public_key };
        });
        request.reuse_existing_keys = !channel_key_creation_force_fresh && g_current_channel_keys != null && keys__may_reuse_channel_keys("join", request.clients.length);
        request.arriving_client_ids = Array.from(channel_key_creation_arrivals);
        channel_key_creation_force_fresh = false;
        channel_key_creation_arrivals.clear();
        channel_key_creation_members = request.clients;
        g_data_processing_worker.postMessage(request);
    }, 100);
}

// media connection encryption
// channel encryption protects the content from the server. connection encryption
// protects the sender fields and authenticates each frame with a counter and MAC
// the server removes the sender's connection layer and applies each receiver's layer
// see crypto_media.c for the server format and counter checks
// 
// wire frame: [1 byte version][8 bytes counter][encrypted body][8 bytes MAC]
// body: [4 bytes sender ID][8 bytes sender counter][inner ciphertext]
// inner counter: [4 bytes sender ID][8 bytes sender counter][4 zero bytes], XOR with the key IV
// connection counter: [8 bytes counter][1 byte direction][1 byte stream][6 zero bytes]
var MEDIA_FRAME_VERSION = 2;
var MEDIA_FRAME_HEADER = 9;
var MEDIA_BODY_HEADER = 12;
var MEDIA_FRAME_CODE = 8;
var MEDIA_FRAME_OVERHEAD = MEDIA_FRAME_HEADER + MEDIA_BODY_HEADER + MEDIA_FRAME_CODE;
var MEDIA_STREAM_AUDIO = 0;
var MEDIA_STREAM_VIDEO = 1;
var MEDIA_DIRECTION_TO_SERVER = 0;
var MEDIA_DIRECTION_FROM_SERVER = 1;

/**
 * @brief turns a hex text into bytes
 *
 * @param string hex -> two hex digits per byte
 *
 * @return Uint8Array the bytes
 */
function keys__hex_to_bytes(hex)
{
    let bytes = new Uint8Array(hex.length / 2);
    for (let i = 0; i < bytes.length; i++)
    {
        bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
    }
    return bytes;
}

/**
 * @brief turns bytes into a hex text
 *
 * @param array bytes -> plain or typed
 *
 * @return string two hex digits per byte
 */
function keys__bytes_to_hex(bytes)
{
    let hex = "";
    for (let i = 0; i < bytes.length; i++)
    {
        hex += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16);
    }
    return hex;
}

/**
 * @brief accepts media keys from the Android Node owner
 *
 * @param string encryption_key_hex -> legacy connection key as 64 hex digits
 * @param string checksum_key_hex -> legacy MAC key as 64 hex digits
 * @param array server_key_hexes -> server keys as hex strings or typed key entries
 * @param object|null session -> negotiated directional keys, or null for the legacy pair
 *
 * @return void
 */
function keys__set_media_keys_from_host(encryption_key_hex, checksum_key_hex, server_key_hexes = [], session = null)
{
    let key_pattern = /^[0-9a-f]{64}$/i;
    if (typeof encryption_key_hex !== "string" || typeof checksum_key_hex !== "string" || !key_pattern.test(encryption_key_hex) || !key_pattern.test(checksum_key_hex) || !Array.isArray(server_key_hexes))
    {
        return;
    }
    let server_layers = [];
    for (let i = 0; i < server_key_hexes.length; i++)
    {
        let entry = typeof server_key_hexes[i] === "string" ? { type: "aes256", key: server_key_hexes[i] } : server_key_hexes[i];
        if (entry == null || CONNECTION_CIPHERS.indexOf(entry.type) < 0 || !key_pattern.test(entry.key))
        {
            return;
        }
        server_layers.push(
        {
            key_bytes: keys__hex_to_bytes(entry.key),
            key_type: entry.type,
            iv_bytes: null
        });
    }
    g_media_keys_from_host =
    {
        key_bytes: keys__hex_to_bytes(encryption_key_hex),
        mac_bytes: keys__hex_to_bytes(checksum_key_hex)
    };
    if (session != null && Array.isArray(session.layers) && session.layers.length >= 1 && session.layers.length <= 9)
    {
        for (let layer of session.layers)
        {
            if (CONNECTION_CIPHERS.indexOf(layer.key_type || "aes256") < 0 || !keys__valid_byte_array(layer.key_bytes, 32) || (layer.receive_key_bytes != null && !keys__valid_byte_array(layer.receive_key_bytes, 32)))
            {
                return;
            }
        }
        if (!keys__valid_byte_array(session.mac_bytes, 32) || (session.receive_mac_bytes != null && !keys__valid_byte_array(session.receive_mac_bytes, 32)))
        {
            return;
        }
        g_media_keys_from_host = session;
    }
    g_media_server_key_layers_from_host = server_layers;
}

/**
 * @brief reads the connection keys used to protect media frames
 *
 * @return object { key_bytes, mac_bytes } or null before the key exchange
 */
function keys__media_session_layer()
{
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes)
        {
            let first = g_metadata_keys[i];
            return { key_bytes: first.key_bytes, mac_bytes: first.mac_bytes,
                receive_mac_bytes: first.receive_mac_bytes,
                layers: g_metadata_keys.filter(function(layer)
                {
                    return layer.session_layer || layer.mac_bytes;
                }) };
        }
    }
    return g_media_keys_from_host;
}

/**
 * @brief returns configured server keys used inside media frames
 *
 * @return array [{ key_bytes, iv_bytes: null }]
 */
function keys__media_server_key_layers()
{
    if (g_media_server_key_layers_from_host != null)
    {
        return g_media_server_key_layers_from_host;
    }
    let layers = [];
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (!g_metadata_keys[i].session_layer && !g_metadata_keys[i].mac_bytes)
        {
            layers.push({ key_bytes: g_metadata_keys[i].key_bytes, key_type: g_metadata_keys[i].key_type || "aes256", iv_bytes: null });
        }
    }
    return layers;
}

/**
 * @brief creates a random starting counter for a media session
 *
 * @return Uint8Array 8 bytes
 */
function keys__counter_create()
{
    let counter = new Uint8Array(8);
    crypto.getRandomValues(counter);
    return counter;
}

/**
 * @brief increments a frame counter with its least significant byte last
 *
 * @param Uint8Array counter -> 8 bytes
 *
 * @return void
 */
function keys__counter_increment(counter)
{
    for (let i = 7; i >= 0; i--)
    {
        counter[i] = (counter[i] + 1) & 0xff;
        if (counter[i] !== 0)
        {
            return;
        }
    }
}

/**
 * @brief combines two counter blocks using XOR
 *
 * @param array left -> 16 bytes or null
 * @param array right -> 16 bytes or null
 *
 * @return Uint8Array the combined block
 */
function keys__media_xor_blocks(left, right)
{
    let block = new Uint8Array(16);
    for (let i = 0; i < 16; i++)
    {
        block[i] = ((left != null ? left[i] : 0) ^ (right != null ? right[i] : 0)) & 0xff;
    }
    return block;
}

/**
 * @brief builds an inner counter from the sender, counter and optional key IV
 *
 * @param number sender_id -> the sending client's id
 * @param Uint8Array counter_bytes -> the sender counter, 8 bytes
 * @param array layer_iv -> the key's iv, 16 bytes, or null
 *
 * @return Uint8Array 16 bytes
 */
function keys__media_inner_block(sender_id, counter_bytes, layer_iv)
{
    let block = new Uint8Array(16);
    block[0] = sender_id & 0xff;
    block[1] = (sender_id >> 8) & 0xff;
    block[2] = (sender_id >> 16) & 0xff;
    block[3] = (sender_id >> 24) & 0xff;
    block.set(counter_bytes, 4);
    return keys__media_xor_blocks(block, layer_iv);
}

/**
 * @brief builds a connection counter for the stream and direction
 *
 * @param Uint8Array counter_bytes -> the frame counter, 8 bytes
 * @param number direction -> MEDIA_DIRECTION_TO_SERVER or MEDIA_DIRECTION_FROM_SERVER
 * @param number stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 *
 * @return Uint8Array 16 bytes
 */
function keys__media_session_block(counter_bytes, direction, stream)
{
    let block = new Uint8Array(16);
    block.set(counter_bytes, 0);
    block[8] = direction;
    block[9] = stream;
    return block;
}

/**
 * @brief applies AES CTR to encrypt or decrypt bytes
 *
 * @param array key_bytes -> the 32 byte key
 * @param Uint8Array block -> the 16 byte counter block to start from
 * @param Uint8Array data -> the bytes
 *
 * @return Uint8Array the result, a new array
 */
function keys__aes_ctr(key_bytes, block, data)
{
    return new aesjs.ModeOfOperation.ctr(key_bytes, new aesjs.Counter(Array.from(block))).encrypt(data);
}

/**
 * @brief applies channel, bot or server encryption layers to media bytes
 *
 * @param array layers -> [{ key_bytes, iv_bytes }], iv_bytes may be null
 * @param number sender_id -> the sending client's id, part of every counter block
 * @param Uint8Array counter_bytes -> the sender counter, part of every counter block
 * @param Uint8Array data -> the bytes
 * @param boolean is_encrypt -> true applies the layers first to last, false last to first
 *
 * @return Uint8Array the result
 */
function keys__media_apply_inner_layers(layers, sender_id, counter_bytes, data, is_encrypt)
{
    let current = new Uint8Array(data);
    for (let step = 0; step < layers.length; step++)
    {
        let i = is_encrypt ? step : (layers.length - 1 - step);
        current = keys__crypt_metadata_layer({ key_type: layers[i].key_type || "aes256", key_bytes: layers[i].key_bytes }, keys__media_inner_block(sender_id, counter_bytes, layers[i].iv_bytes), current, is_encrypt);
    }
    return current;
}

/**
 * @brief prefixes the inner ciphertext with its sender and counter
 *
 * @param number sender_id -> sending client ID
 * @param Uint8Array counter_bytes -> sender counter, 8 bytes
 * @param Uint8Array inner -> encrypted media content
 *
 * @return Uint8Array sender fields followed by the inner ciphertext
 */
function keys__media_build_body(sender_id, counter_bytes, inner)
{
    let body = new Uint8Array(MEDIA_BODY_HEADER + inner.length);
    body[0] = sender_id & 0xff;
    body[1] = (sender_id >> 8) & 0xff;
    body[2] = (sender_id >> 16) & 0xff;
    body[3] = (sender_id >> 24) & 0xff;
    body.set(counter_bytes, 4);
    body.set(inner, MEDIA_BODY_HEADER);
    return body;
}

/**
 * @brief encrypts a media body for the connection and appends its MAC
 *
 * @param object session_layer -> { key_bytes, mac_bytes } from keys__media_session_layer
 * @param number stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param Uint8Array counter_bytes -> the frame counter, 8 bytes, increased before every frame
 * @param Uint8Array body -> from keys__media_build_body
 *
 * @return Uint8Array the frame ready to send, or null without keys
 */
function keys__media_encrypt_frame(session_layer, stream, counter_bytes, body)
{
    if (session_layer == null || body.length < MEDIA_BODY_HEADER)
    {
        return null;
    }
    let frame = new Uint8Array(MEDIA_FRAME_HEADER + body.length + MEDIA_FRAME_CODE);
    frame[0] = MEDIA_FRAME_VERSION;
    frame.set(counter_bytes, 1);
    frame.set(keys__media_session_crypt(session_layer, keys__media_session_block(counter_bytes, MEDIA_DIRECTION_TO_SERVER, stream), body, true), MEDIA_FRAME_HEADER);
    let code = keys___hmac_sha256(session_layer.mac_bytes, frame.subarray(0, MEDIA_FRAME_HEADER + body.length));
    frame.set(code.slice(0, MEDIA_FRAME_CODE), MEDIA_FRAME_HEADER + body.length);
    return frame;
}

/**
 * @brief authenticates a received media frame and removes its connection encryption
 *
 * @param object session_layer -> { key_bytes, mac_bytes } from keys__media_session_layer
 * @param number stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param Uint8Array frame -> the frame as received
 *
 * @return object { sender_id, counter_bytes, inner } or null when the frame is not a valid frame of this client
 */
function keys__media_decrypt_frame(session_layer, stream, frame)
{
    if (session_layer == null || !(frame instanceof Uint8Array) || frame.length <= MEDIA_FRAME_OVERHEAD || frame[0] !== MEDIA_FRAME_VERSION)
    {
        return null;
    }
    let code_offset = frame.length - MEDIA_FRAME_CODE;
    let expected = keys___hmac_sha256(session_layer.receive_mac_bytes || session_layer.mac_bytes, frame.subarray(0, code_offset));
    let difference = 0;
    for (let i = 0; i < MEDIA_FRAME_CODE; i++)
    {
        difference |= expected[i] ^ frame[code_offset + i];
    }
    if (difference !== 0)
    {
        return null;
    }
    let body = keys__media_session_crypt(session_layer, keys__media_session_block(frame.subarray(1, MEDIA_FRAME_HEADER), MEDIA_DIRECTION_FROM_SERVER, stream), frame.subarray(MEDIA_FRAME_HEADER, code_offset), false);
    return {
        sender_id: (body[0] | (body[1] << 8) | (body[2] << 16) | (body[3] << 24)) >>> 0,
        counter_bytes: body.slice(4, MEDIA_BODY_HEADER),
        inner: body.subarray(MEDIA_BODY_HEADER)
    };
}

/**
 * @brief applies the negotiated connection layers for an audio or video frame
 *
 * @param object session -> connection keys and optional directional layers
 * @param Uint8Array block -> counter block, 16 bytes
 * @param Uint8Array data -> input bytes
 * @param boolean encrypt -> true to encrypt outgoing data, false to decrypt received data
 *
 * @return Uint8Array the transformed bytes
 */
function keys__media_session_crypt(session, block, data, encrypt)
{
    let layers = session.layers || [{ key_type: "aes256", key_bytes: session.key_bytes }];
    let result = data;
    for (let index = 0; index < layers.length; index++)
    {
        let selected = encrypt ? index : layers.length - index - 1;
        result = keys__crypt_metadata_layer(layers[selected], block, result, encrypt);
    }
    return result;
}

/**
 * @brief checks that a plain array contains the expected number of bytes
 *
 * @param any value -> value to validate
 * @param number length -> required byte count
 *
 * @return boolean -> true for an array of integer bytes with the required length
 */
function keys__valid_byte_array(value, length)
{
    return Array.isArray(value) && value.length === length && value.every(function(byte)
    {
        return Number.isInteger(byte) && byte >= 0 && byte <= 255;
    });
}
